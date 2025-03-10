#include <coldforce/core/co_std.h>
#include <coldforce/core/co_string.h>

#include <coldforce/tls/co_tls_tcp_client.h>

#include <coldforce/http/co_http_client.h>
#include <coldforce/http/co_http_log.h>

//---------------------------------------------------------------------------//
// http connection
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
// private
//---------------------------------------------------------------------------//

static void
co_http_connection_on_tls_handshake(
    co_thread_t* thread,
    co_tcp_client_t* tcp_client,
    int error_code
)
{
    co_http_connection_t* client =
        (co_http_connection_t*)tcp_client->sock.sub_class;

    if (client->callbacks.on_connect != NULL)
    {
        client->callbacks.on_connect(thread, client, error_code);
    }
}

void
co_http_connection_on_tcp_connect(
    co_thread_t* thread,
    co_tcp_client_t* tcp_client,
    int error_code
)
{
    if (error_code == 0)
    {
        co_tls_client_t* tls =
            (co_tls_client_t*)tcp_client->sock.tls;

        if (tls != NULL)
        {
            tls->callbacks.on_handshake =
                (co_tls_handshake_fn)co_http_connection_on_tls_handshake;

            co_tls_tcp_handshake_start(tcp_client);

            return;
        }
    }

    co_http_connection_t* conn =
        (co_http_connection_t*)tcp_client->sock.sub_class;

    if (conn->callbacks.on_connect != NULL)
    {
        conn->callbacks.on_connect(thread, conn, error_code);
    }
}

void
co_http_connection_on_tcp_close(
    co_thread_t* thread,
    co_tcp_client_t* tcp_client
)
{
    co_http_connection_t* conn =
        (co_http_connection_t*)tcp_client->sock.sub_class;

    if (conn->callbacks.on_close != NULL)
    {
        conn->callbacks.on_close(thread, conn);
    }
}

bool
co_http_connection_setup(
    co_http_connection_t* conn,
    co_url_st* url_origin,
    const co_net_addr_t* local_net_addr,
    const char** protocols,
    size_t protocol_count,
    co_tls_ctx_st* tls_ctx
)
{
    if (url_origin->host == NULL)
    {
        return false;
    }

    bool tls_scheme = false;

    if (url_origin->scheme == NULL)
    {
        url_origin->scheme = co_string_duplicate("http");
    }
    else if (
        co_string_case_compare(
            url_origin->scheme, "https") == 0)
    {
        tls_scheme = true;
    }

    if (tls_scheme)
    {
        conn->module.destroy = co_tls_tcp_client_destroy;
        conn->module.close = co_tcp_close;
        conn->module.connect = co_tcp_connect_start;
        conn->module.send = co_tls_tcp_send;
        conn->module.receive_all = co_tls_tcp_receive_all;

        conn->tcp_client =
            co_tls_tcp_client_create(local_net_addr, tls_ctx);

        if (conn->tcp_client != NULL)
        {
            co_tls_tcp_set_host_name(
                conn->tcp_client, url_origin->host);

            if (protocols != NULL && protocol_count > 0)
            {
                co_tls_tcp_set_available_protocols(
                    conn->tcp_client, protocols, protocol_count);
            }
        }
    }
    else
    {
        conn->module.destroy = co_tcp_client_destroy;
        conn->module.close = co_tcp_close;
        conn->module.connect = co_tcp_connect_start;
        conn->module.send = co_tcp_send;
        conn->module.receive_all = co_tcp_receive_all;

        conn->tcp_client =
            co_tcp_client_create(local_net_addr);
    }

    if (conn->tcp_client == NULL)
    {
        return false;
    }

    int address_family =
        co_net_addr_get_family(local_net_addr);

    co_net_addr_init(&conn->tcp_client->sock.remote.net_addr);

    if (!co_url_to_net_addr(
        url_origin, address_family,
        &conn->tcp_client->sock.remote.net_addr))
    {
        conn->module.destroy(conn->tcp_client);
        conn->tcp_client = NULL;

        co_http_log_error(NULL, NULL, NULL,
            "failed to resolve hostname (%s)", url_origin->src);

        return false;
    }

    conn->tcp_client->sock.sub_class = conn;
    conn->callbacks.on_connect = NULL;
    conn->callbacks.on_close = NULL;
    conn->url_origin = url_origin;
    conn->receive_data.index = 0;
    conn->receive_data.ptr = co_byte_array_create();

    conn->tcp_client->callbacks.on_connect =
        co_http_connection_on_tcp_connect;
    conn->tcp_client->callbacks.on_close =
        co_http_connection_on_tcp_close;

    return true;
}

void
co_http_connection_cleanup(
    co_http_connection_t* conn
)
{
    if (conn != NULL)
    {
        co_byte_array_destroy(conn->receive_data.ptr);
        conn->receive_data.ptr = NULL;

        co_url_destroy(conn->url_origin);
        conn->url_origin = NULL;

        if (conn->tcp_client != NULL)
        {
            conn->module.destroy(conn->tcp_client);
            conn->tcp_client = NULL;
        }
    }
}

void
co_http_connection_move(
    co_http_connection_t* from_conn,
    co_http_connection_t* to_conn
)
{
    *to_conn = *from_conn;

    to_conn->tcp_client->sock.sub_class = to_conn;

    from_conn->tcp_client = NULL;
    from_conn->callbacks.on_connect = NULL;
    from_conn->callbacks.on_close = NULL;
    from_conn->url_origin = NULL;
    from_conn->receive_data.ptr = NULL;
}

bool
co_http_connection_is_server(
    const co_http_connection_t* conn
)
{
    return (conn->url_origin == NULL);
}

//---------------------------------------------------------------------------//
// public
//---------------------------------------------------------------------------//

bool
co_http_connection_send_request(
    co_http_connection_t* conn,
    const co_http_request_t* request
)
{
    co_http_log_debug_request_header(
        &conn->tcp_client->sock.local.net_addr, "-->",
        &conn->tcp_client->sock.remote.net_addr,
        request, "http send request");

    co_byte_array_t* buffer = co_byte_array_create();

    co_http_request_serialize(request, buffer);

    bool result =
        co_http_connection_send_data(conn,
            co_byte_array_get_ptr(buffer, 0),
            co_byte_array_get_count(buffer));

    co_byte_array_destroy(buffer);

    if (result)
    {
        if (!co_tcp_is_timer_running(conn->tcp_client))
        {
            co_tcp_start_timer(conn->tcp_client);
        }
    }

    return result;
}

bool
co_http_connection_send_response(
    co_http_connection_t* conn,
    const co_http_response_t* response
)
{
    co_http_log_debug_response_header(
        &conn->tcp_client->sock.local.net_addr, "-->",
        &conn->tcp_client->sock.remote.net_addr,
        response, "http send response");

    co_byte_array_t* buffer = co_byte_array_create();

    co_http_response_serialize(response, buffer);

    bool result =
        co_http_connection_send_data(conn,
            co_byte_array_get_ptr(buffer, 0),
            co_byte_array_get_count(buffer));

    co_byte_array_destroy(buffer);

    return result;
}

bool
co_http_connection_send_data(
    co_http_connection_t* conn,
    const void* data,
    size_t data_size
)
{
    return conn->module.send(
        conn->tcp_client, data, data_size);
}
