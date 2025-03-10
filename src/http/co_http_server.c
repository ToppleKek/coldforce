#include <coldforce/core/co_std.h>
#include <coldforce/core/co_string.h>
#include <coldforce/core/co_string_token.h>

#include <coldforce/tls/co_tls_tcp_client.h>
#include <coldforce/tls/co_tls_tcp_server.h>

#include <coldforce/http/co_http_server.h>
#include <coldforce/http/co_http_client.h>
#include <coldforce/http/co_http_config.h>
#include <coldforce/http/co_http_log.h>

//---------------------------------------------------------------------------//
// http server
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
// private
//---------------------------------------------------------------------------//

static void
co_http_server_on_request(
    co_thread_t* thread,
    co_http_client_t* client,
    int error_code
)
{
    co_http_request_t* request = client->request;

    if (client->callbacks.on_receive_finish != NULL)
    {
        client->callbacks.on_receive_finish(
            thread, client, request, NULL, error_code);
    }

    co_http_request_destroy(client->request);
    client->request = NULL;
}

void
co_http_server_on_tcp_receive_ready(
    co_thread_t* thread,
    co_tcp_client_t* tcp_client
)
{
    co_http_client_t* client =
        (co_http_client_t*)tcp_client->sock.sub_class;

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
        if (client->request == NULL)
        {
            client->request = co_http_request_create(NULL, NULL);

            if (client->request == NULL)
            {
                co_http_server_on_request(
                    thread, client, CO_HTTP_ERROR_OUT_OF_MEMORY);

                return;
            }

            int result = co_http_request_deserialize(
                client->request,
                client->conn.receive_data.ptr,
                &client->conn.receive_data.index);

            if (result == CO_HTTP_PARSE_COMPLETE)
            {
                co_http_log_debug_request_header(
                    &client->conn.tcp_client->sock.local.net_addr, "<--",
                    &client->conn.tcp_client->sock.remote.net_addr,
                    client->request, "http receive request");

                co_http_content_receiver_clear(&client->content_receiver);

                if (!co_http_start_receive_content(
                    &client->content_receiver, client,
                    &client->request->message,
                    client->conn.receive_data.index))
                {
                    return;
                }

                if ((!client->content_receiver.chunked) &&
                    (client->content_receiver.size >
                        co_http_config_get_max_receive_content_size()))
                {
                    co_http_server_on_request(
                        thread, client, CO_HTTP_ERROR_CONTENT_TOO_BIG);

                    return;
                }
            }
            else if (result == CO_HTTP_PARSE_MORE_DATA)
            {
                co_http_request_destroy(client->request);
                client->request = NULL;

                return;
            }
            else
            {
                co_http_server_on_request(thread, client, result);

                return;
            }
        }

        int result = co_http_receive_content_data(
            &client->content_receiver, client, client->conn.receive_data.ptr);

        if (result == CO_HTTP_PARSE_COMPLETE)
        {
            co_http_complete_receive_content(
                &client->content_receiver,
                &client->conn.receive_data.index,
                &client->request->message.data);

            co_http_server_on_request(thread, client, 0);

            if (client->conn.tcp_client == NULL)
            {
                return;
            }
        }
        else if (result == CO_HTTP_PARSE_MORE_DATA)
        {
            co_http_content_more_data(
                &client->content_receiver,
                &client->conn.receive_data.index,
                client->conn.receive_data.ptr);

            return;
        }
        else if (result == CO_HTTP_PARSE_CANCEL)
        {
            co_http_server_on_request(thread, client, CO_HTTP_ERROR_CANCEL);

            return;
        }
        else
        {
            co_http_server_on_request(thread, client, result);

            return;
        }
    }

    client->conn.receive_data.index = 0;
    co_byte_array_clear(client->conn.receive_data.ptr);
}

void
co_http_server_on_http_connection_close(
    co_thread_t* thread,
    co_http_connection_t* conn
)
{
    co_http_client_t* client = (co_http_client_t*)conn;

    if (client->callbacks.on_close != NULL)
    {
        client->callbacks.on_close(thread, client);
    }
}

//---------------------------------------------------------------------------//
// public
//---------------------------------------------------------------------------//

bool
co_http_send_response(
    co_http_client_t* client,
    co_http_response_t* response
)
{
    bool result = co_http_connection_send_response(
        &client->conn, response);

    if (result)
    {
        co_http_response_destroy(response);
    }

    return result;
}

bool
co_http_start_chunked_response(
    co_http_client_t* client,
    co_http_response_t* response
)
{
    const char* transfer_encoding =
        co_http_header_get_field(&response->message.header,
            CO_HTTP_HEADER_TRANSFER_ENCODING);

    if (transfer_encoding == NULL)
    {
        co_http_header_add_field(
            &response->message.header,
            CO_HTTP_HEADER_TRANSFER_ENCODING,
            CO_HTTP_TRANSFER_ENCODING_CHUNKED);
    }
    else
    {
        co_string_token_st tokens[32];

        size_t token_count = co_string_token_split(
            transfer_encoding, tokens, 32);

        if (!co_string_token_contains(
            tokens, token_count,
            CO_HTTP_TRANSFER_ENCODING_CHUNKED))
        {
            char new_item[1024];

            strcpy(new_item, transfer_encoding);
            strcat(new_item, ", ");
            strcat(new_item, CO_HTTP_TRANSFER_ENCODING_CHUNKED);

            co_http_header_set_field(
                &response->message.header,
                CO_HTTP_HEADER_TRANSFER_ENCODING,
                new_item);
        }

        co_string_token_cleanup(tokens, token_count);
    }

    return co_http_send_response(client, response);
}

bool
co_http_send_chunked_data(
    co_http_client_t* client,
    const void* data,
    size_t data_length
)
{
    co_byte_array_t* buffer = co_byte_array_create();

    char chunk_size[64];
    sprintf(chunk_size, "%x"CO_HTTP_CRLF, (unsigned int)data_length);

    co_byte_array_add_string(buffer, chunk_size);

    if (data_length > 0)
    {
        co_byte_array_add(buffer, data, data_length);
    }

    co_byte_array_add_string(buffer, CO_HTTP_CRLF);

    co_http_log_debug(NULL, NULL, NULL,
        "http send chunked data %zd", data_length);

    bool result =
        co_http_connection_send_data(
            &client->conn,
            co_byte_array_get_ptr(buffer, 0),
            co_byte_array_get_count(buffer));

    co_byte_array_destroy(buffer);

    return result;
}

bool
co_http_end_chunked_response(
    co_http_client_t* client
)
{
    return co_http_send_chunked_data(client, NULL, 0);
}
