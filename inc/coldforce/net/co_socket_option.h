#ifndef CO_SOCKET_OPTION_H_INCLUDED
#define CO_SOCKET_OPTION_H_INCLUDED

#include <coldforce/net/co_net.h>
#include <coldforce/net/co_socket.h>

CO_EXTERN_C_BEGIN

//---------------------------------------------------------------------------//
// socket option
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

// any
CO_NET_API bool co_socket_option_set(
    co_socket_t* sock, int level, int name, const void* data, size_t length);
CO_NET_API bool co_socket_option_get(
    const co_socket_t* sock, int level, int name, void* buffer, size_t* length);

// SO_REUSEADDR
CO_NET_API bool co_socket_option_set_reuse_addr(co_socket_t* sock, bool enable);
CO_NET_API bool co_socket_option_get_reuse_addr(const co_socket_t* sock, bool* enable);

// SO_KEEPALIVE
CO_NET_API bool co_socket_option_set_keep_alive(co_socket_t* sock, bool enable);
CO_NET_API bool co_socket_option_get_keep_alive(const co_socket_t* sock, bool* enable);

// SO_SNDBUF
CO_NET_API bool co_socket_option_set_send_buff_size(co_socket_t* sock, size_t size);
CO_NET_API bool co_socket_option_get_send_buff_size(const co_socket_t* sock, size_t* size);

// SO_RCVBUF
CO_NET_API bool co_socket_option_set_receive_buff_size(co_socket_t* sock, size_t size);
CO_NET_API bool co_socket_option_get_receive_buff_size(const co_socket_t* sock, size_t* size);

// SO_LINGER
CO_NET_API bool co_socket_option_set_linger(co_socket_t* sock, const struct linger* linger);
CO_NET_API bool co_socket_option_get_linger(const co_socket_t* sock, struct linger* linger);

// TCP_NODELAY
CO_NET_API bool co_socket_option_set_tcp_no_delay(co_socket_t* sock, bool enable);
CO_NET_API bool co_socket_option_get_tcp_no_delay(const co_socket_t* sock, bool* enable);

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

CO_EXTERN_C_END

#endif // CO_SOCKET_OPTION_H_INCLUDED
