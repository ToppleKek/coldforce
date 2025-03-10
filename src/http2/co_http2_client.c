#include <coldforce/core/co_std.h>
#include <coldforce/core/co_string.h>

#include <coldforce/net/co_net_addr_resolve.h>
#include <coldforce/net/co_byte_order.h>

#include <coldforce/tls/co_tls_tcp_client.h>

#include <coldforce/http/co_base64.h>
#include <coldforce/http/co_http_server.h>

#include <coldforce/http2/co_http2_client.h>
#include <coldforce/http2/co_http2_stream.h>
#include <coldforce/http2/co_http2_frame.h>
#include <coldforce/http2/co_http2_log.h>

//---------------------------------------------------------------------------//
// http2 client
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
// private
//---------------------------------------------------------------------------//

void
co_http2_server_on_tcp_receive_ready(
    co_thread_t* thread,
    co_tcp_client_t* tcp_client
);

void
co_http2_client_setup(
    co_http2_client_t* client
)
{
    client->callbacks.on_connect = NULL;
    client->callbacks.on_close = NULL;
    client->callbacks.on_receive_start = NULL;
    client->callbacks.on_receive_finish = NULL;
    client->callbacks.on_receive_data = NULL;
    client->callbacks.on_push_request = NULL;
    client->callbacks.on_push_start = NULL;
    client->callbacks.on_push_finish = NULL;
    client->callbacks.on_push_data = NULL;
    client->callbacks.on_priority = NULL;
    client->callbacks.on_window_update = NULL;
    client->callbacks.on_close_stream = NULL;
    client->callbacks.on_ping = NULL;

    co_map_ctx_st map_ctx = { 0 };
    map_ctx.destroy_value =
        (co_item_destroy_fn)co_http2_stream_destroy;

    client->stream_map = co_map_create(&map_ctx);

    client->last_stream_id = 0;
    client->new_stream_id = 0;

    client->local_settings.header_table_size =
        CO_HTTP2_SETTING_DEFAULT_HEADER_TABLE_SIZE;
    client->local_settings.enable_push =
        CO_HTTP2_SETTING_DEFAULT_ENABLE_PUSH;
    client->local_settings.max_concurrent_streams =
        CO_HTTP2_SETTING_DEFAULT_MAX_CONCURRENT_STREAMS;
    client->local_settings.initial_window_size =
        CO_HTTP2_SETTING_DEFAULT_INITIAL_WINDOW_SIZE;
    client->local_settings.max_frame_size =
        CO_HTTP2_SETTING_DEFAULT_MAX_FRAME_SIZE;
    client->local_settings.max_header_list_size =
        CO_HTTP2_SETTING_DEFAULT_MAX_HEADER_LIST_SIZE;
    client->local_settings.enable_conncet_protocol =
        CO_HTTP2_SETTING_DEFAULT_ENABLE_CONNECT_PROTOCOL;

    client->remote_settings.header_table_size =
        CO_HTTP2_SETTING_DEFAULT_HEADER_TABLE_SIZE;
    client->remote_settings.enable_push =
        CO_HTTP2_SETTING_DEFAULT_ENABLE_PUSH;
    client->remote_settings.max_concurrent_streams =
        CO_HTTP2_SETTING_DEFAULT_MAX_CONCURRENT_STREAMS;
    client->remote_settings.initial_window_size =
        CO_HTTP2_SETTING_DEFAULT_INITIAL_WINDOW_SIZE;
    client->remote_settings.max_frame_size =
        CO_HTTP2_SETTING_DEFAULT_MAX_FRAME_SIZE;
    client->remote_settings.max_header_list_size =
        CO_HTTP2_SETTING_DEFAULT_MAX_HEADER_LIST_SIZE;
    client->remote_settings.enable_conncet_protocol =
        CO_HTTP2_SETTING_DEFAULT_ENABLE_CONNECT_PROTOCOL;

    co_http2_hpack_dynamic_table_setup(
        &client->local_dynamic_table,
        client->local_settings.max_header_list_size);
    co_http2_hpack_dynamic_table_setup(
        &client->remote_dynamic_table,
        client->remote_settings.max_header_list_size);

    client->system_stream =
        co_http2_stream_create(0, client, NULL, NULL, NULL);
}

void
co_http2_client_cleanup(
    co_http2_client_t* client
)
{
    if (client != NULL)
    {
        co_byte_array_destroy(client->conn.receive_data.ptr);
        client->conn.receive_data.ptr = NULL;

        co_http2_stream_destroy(client->system_stream);
        client->system_stream = NULL;

        co_map_destroy(client->stream_map);
        client->stream_map = NULL;

        co_http2_hpack_dynamic_table_cleanup(&client->local_dynamic_table);
        co_http2_hpack_dynamic_table_cleanup(&client->remote_dynamic_table);
    }
}

static void
co_http2_set_setting_param(
    co_http2_settings_st* settings,
    uint16_t identifier,
    uint32_t value
)
{
    switch (identifier)
    {
    case CO_HTTP2_SETTING_ID_HEADER_TABLE_SIZE:
    {
        settings->header_table_size = value;

        break;
    }
    case CO_HTTP2_SETTING_ID_ENABLE_PUSH:
    {
        settings->enable_push = value;

        break;
    }
    case CO_HTTP2_SETTING_ID_MAX_CONCURRENT_STREAMS:
    {
        settings->max_concurrent_streams = value;

        break;
    }
    case CO_HTTP2_SETTING_ID_INITIAL_WINDOW_SIZE:
    {
        settings->initial_window_size = value;

        break;
    }
    case CO_HTTP2_SETTING_ID_MAX_FRAME_SIZE:
    {
        settings->max_frame_size = value;

        break;
    }
    case CO_HTTP2_SETTING_ID_MAX_HEADER_LIST_SIZE:
    {
        settings->max_header_list_size = value;

        break;
    }
    case CO_HTTP2_SETTING_ID_ENABLE_CONNECT_PROTOCOL:
    {
        settings->enable_conncet_protocol = value;

        break;
    }
    default:
        break;
    }
}

co_http2_stream_t*
co_http2_create_stream(
    co_http2_client_t* client
)
{
    client->new_stream_id += 2;

    co_http2_stream_t* stream = co_http2_stream_create(
        client->new_stream_id, client,
        client->callbacks.on_receive_start,
        client->callbacks.on_receive_finish,
        client->callbacks.on_receive_data);

    co_map_set(client->stream_map,
        (void*)(uintptr_t)client->new_stream_id, stream);

    return stream;
}

void
co_http2_destroy_stream(
    co_http2_client_t* client,
    co_http2_stream_t* stream
)
{
    if (client != NULL &&
        client->stream_map != NULL &&
        stream != NULL)
    {
        co_map_remove(
            client->stream_map, (void*)(uintptr_t)stream->id);
    }
}

void
co_http2_client_on_close(
    co_http2_client_t* client,
    int error_code
)
{
    client->conn.module.close(client->conn.tcp_client);

    if (client->callbacks.on_close != NULL)
    {
        client->callbacks.on_close(
            client->conn.tcp_client->sock.owner_thread,
            client, error_code);
    }
}

void
co_http2_client_on_receive_system_frame(
    co_http2_client_t* client,
    const co_http2_frame_t* frame
)
{
    switch (frame->header.type)
    {
    case CO_HTTP2_FRAME_TYPE_SETTINGS:
    {
        if (frame->header.flags != 0)
        {
            if (client->callbacks.on_connect != NULL)
            {
                co_http2_connect_fn handler = client->callbacks.on_connect;
                client->callbacks.on_connect = NULL;

                handler(
                    client->conn.tcp_client->sock.owner_thread,
                    client, 0);
            }

            return;
        }

        for (size_t index = 0;
            index < frame->payload.settings.param_count;
            ++index)
        {
            co_http2_set_setting_param(
                &client->remote_settings,
                frame->payload.settings.params[index].id,
                frame->payload.settings.params[index].value);
        }

        co_http2_frame_t* ack_frame =
            co_http2_create_settings_frame(false, true, NULL, 0);

        co_http2_stream_send_frame(
            client->system_stream, ack_frame);

        break;
    }
    case CO_HTTP2_FRAME_TYPE_PING:
    {
        if (frame->header.flags & CO_HTTP2_FRAME_FLAG_ACK)
        {
            if (client->callbacks.on_ping != NULL)
            {
                client->callbacks.on_ping(
                    client->conn.tcp_client->sock.owner_thread,
                    client, frame->payload.ping.opaque_data);
            }
        }
        else
        {
            co_http2_frame_t* ack_frame =
                co_http2_create_ping_frame(true,
                    frame->payload.ping.opaque_data);

            co_http2_stream_send_frame(
                client->system_stream, ack_frame);
        }

        break;
    }
    case CO_HTTP2_FRAME_TYPE_GOAWAY:
    {
        co_http2_client_on_close(
            (co_http2_client_t*)client->conn.tcp_client->sock.sub_class,
            CO_HTTP2_ERROR_STREAM_CLOSED - frame->payload.goaway.error_code);

        break;
    }
    case CO_HTTP2_FRAME_TYPE_WINDOW_UPDATE:
    {
        if ((client->system_stream->remote_window_size +
            frame->payload.window_update.window_size_increment) >
            CO_HTTP2_SETTING_MAX_WINDOW_SIZE)
        {
            co_http2_close(
                client, CO_HTTP2_STREAM_ERROR_FLOW_CONTROL_ERROR);
            co_http2_client_on_close(
                client, CO_HTTP2_STREAM_ERROR_FLOW_CONTROL_ERROR);

            break;
        }

        client->system_stream->remote_window_size +=
            frame->payload.window_update.window_size_increment;

        if (client->callbacks.on_window_update != NULL)
        {
            client->callbacks.on_window_update(
                client->conn.tcp_client->sock.owner_thread,
                client, client->system_stream);
        }

        break;
    }
    default:
    {
        break;
    }
    }
}

bool
co_http2_client_on_push_promise(
    co_http2_client_t* client,
    co_http2_stream_t* stream,
    uint32_t promised_id,
    co_http2_header_t* header
)
{
    co_http2_stream_t* promised_stream =
        co_http2_stream_create(
            promised_id, client,
            client->callbacks.on_push_start,
            client->callbacks.on_push_finish,
            client->callbacks.on_push_data);

    co_map_set(client->stream_map,
        (void*)(uintptr_t)promised_id, promised_stream);

    if (client->last_stream_id < promised_id)
    {
        client->last_stream_id = promised_id;
    }

    promised_stream->send_header = header;
    promised_stream->state = CO_HTTP2_STREAM_STATE_RESERVED_REMOTE;

    if (client->callbacks.on_push_request != NULL)
    {
        if (client->callbacks.on_push_request(
            client->conn.tcp_client->sock.owner_thread,
            client, stream, promised_stream, header))
        {
            return (client->conn.tcp_client != NULL);
        }

        if (client->conn.tcp_client == NULL)
        {
            return false;
        }
    }

    co_http2_stream_send_rst_stream(
        promised_stream, CO_HTTP2_STREAM_ERROR_CANCEL);

    return true;
}

static void
co_http2_client_on_http_connection_connect(
    co_thread_t* thread,
    co_http_connection_t* conn,
    int error_code
)
{
    co_http2_client_t* client = (co_http2_client_t*)conn;

    if (error_code == 0)
    {
        co_http2_log_info(
            &client->conn.tcp_client->sock.local.net_addr,
            "-->",
            &client->conn.tcp_client->sock.remote.net_addr,
            "http2 send connection preface");

        co_http_connection_send_data(
            &client->conn,
            CO_HTTP2_CONNECTION_PREFACE,
            CO_HTTP2_CONNECTION_PREFACE_LENGTH);

        co_http2_send_initial_settings(client);
    }
    else
    {
        co_http2_log_error(
            &client->conn.tcp_client->sock.local.net_addr,
            "<--",
            &client->conn.tcp_client->sock.remote.net_addr,
            "http2 connect error (%d)",
            error_code);

        if (client->callbacks.on_connect != NULL)
        {
            client->callbacks.on_connect(
                thread, client, error_code);
        }
    }
}

void
co_http2_client_on_http_connection_close(
    co_thread_t* thread,
    co_http_connection_t* conn
)
{
    (void)thread;

    co_http2_client_t* client = (co_http2_client_t*)conn;

    co_http2_client_on_close(client, 0);
}

void
co_http2_client_on_tcp_receive_ready(
    co_thread_t* thread,
    co_tcp_client_t* tcp_client
)
{
    (void)thread;

    co_http2_client_t* client =
        (co_http2_client_t*)tcp_client->sock.sub_class;

    ssize_t receive_result =
        client->conn.module.receive_all(
            client->conn.tcp_client,
            client->conn.receive_data.ptr);

    if (receive_result <= 0)
    {
        return;
    }

    size_t data_size =
        co_byte_array_get_count(client->conn.receive_data.ptr);

    while (data_size > client->conn.receive_data.index)
    {
        co_http2_frame_t* frame = co_http2_frame_create();

        int result = co_http2_frame_deserialize(
            client->conn.receive_data.ptr,
            &client->conn.receive_data.index,
            client->local_settings.max_frame_size, frame);

        co_assert(data_size >= client->conn.receive_data.index);

        if (result == CO_HTTP_PARSE_COMPLETE)
        {
            co_http2_stream_t* stream =
                co_http2_get_stream(client, frame->header.stream_id);

            if ((stream == NULL) ||
                (stream->state == CO_HTTP2_STREAM_STATE_CLOSED))
            {
                co_http2_frame_destroy(frame);

                continue;
            }

            co_http2_log_debug_frame(
                &client->conn.tcp_client->sock.local.net_addr, "<--",
                &client->conn.tcp_client->sock.remote.net_addr,
                frame,
                "http2 receive frame");

            if (frame->header.stream_id == 0)
            {
                co_http2_client_on_receive_system_frame(client, frame);
            }
            else
            {
                if (co_http2_stream_on_receive_frame(stream, frame))
                {
                    if (frame->header.type == CO_HTTP2_FRAME_TYPE_DATA)
                    {
                        co_http2_stream_update_local_window_size(
                            client->system_stream, frame->header.length);
                    }
                }

                if (stream->state == CO_HTTP2_STREAM_STATE_CLOSED)
                {
                    co_http2_destroy_stream(client, stream);
                }
            }

            co_http2_frame_destroy(frame);

            if (client->conn.tcp_client == NULL)
            {
                return;
            }
        }
        else if (result == CO_HTTP_PARSE_MORE_DATA)
        {
            co_http2_frame_destroy(frame);

            return;
        }
        else
        {
            co_http2_frame_destroy(frame);

            co_http2_close(
                client, CO_HTTP2_STREAM_ERROR_FRAME_SIZE_ERROR);
            co_http2_client_on_close(
                client, CO_HTTP2_STREAM_ERROR_FRAME_SIZE_ERROR);

            return;
        }
    }

    client->conn.receive_data.index = 0;
    co_byte_array_clear(client->conn.receive_data.ptr);
}

bool
co_http2_set_upgrade_settings(
    const char* b64_settings,
    size_t b64_settings_length,
    co_http2_settings_st* settings
)
{
    size_t settings_data_size = 0;
    uint8_t* settings_data = NULL;

    bool result = co_base64url_decode(
        b64_settings, b64_settings_length,
        &settings_data, &settings_data_size);

    if (result)
    {
        uint16_t param_count =
            (uint16_t)(settings_data_size /
                (sizeof(uint16_t) + sizeof(uint32_t)));
        const uint8_t* temp_data = settings_data;

        for (uint16_t count = 0; count < param_count; ++count)
        {
            uint16_t identifier;
            uint32_t value;

            memcpy(&identifier, temp_data, sizeof(uint16_t));
            identifier = co_byte_order_16_network_to_host(identifier);
            temp_data += sizeof(uint16_t);

            memcpy(&value, temp_data, sizeof(uint16_t));
            value = co_byte_order_32_network_to_host(value);
            temp_data += sizeof(uint32_t);

            co_http2_set_setting_param(settings, identifier, value);
        }
    }

    co_mem_free(settings_data);

    return result;
}

//---------------------------------------------------------------------------//
// public
//---------------------------------------------------------------------------//

co_http2_client_t*
co_http2_client_create(
    const char* url_origin,
    const co_net_addr_t* local_net_addr,
    co_tls_ctx_st* tls_ctx
)
{
    co_http2_client_t* client =
        (co_http2_client_t*)co_mem_alloc(sizeof(co_http2_client_t));

    if (client == NULL)
    {
        return NULL;
    }

    co_url_st* url = co_url_create(url_origin);

    const char* protocols[1] = { CO_HTTP2_PROTOCOL };

    if (!co_http_connection_setup(
        (co_http_connection_t*)client, url, local_net_addr,
        protocols, 1, tls_ctx))
    {
        co_url_destroy(url);
        co_mem_free(client);

        return NULL;
    }

    client->conn.tcp_client->callbacks.on_receive =
        (co_tcp_receive_fn)co_http2_client_on_tcp_receive_ready;

    client->conn.callbacks.on_connect =
        (co_http_connection_connect_fn)
            co_http2_client_on_http_connection_connect;
    client->conn.callbacks.on_close =
        (co_http_connection_close_fn)
            co_http2_client_on_http_connection_close;

    co_http2_client_setup(client);

    client->new_stream_id = UINT32_MAX;

    return client;
}

void
co_http2_client_destroy(
    co_http2_client_t* client
)
{
    if (client != NULL)
    {
        co_http2_close(client, 0);

        co_http2_client_cleanup(client);
        co_http_connection_cleanup(&client->conn);

        co_mem_free_later(client);
    }
}

co_http2_callbacks_st*
co_http2_get_callbacks(
    co_http2_client_t* client
)
{
    return &client->callbacks;
}

bool
co_http2_is_running(
    const co_http2_client_t* client
)
{
    co_map_iterator_t it;
    co_map_iterator_init(client->stream_map, &it);

    while (co_map_iterator_has_next(&it))
    {
        const co_map_data_st* data = &it.item->data;
        co_http2_stream_t* stream = (co_http2_stream_t*)data->value;
        co_map_iterator_get_next(&it);

        if ((stream->state != CO_HTTP2_STREAM_STATE_CLOSED) &&
            (stream->state != CO_HTTP2_STREAM_STATE_REMOTE_CLOSED))
        {
            return true;
        }
    }

    return false;
}

bool
co_http2_connect_start(
    co_http2_client_t* client
)
{
    return client->conn.module.connect(
        client->conn.tcp_client,
        &client->conn.tcp_client->sock.remote.net_addr);
}

void
co_http2_close(
    co_http2_client_t* client,
    int error_code
)
{
    if ((client != NULL) &&
        (client->conn.tcp_client != NULL) &&
        (co_tcp_is_open(client->conn.tcp_client)))
    {
        if ((client->system_stream != NULL) &&
            (client->system_stream->state !=
                CO_HTTP2_STREAM_STATE_CLOSED))
        {
            co_http2_frame_t* goaway_frame =
                co_http2_create_goaway_frame(
                    false, client->last_stream_id, error_code,
                    NULL, 0);

            co_http2_stream_send_frame(
                client->system_stream, goaway_frame);
        }

        client->conn.module.close(client->conn.tcp_client);
    }
}

co_http2_stream_t*
co_http2_get_stream(
    co_http2_client_t* client,
    uint32_t stream_id
)
{
    co_http2_stream_t* stream = NULL;

    if (stream_id != 0)
    {
        co_map_data_st* data =
            co_map_get(client->stream_map, (void*)(uintptr_t)stream_id);

        if (data != NULL)
        {
            stream = (co_http2_stream_t*)data->value;
        }
    }
    else
    {
        stream = client->system_stream;
    }

    return stream;
}

bool
co_http2_send_initial_settings(
    co_http2_client_t* client
)
{
    uint16_t param_count = 0;
    co_http2_setting_param_st params[CO_HTTP2_SETTING_MAX_SIZE];

    if (client->local_settings.header_table_size !=
        CO_HTTP2_SETTING_DEFAULT_HEADER_TABLE_SIZE)
    {
        params[param_count].id =
            CO_HTTP2_SETTING_ID_HEADER_TABLE_SIZE;
        params[param_count].value =
            client->local_settings.header_table_size;
        ++param_count;
    }

    if (client->local_settings.enable_push !=
        CO_HTTP2_SETTING_DEFAULT_ENABLE_PUSH)
    {
        params[param_count].id =
            CO_HTTP2_SETTING_ID_ENABLE_PUSH;
        params[param_count].value =
            client->local_settings.enable_push;
        ++param_count;
    }

    if (client->local_settings.max_concurrent_streams !=
        CO_HTTP2_SETTING_DEFAULT_MAX_CONCURRENT_STREAMS)
    {
        params[param_count].id =
            CO_HTTP2_SETTING_ID_MAX_CONCURRENT_STREAMS;
        params[param_count].value =
            client->local_settings.max_concurrent_streams;
        ++param_count;
    }

    if (client->local_settings.initial_window_size !=
        CO_HTTP2_SETTING_DEFAULT_INITIAL_WINDOW_SIZE)
    {
        params[param_count].id =
            CO_HTTP2_SETTING_ID_INITIAL_WINDOW_SIZE;
        params[param_count].value =
            client->local_settings.initial_window_size;
        ++param_count;
    }

    if (client->local_settings.max_frame_size !=
        CO_HTTP2_SETTING_DEFAULT_MAX_FRAME_SIZE)
    {
        params[param_count].id =
            CO_HTTP2_SETTING_ID_MAX_FRAME_SIZE;
        params[param_count].value =
            client->local_settings.max_frame_size;
        ++param_count;
    }

    if (client->local_settings.max_header_list_size !=
        CO_HTTP2_SETTING_DEFAULT_MAX_HEADER_LIST_SIZE)
    {
        params[param_count].id =
            CO_HTTP2_SETTING_ID_MAX_HEADER_LIST_SIZE;
        params[param_count].value =
            client->local_settings.max_header_list_size;
        ++param_count;
    }

    if (client->local_settings.enable_conncet_protocol !=
        CO_HTTP2_SETTING_DEFAULT_ENABLE_CONNECT_PROTOCOL)
    {
        params[param_count].id =
            CO_HTTP2_SETTING_ID_ENABLE_CONNECT_PROTOCOL;
        params[param_count].value =
            client->local_settings.enable_conncet_protocol;
        ++param_count;
    }

    co_http2_frame_t* frame =
        co_http2_create_settings_frame(
            false, false, params, param_count);

    bool result = co_http2_stream_send_frame(
        client->system_stream, frame);

    if (result &&
        (client->local_settings.initial_window_size !=
            CO_HTTP2_SETTING_DEFAULT_INITIAL_WINDOW_SIZE))
    {
        result = co_http2_stream_send_window_update(
            client->system_stream,
            client->local_settings.initial_window_size);
    }

    return result;
}

void
co_http2_init_settings(
    co_http2_client_t* client,
    const co_http2_setting_param_st* params,
    uint16_t param_count
)
{
    for (size_t index = 0; index < param_count; ++index)
    {
        co_http2_set_setting_param(
            &client->local_settings,
            params[index].id, params[index].value);
    }
}

void
co_http2_update_settings(
    co_http2_client_t* client,
    const co_http2_setting_param_st* params,
    uint16_t param_count
)
{
    co_http2_frame_t* settings_frame =
        co_http2_create_settings_frame(
            false, false, params, param_count);

    co_http2_stream_send_frame(
        client->system_stream, settings_frame);

    for (size_t index = 0; index < param_count; ++index)
    {
        co_http2_set_setting_param(
            &client->local_settings,
            params[index].id, params[index].value);
    }
}

const co_http2_settings_st*
co_http2_get_local_settings(
    const co_http2_client_t* client
)
{
    return &client->local_settings;
}

const co_http2_settings_st*
co_http2_get_remote_settings(
    const co_http2_client_t* client
)
{
    return &client->remote_settings;
}

co_socket_t*
co_http2_get_socket(
    co_http2_client_t* client
)
{
    return ((client->conn.tcp_client != NULL) ?
        &client->conn.tcp_client->sock : NULL);
}

const char*
co_http2_get_url_origin(
    const co_http2_client_t* client
)
{
    return ((client->conn.url_origin != NULL) ?
        client->conn.url_origin->src : NULL);
}

bool
co_http2_is_open(
    const co_http2_client_t* client
)
{
    return ((client->conn.tcp_client != NULL) ?
        co_tcp_is_open(client->conn.tcp_client) : false);
}

void
co_http2_set_user_data(
    co_http2_client_t* client,
    void* user_data
)
{
    co_tcp_set_user_data(
        client->conn.tcp_client, user_data);
}

void*
co_http2_get_user_data(
    const co_http2_client_t* client
)
{
    return co_tcp_get_user_data(
        client->conn.tcp_client);
}
