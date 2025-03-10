#include <coldforce/core/co_std.h>
#include <coldforce/core/co_string.h>
#include <coldforce/core/co_random.h>

#include <coldforce/net/co_byte_order.h>
#include <coldforce/net/co_net_addr_resolve.h>

#include <coldforce/tls/co_tls_tcp_client.h>

#include <coldforce/http/co_http_log.h>

#include <coldforce/ws/co_ws_client.h>
#include <coldforce/ws/co_ws_http_extension.h>
#include <coldforce/ws/co_ws_log.h>

//---------------------------------------------------------------------------//
// websocket client
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
// private
//---------------------------------------------------------------------------//

void
co_ws_client_setup(
    co_ws_client_t* client
)
{
    client->callbacks.on_connect = NULL;
    client->callbacks.on_upgrade = NULL;
    client->callbacks.on_receive_frame = NULL;
    client->callbacks.on_close = NULL;

    client->upgrade_request = NULL;
    client->mask = false;
    client->closed = false;
}

void
co_ws_client_cleanup(
    co_ws_client_t* client
)
{
    if (client != NULL)
    {
        co_http_request_destroy(client->upgrade_request);
        client->upgrade_request = NULL;
    }
}

void
co_ws_client_on_receive_frame(
    co_thread_t* thread,
    co_ws_client_t* client,
    co_ws_frame_t* frame,
    int error_code
)
{
    if (error_code == CO_WS_ERROR_DATA_TOO_BIG)
    {
        co_ws_send_close(client,
            CO_WS_CLOSE_REASON_DATA_TOO_BIG, NULL);
    }
    else if (error_code != 0)
    {
        co_ws_send_close(client,
            CO_WS_CLOSE_REASON_PROTOCOL_ERROR, NULL);
    }

    if (client->callbacks.on_receive_frame != NULL)
    {
        client->callbacks.on_receive_frame(
            thread, client, frame, error_code);
    }

    co_ws_frame_destroy(frame);
}

static bool
co_ws_client_on_receive_http_response(
    co_thread_t* thread,
    co_ws_client_t* client
)
{
    co_http_response_t* response = co_http_response_create(0, NULL);

    int parse_result =
        co_http_response_deserialize(response,
            client->conn.receive_data.ptr,
            &client->conn.receive_data.index);

    if (parse_result == CO_HTTP_PARSE_MORE_DATA)
    {
        co_http_response_destroy(response);

        return true;
    }
    else if (parse_result != CO_HTTP_PARSE_COMPLETE)
    {
        co_http_response_destroy(response);

        return false;
    }

    co_http_log_debug_response_header(
        &client->conn.tcp_client->sock.local.net_addr,
        "<--",
        &client->conn.tcp_client->sock.remote.net_addr,
        response,
        "http receive response");

    if (client->callbacks.on_upgrade == NULL)
    {
        co_http_response_destroy(response);

        return false;
    }

    int error_code = 0;

    if (!co_http_response_validate_ws_upgrade(
        response, client->upgrade_request))
    {
        error_code = CO_WS_ERROR_UPGRADE_REFUSED;
    }

    co_http_request_t* upgrade_request = client->upgrade_request;
    client->upgrade_request = NULL;

    if (client->callbacks.on_upgrade != NULL)
    {
        co_ws_upgrade_fn handler = client->callbacks.on_upgrade;
        client->callbacks.on_upgrade = NULL;

        handler(thread, client,
            (const co_http_message_t*)response, error_code);
    }

    co_http_request_destroy(upgrade_request);
    co_http_response_destroy(response);

    return true;
}

static void
co_ws_client_on_http_connection_connect(
    co_thread_t* thread,
    co_http_connection_t* conn,
    int error_code
)
{
    co_ws_client_t* client = (co_ws_client_t*)conn;

    if (client->callbacks.on_connect != NULL)
    {
        client->callbacks.on_connect(
            thread, client, error_code);
    }
}

void
co_ws_client_on_http_connection_close(
    co_thread_t* thread,
    co_http_connection_t* conn
)
{
    co_ws_client_t* client = (co_ws_client_t*)conn;

    if (client->callbacks.on_close != NULL)
    {
        client->callbacks.on_close(thread, client);
    }
}

void
co_ws_client_on_tcp_receive_ready(
    co_thread_t* thread,
    co_tcp_client_t* tcp_client
)
{
    co_ws_client_t* client =
        (co_ws_client_t*)tcp_client->sock.sub_class;

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
        if ((data_size - client->conn.receive_data.index) <
            CO_WS_FRAME_HEADER_MIN_SIZE)
        {
            return;
        }

        co_ws_frame_t* frame = co_ws_frame_create();

        int result = co_ws_frame_deserialize(frame,
            co_byte_array_get_ptr(client->conn.receive_data.ptr, 0),
            co_byte_array_get_count(client->conn.receive_data.ptr),
            &client->conn.receive_data.index);

        if (result == CO_WS_PARSE_COMPLETE)
        {
            co_ws_log_debug_frame(
                &client->conn.tcp_client->sock.local.net_addr,
                "<--",
                &client->conn.tcp_client->sock.remote.net_addr,
                frame->header.fin,
                frame->header.opcode,
                frame->payload_data,
                (size_t)frame->header.payload_size,
                "ws receive frame");

            co_ws_client_on_receive_frame(
                thread, client, frame, 0);

            if (client->conn.tcp_client == NULL)
            {
                return;
            }

            continue;
        }
        else if (result == CO_WS_PARSE_MORE_DATA)
        {
            co_ws_frame_destroy(frame);

            return;
        }
        else
        {
            co_ws_frame_destroy(frame);

            if (co_ws_client_on_receive_http_response(thread, client))
            {
                if (client->conn.tcp_client != NULL)
                {
                    continue;
                }
            }
            else if (client->callbacks.on_upgrade != NULL)
            {
                co_ws_upgrade_fn handler = client->callbacks.on_upgrade;
                client->callbacks.on_upgrade = NULL;

                handler(thread, client,
                    NULL, CO_WS_ERROR_INVALID_RESPONSE);
            }
            else
            {
                co_ws_client_on_receive_frame(
                    thread, client, NULL, result);
            }

            return;
        }
    }

    client->conn.receive_data.index = 0;
    co_byte_array_clear(client->conn.receive_data.ptr);
}

//---------------------------------------------------------------------------//
// public
//---------------------------------------------------------------------------//

co_ws_client_t*
co_ws_client_create(
    const char* url_origin,
    const co_net_addr_t* local_net_addr,
    co_tls_ctx_st* tls_ctx
)
{
    if (url_origin == NULL)
    {
        return NULL;
    }

    co_ws_client_t* client =
        (co_ws_client_t*)co_mem_alloc(sizeof(co_ws_client_t));

    if (client == NULL)
    {
        return NULL;
    }

    co_url_st* url = co_url_create(url_origin);

    if (url->host == NULL)
    {
        co_url_destroy(url);
        co_mem_free(client);

        return NULL;
    }

    if (url->scheme == NULL)
    {
        client->conn.url_origin->scheme = co_string_duplicate("http");
    }
    else if (co_string_case_compare(url->scheme, "wss") == 0)
    {
        co_string_destroy(url->scheme);
        url->scheme = co_string_duplicate("https");
    }
    else if (co_string_case_compare(url->scheme, "ws") == 0)
    {
        co_string_destroy(url->scheme);
        url->scheme = co_string_duplicate("http");
    }

    const char* protocols[1] = { CO_HTTP_PROTOCOL };

    if (!co_http_connection_setup(
        (co_http_connection_t*)client, url, local_net_addr,
        protocols, 1, tls_ctx))
    {
        co_url_destroy(url);
        co_mem_free(client);

        return NULL;
    }

    client->conn.tcp_client->callbacks.on_receive =
        (co_tcp_receive_fn)
            co_ws_client_on_tcp_receive_ready;

    client->conn.callbacks.on_connect =
        (co_http_connection_connect_fn)
            co_ws_client_on_http_connection_connect;
    client->conn.callbacks.on_close =
        (co_http_connection_close_fn)
            co_ws_client_on_http_connection_close;

    co_ws_client_setup(client);

    client->mask = true;

    return client;
}

void
co_ws_client_destroy(
    co_ws_client_t* client
)
{
    if (client != NULL)
    {
        if (client->conn.tcp_client != NULL)
        {
            co_ws_send_close(
                client, CO_WS_CLOSE_REASON_NORMAL, NULL);
        }

        co_ws_client_cleanup(client);
        co_http_connection_cleanup(&client->conn);

        co_mem_free_later(client);
    }
}

co_ws_callbacks_st*
co_ws_get_callbacks(
    co_ws_client_t* client
)
{
    return &client->callbacks;
}

bool
co_ws_connect_start(
    co_ws_client_t* client
)
{
    return client->conn.module.connect(
        client->conn.tcp_client,
        &client->conn.tcp_client->sock.remote.net_addr);
}

bool
co_ws_send_upgrade_request(
    co_ws_client_t* client,
    co_http_request_t* upgrade_request
)
{
    if (!co_http_header_contains(
        &upgrade_request->message.header, CO_HTTP_HEADER_HOST))
    {
        co_http_header_add_field(
            &upgrade_request->message.header,
            CO_HTTP_HEADER_HOST,
            client->conn.url_origin->host_and_port);
    }

    client->upgrade_request = upgrade_request;

    return co_http_connection_send_request(
        (co_http_connection_t*)client, upgrade_request);
}

void
co_ws_close(
    co_ws_client_t* client
)
{
    if ((client != NULL) &&
        (client->conn.tcp_client != NULL) &&
        (co_tcp_is_open(client->conn.tcp_client)))
    {
        client->conn.module.close(client->conn.tcp_client);
    }
}

bool
co_ws_send(
    co_ws_client_t* client,
    bool fin,
    uint8_t opcode,
    const void* data,
    size_t data_size
)
{
    co_ws_log_debug_frame(
        &client->conn.tcp_client->sock.local.net_addr,
        "-->",
        &client->conn.tcp_client->sock.remote.net_addr,
        fin, opcode, data, data_size,
        "ws send frame");

    co_byte_array_t* buffer = co_byte_array_create();

    co_ws_frame_serialize(
        fin, opcode,
        !co_http_connection_is_server(&client->conn),
        data, data_size,
        buffer);

    bool result =
        co_http_connection_send_data(
            &client->conn,
            co_byte_array_get_ptr(buffer, 0),
            co_byte_array_get_count(buffer));

    co_byte_array_destroy(buffer);

    return result;
}

bool
co_ws_send_binary(
    co_ws_client_t* client,
    const void* data,
    size_t data_size
)
{
    return co_ws_send(client, true,
        CO_WS_OPCODE_BINARY, data, data_size);
}

bool
co_ws_send_text(
    co_ws_client_t* client,
    const char* utf8_str
)
{
    return co_ws_send(client, true,
        CO_WS_OPCODE_TEXT, utf8_str, strlen(utf8_str));
}

bool
co_ws_send_continuation(
    co_ws_client_t* client,
    bool fin,
    const void* data,
    size_t data_size
)
{
    return co_ws_send(client, fin,
        CO_WS_OPCODE_CONTINUATION, data, data_size);
}

bool
co_ws_send_close(
    co_ws_client_t* client,
    uint16_t reason_code,
    const char* utf8_reason_str
)
{
    if (client->closed)
    {
        return true;
    }

    size_t data_size = sizeof(reason_code);

    if (utf8_reason_str != NULL)
    {
        data_size += strlen(utf8_reason_str);
    }

    uint8_t* data =
        (uint8_t*)co_mem_alloc(data_size + 1);

    if (data == NULL)
    {
        return false;
    }

    data[0] = (uint8_t)((reason_code & 0xff00) >> 8);
    data[1] = (uint8_t)(reason_code & 0x00ff);

    if (utf8_reason_str != NULL)
    {
        strcpy((char*)&data[2], utf8_reason_str);
    }
    
    bool result = co_ws_send(client,
        true, CO_WS_OPCODE_CLOSE, data, data_size);

    co_mem_free(data);

    client->closed = true;

    return result;
}

bool
co_ws_send_ping(
    co_ws_client_t* client,
    const void* data,
    size_t data_size
)
{
    return co_ws_send(client,
        true, CO_WS_OPCODE_PING, data, data_size);
}

bool
co_ws_send_pong(
    co_ws_client_t* client,
    const void* data,
    size_t data_size
)
{
    return co_ws_send(client,
        true, CO_WS_OPCODE_PONG, data, data_size);
}

void
co_ws_default_handler(
    co_ws_client_t* client,
    const co_ws_frame_t* frame
)
{
    switch (frame->header.opcode)
    {
    case CO_WS_OPCODE_CLOSE:
    {
        uint16_t reason_code = CO_WS_CLOSE_REASON_NORMAL;

        if (frame->header.payload_size >= sizeof(uint16_t))
        {
            memcpy(&reason_code,
                frame->payload_data, sizeof(uint16_t));
            reason_code =
                co_byte_order_16_network_to_host(reason_code);
        }

        co_ws_send_close(client, reason_code, NULL);

        break;
    }
    case CO_WS_OPCODE_PING:
    {
        co_ws_send_pong(client,
            frame->payload_data, (size_t)frame->header.payload_size);

        break;
    }
    case CO_WS_OPCODE_PONG:
    {
        break;
    }
    default:
        break;
    }
}

co_socket_t*
co_ws_get_socket(
    co_ws_client_t* client
)
{
    return ((client->conn.tcp_client != NULL) ?
        &client->conn.tcp_client->sock : NULL);
}

const char*
co_ws_get_url_origin(
    const co_ws_client_t* client
)
{
    return ((client->conn.url_origin != NULL) ?
        client->conn.url_origin->src : NULL);
}

bool
co_ws_is_open(
    const co_ws_client_t* client
)
{
    return (!client->closed) && ((client->conn.tcp_client != NULL) ?
        co_tcp_is_open(client->conn.tcp_client) : false);
}

void
co_ws_set_user_data(
    co_ws_client_t* client,
    void* user_data
)
{
    co_tcp_set_user_data(
        client->conn.tcp_client, user_data);
}

void*
co_ws_get_user_data(
    const co_ws_client_t* client
)
{
    return co_tcp_get_user_data(
        client->conn.tcp_client);
}
