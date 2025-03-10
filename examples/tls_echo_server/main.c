#include <coldforce.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
#   ifdef CO_USE_WOLFSSL
#       pragma comment(lib, "wolfssl.lib")
#   elif defined(CO_USE_OPENSSL)
#       pragma comment(lib, "libssl.lib")
#       pragma comment(lib, "libcrypto.lib")
#   endif
#endif

//---------------------------------------------------------------------------//
// app object
//---------------------------------------------------------------------------//

typedef struct
{
    co_app_t base_app;

    // app data
    co_tcp_server_t* tcp_server;
    co_list_t* tcp_clients;

} app_st;

#define app_get_remote_address(protocol, net_unit, buffer) \
    co_net_addr_to_string( \
        co_socket_get_remote_net_addr( \
            co_##protocol##_get_socket(net_unit)), \
        buffer, sizeof(buffer));

void
app_on_tls_handshake(
    app_st* self,
    co_tcp_client_t* tcp_client,
    int error_code
);

//---------------------------------------------------------------------------//
// tcp callback
//---------------------------------------------------------------------------//

void
app_on_tcp_receive(
    app_st* self,
    co_tcp_client_t* tcp_client
)
{
    (void)self;

    char remote_str[64];
    app_get_remote_address(tcp, tcp_client, remote_str);

    for (;;)
    {
        char buffer[1024];

        // receive data
        ssize_t size =
            co_tls_tcp_receive(tcp_client, buffer, sizeof(buffer));

        if (size <= 0)
        {
            break;
        }

        printf("tcp received: %zd bytes from %s\n",
            (size_t)size, remote_str);

        // send (echo)
        co_tls_tcp_send(tcp_client, buffer, size);
    }
}

void
app_on_tcp_close(
    app_st* self,
    co_tcp_client_t* tcp_client
)
{
    char remote_str[64];
    app_get_remote_address(tcp, tcp_client, remote_str);
    printf("tcp closed %s\n", remote_str);

    co_list_remove(self->tcp_clients, tcp_client);
}

void
app_on_tcp_accept(
    app_st* self,
    co_tcp_server_t* tcp_server,
    co_tcp_client_t* tcp_client
)
{
    (void)tcp_server;

    // accept
    co_tcp_accept((co_thread_t*)self, tcp_client);

    // tcp callbacks
    co_tcp_callbacks_st* tcp_callbacks = co_tcp_get_callbacks(tcp_client);
    tcp_callbacks->on_receive = (co_tcp_receive_fn)app_on_tcp_receive;
    tcp_callbacks->on_close = (co_tcp_close_fn)app_on_tcp_close;

    // tls callbacks
    co_tls_callbacks_st* tls_callbacks = co_tls_tcp_get_callbacks(tcp_client);
    tls_callbacks->on_handshake = (co_tls_handshake_fn)app_on_tls_handshake;

    // start tls handshake
    co_tls_tcp_handshake_start(tcp_client);

    co_list_add_tail(self->tcp_clients, tcp_client);

    char remote_str[64];
    app_get_remote_address(tcp, tcp_client, remote_str);
    printf("tcp accept: %s\n", remote_str);
}

//---------------------------------------------------------------------------//
// tls callback
//---------------------------------------------------------------------------//

void
app_on_tls_handshake(
    app_st* self,
    co_tcp_client_t* tcp_client,
    int error_code
)
{
    char remote_str[64];
    app_get_remote_address(tcp, tcp_client, remote_str);

    if (error_code == 0)
    {
        printf("tls handshake success: %s\n", remote_str);
    }
    else
    {
        printf("tls handshake failed: %s\n", remote_str);

        co_list_remove(self->tcp_clients, tcp_client);
    }
}

//---------------------------------------------------------------------------//
// tls setup
//---------------------------------------------------------------------------//

bool
app_tls_setup(
    co_tls_ctx_st* tls_ctx
)
{
#ifdef CO_USE_TLS
    const char* certificate_file = "../../../test_file/server.crt";
    const char* private_key_file = "../../../test_file/server.key";

    SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_server_method());

    if (SSL_CTX_use_certificate_file(
        ssl_ctx, certificate_file, SSL_FILETYPE_PEM) != 1)
    {
        SSL_CTX_free(ssl_ctx);

        printf("SSL_CTX_use_certificate_file failed\n");

        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(
        ssl_ctx, private_key_file, SSL_FILETYPE_PEM) != 1)
    {
        SSL_CTX_free(ssl_ctx);

        printf("SSL_CTX_use_PrivateKey_file failed\n");

        return false;
    }

    tls_ctx->ssl_ctx = ssl_ctx;
#endif

    return true;
}

//---------------------------------------------------------------------------//
// app callback
//---------------------------------------------------------------------------//

bool
app_on_create(
    app_st* self
)
{
    const co_args_st* args = co_app_get_args((co_app_t*)self);

    if (args->count <= 1)
    {
        printf("<Usage>\n");
        printf("tls_echo_server <port_number>\n");

        return false;
    }

    uint16_t port = (uint16_t)atoi(args->values[1]);

    // client list
    co_list_ctx_st list_ctx = { 0 };
    list_ctx.destroy_value =
        (co_item_destroy_fn)co_tls_tcp_client_destroy; // auto destroy
    self->tcp_clients = co_list_create(&list_ctx);

    // local address
    co_net_addr_t local_net_addr = { 0 };
    co_net_addr_set_family(&local_net_addr, CO_NET_ADDR_FAMILY_IPV4);
    co_net_addr_set_port(&local_net_addr, port);
    
    // tls setup
    co_tls_ctx_st tls_ctx = { 0 };
    if (!app_tls_setup(&tls_ctx))
    {
        return false;
    }

    // create tls server
    self->tcp_server =
        co_tls_tcp_server_create(&local_net_addr, &tls_ctx);

    if (self->tcp_server == NULL)
    {
        printf("Failed to create tls server (maybe SSL/TLS library was not found)\n");

        return false;
    }

    // socket option
    co_socket_option_set_reuse_addr(
        co_tcp_server_get_socket(self->tcp_server), true);

    // callbacks
    co_tcp_server_callbacks_st* callbacks =
        co_tcp_server_get_callbacks(self->tcp_server);
    callbacks->on_accept = (co_tcp_accept_fn)app_on_tcp_accept;

    // start listen
    co_tls_tcp_server_start(self->tcp_server, SOMAXCONN);

    char local_str[64];
    co_net_addr_to_string(&local_net_addr, local_str, sizeof(local_str));
    printf("start server: %s\n", local_str);

    return true;
}

void
app_on_destroy(
    app_st* self
)
{
    co_tls_tcp_server_destroy(self->tcp_server);
    co_list_destroy(self->tcp_clients);
}

void
app_on_signal(
    int sig
)
{
    (void)sig;

    // quit app
    co_app_stop();
}

//---------------------------------------------------------------------------//
// main
//---------------------------------------------------------------------------//

int
main(
    int argc,
    char* argv[]
)
{
    co_win_debug_crt_set_flags();

    signal(SIGINT, app_on_signal);

//    co_tls_log_set_level(CO_LOG_LEVEL_MAX);
//    co_tcp_log_set_level(CO_LOG_LEVEL_MAX);

    // app instance
    app_st self = { 0 };

    // start app
    return co_net_app_start(
        (co_app_t*)&self, "tls-server-app",
        (co_app_create_fn)app_on_create,
        (co_app_destroy_fn)app_on_destroy,
        argc, argv);
}
