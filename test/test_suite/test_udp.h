#pragma once

#include "test_std.h"
#include "test_udp_server.h"

typedef struct
{
    co_udp_t* udp_client;
    co_byte_array_t* send_data;
    co_byte_array_t* receive_data;
    co_timer_t* send_timer;
    size_t send_index;
    size_t send_count;
    size_t send_async_count;
    size_t send_async_comp_count;

    size_t total_send_count;
    size_t receive_count;

} test_udp_client_st;

typedef struct
{
    co_thread_t base;

    co_net_addr_family_t family;
    const char* server_address;
    uint16_t server_port;

    size_t data_size;
    size_t client_count;
    co_net_addr_t remote_net_addr;

    co_list_t* test_udp_clients;
    test_udp_server_thread_st test_udp_server_thread;

} test_udp_thread_st;

void test_udp_run(test_udp_thread_st* thread);
