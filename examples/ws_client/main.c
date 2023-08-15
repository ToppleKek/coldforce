#include <coldforce.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// my app object
typedef struct
{
    co_app_t base_app;

    // my app data
    co_ws_client_t* client;
    char* base_url;
    char* path;

} my_app;

void on_my_ws_receive_frame(my_app* self, co_ws_client_t* client, const co_ws_frame_t* frame, int error_code)
{
    if (error_code == 0)
    {
        bool fin = co_ws_frame_get_fin(frame);
        uint8_t opcode = co_ws_frame_get_opcode(frame);
        size_t data_size = (size_t)co_ws_frame_get_payload_size(frame);
        const uint8_t* data = co_ws_frame_get_payload_data(frame);

        switch (opcode)
        {
        case CO_WS_OPCODE_TEXT:
        {
            printf("receive text(%d): %*.*s\n", fin, (int)data_size, (int)data_size, (char*)data);

            break;
        }
        case CO_WS_OPCODE_BINARY:
        {
            printf("receive binary(%d): %zu bytes\n", fin, data_size);

            break;
        }
        case CO_WS_OPCODE_CONTINUATION:
        {
            printf("receive continuation(%d): %zu bytes\n", fin, data_size);

            break;
        }
        default:
        {
            co_ws_default_handler(client, frame);

            break;
        }
        }
    }
    else
    {
        printf("receive error: %d\n", error_code);

        // close
        co_ws_client_destroy(client);
        self->client = NULL;
    }
}

void on_my_ws_close(my_app* self, co_ws_client_t* client)
{
    printf("close\n");

    // close
    co_ws_client_destroy(client);
    self->client = NULL;

    // quit app
    co_app_stop();
}

void on_my_ws_upgrade(my_app* self, co_ws_client_t* client, const co_http_response_t* response, int error_code)
{
    (void)response;

    printf("receive upgrade response: %d\n", error_code);

    if (error_code == 0)
    {
        printf("upgrade success\n");

        // send
        co_ws_send_text(client, "hello");

        return;
    }
    else
    {
        printf("upgrade error\n");
    }

    // close
    co_ws_client_destroy(client);
    self->client = NULL;

    // quit app
    co_app_stop();
}

void on_my_ws_connect(my_app* self, co_ws_client_t* client, int error_code)
{
    if (error_code == 0)
    {
        printf("connect success\n");

        co_http_request_t* request =
            co_http_request_create_ws_upgrade(self->path, NULL, NULL);

        co_ws_send_upgrade_request(self->client, request);
    }
    else
    {
        printf("connect error: %d\n", error_code);

        co_ws_client_destroy(client);
        self->client = NULL;
    }
}

bool on_my_app_create(my_app* self)
{
    const co_args_st* args = co_app_get_args((co_app_t*)self);

    if (args->count < 2)
    {
        printf("<Usage>\n");
        printf("ws_client <url>\n");

        return false;
    }

    co_url_st* url = co_url_create(args->values[1]);

    self->base_url = co_url_create_base_url(url);
    self->path = co_url_create_path_and_query(url);

    co_url_destroy(url);

    co_net_addr_t local_net_addr = { 0 };
    co_net_addr_set_family(&local_net_addr, CO_NET_ADDR_FAMILY_IPV4);

    self->client = co_ws_client_create(self->base_url, &local_net_addr, NULL);

    if (self->client == NULL)
    {
        printf("error: faild to resolve hostname or OpenSSL is not installed\n");

        return false;
    }

    // callback
    co_ws_callbacks_st* callbacks = co_ws_get_callbacks(self->client);
    callbacks->on_connect = (co_ws_connect_fn)on_my_ws_connect;
    callbacks->on_upgrade = (co_ws_upgrade_fn)on_my_ws_upgrade;
    callbacks->on_receive_frame = (co_ws_receive_frame_fn)on_my_ws_receive_frame;
    callbacks->on_close = (co_ws_close_fn)on_my_ws_close;

    // connect
    co_ws_connect(self->client);

    return true;
}

void on_my_app_destroy(my_app* self)
{
    co_ws_client_destroy(self->client);

    co_string_destroy(self->base_url);
    co_string_destroy(self->path);
}

int main(int argc, char* argv[])
{
//    co_http_log_set_level(CO_LOG_LEVEL_MAX);
//    co_ws_log_set_level(CO_LOG_LEVEL_MAX);

    my_app app = { 0 };

    co_net_app_init(
        (co_app_t*)&app,
        (co_app_create_fn)on_my_app_create,
        (co_app_destroy_fn)on_my_app_destroy,
        argc, argv);

    // run
    int exit_code = co_app_run((co_app_t*)&app);

    co_net_app_cleanup((co_app_t*)&app);

    return exit_code;
}
