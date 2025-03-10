#pragma once

#include "test_std.h"

typedef struct
{
    co_thread_t base;

    bool close;
    co_net_addr_family_t family;
    const char* address;
    uint16_t port;
    co_tcp_server_t* tcp_server;
    co_list_t* tcp_clients;

} test_tcp_server_thread_st;

void test_tcp_server_thread_start(test_tcp_server_thread_st* test_tcp_server_thread);
void test_tcp_server_thread_stop(test_tcp_server_thread_st* test_tcp_server_thread);
