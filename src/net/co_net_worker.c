#include <coldforce/core/co_std.h>

#include <coldforce/net/co_net_worker.h>
#include <coldforce/net/co_net_event.h>
#include <coldforce/net/co_net_log.h>

//---------------------------------------------------------------------------//
// net worker
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
//---------------------------------------------------------------------------//

//---------------------------------------------------------------------------//
// private
//---------------------------------------------------------------------------//

static void
co_net_worker_on_idle(
    co_net_worker_t* net_worker
)
{
    co_event_worker_on_idle(&net_worker->event_worker);

#ifdef CO_OS_WIN
    co_win_try_clear_io_ctx_trash(net_worker->net_selector);
#endif
}

static void
co_net_worker_on_tcp_accept(
    co_net_worker_t* net_worker,
    co_tcp_client_t* client
)
{
    if (net_worker->callbacks.on_tcp_accept != NULL)
    {
        CO_DEBUG_SOCKET_COUNTER_INC();

        net_worker->callbacks.on_tcp_accept(
            co_thread_get_current(), NULL, client);
    }
    else
    {
        co_tcp_client_destroy(client);
    }
}

static void
co_net_worker_on_udp_accept(
    co_net_worker_t* net_worker,
    co_udp_t* udp_conn
)
{
    if (net_worker->callbacks.on_udp_accept != NULL)
    {
        CO_DEBUG_SOCKET_COUNTER_INC();

        net_worker->callbacks.on_udp_accept(
            co_thread_get_current(), NULL, udp_conn);
    }
    else
    {
        co_udp_destroy(udp_conn);
    }
}

co_net_worker_t*
co_net_worker_create(
    void
)
{
    co_net_worker_t* net_worker =
        (co_net_worker_t*)co_mem_alloc(sizeof(co_net_worker_t));

    if (net_worker == NULL)
    {
        return NULL;
    }

    net_worker->event_worker.wait = (co_event_wait_fn)co_net_worker_wait;
    net_worker->event_worker.dispatch = (co_event_dispatch_fn)co_net_worker_dispatch;
    net_worker->event_worker.wake_up = (co_event_wake_up_fn)co_net_worker_wake_up;
    net_worker->event_worker.on_idle = (co_event_idle_fn)co_net_worker_on_idle;

    net_worker->net_selector = co_net_selector_create();
    net_worker->tcp_servers = NULL;
    net_worker->tcp_clients = NULL;
    net_worker->udps = NULL;

    net_worker->callbacks.on_tcp_accept = NULL;
    net_worker->callbacks.on_udp_accept = NULL;

    net_worker->on_destroy = NULL;

#ifdef CO_DEBUG
    net_worker->sock_count = 0;
#endif

    return net_worker;
}

void
co_net_worker_cleanup(
    co_net_worker_t* net_worker
)
{
    if (net_worker == NULL)
    {
        return;
    }

    if (net_worker->tcp_servers != NULL)
    {
        co_list_destroy(net_worker->tcp_servers);
        net_worker->tcp_servers = NULL;
    }

    if (net_worker->tcp_clients != NULL)
    {
        co_list_destroy(net_worker->tcp_clients);
        net_worker->tcp_clients = NULL;
    }

    if (net_worker->udps != NULL)
    {
        co_list_destroy(net_worker->udps);
        net_worker->udps = NULL;
    }

    co_net_selector_destroy(net_worker->net_selector);
    net_worker->net_selector = NULL;

    co_assert(net_worker->sock_count == 0);
}

void
co_net_worker_on_destroy(
    co_thread_t* thread
)
{
    co_net_worker_t* net_worker =
        (co_net_worker_t*)thread->event_worker;

    if (net_worker->on_destroy != NULL)
    {
        net_worker->on_destroy(thread);
    }

    if (net_worker->tcp_clients != NULL)
    {
        if (co_list_get_count(net_worker->tcp_clients) > 0)
        {
            for (int counter = 0; counter < 3; ++counter)
            {
                net_worker->event_worker.wait(
                    &net_worker->event_worker, 1000);

                if (co_list_get_count(
                    net_worker->tcp_clients) == 0)
                {
                    break;
                }
            }

            while (co_list_get_count(net_worker->tcp_clients) > 0)
            {
                co_list_data_st* data =
                    co_list_get_head(net_worker->tcp_clients);

                co_tcp_client_on_close((co_tcp_client_t*)data->value);
            }
        }
    }
}

co_wait_result_t
co_net_worker_wait(
    co_net_worker_t* net_worker,
    uint32_t msec
)
{
    return co_net_selector_wait(net_worker->net_selector, msec);
}

void
co_net_worker_wake_up(
    co_net_worker_t* net_worker
)
{
    co_net_selector_wake_up(net_worker->net_selector);
}

bool
co_net_worker_dispatch(
    co_net_worker_t* net_worker,
    co_event_st* event
)
{
    switch (event->id)
    {
    case CO_NET_EVENT_ID_TCP_ACCEPT_READY:
    {
        co_tcp_server_on_accept_ready(
            (co_tcp_server_t*)event->param1);
        break;
    }
    case CO_NET_EVENT_ID_TCP_CONNECT_COMPLETE:
    {
        co_tcp_client_on_connect_complete(
            (co_tcp_client_t*)event->param1, (int)event->param2);
        break;
    }
#ifndef CO_OS_WIN
    case CO_NET_EVENT_ID_TCP_SEND_ASYNC_READY:
    {
        co_tcp_client_on_send_async_ready(
            (co_tcp_client_t*)event->param1);
        break;
    }
#endif
    case CO_NET_EVENT_ID_TCP_SEND_ASYNC_COMPLETE:
    {
        co_tcp_client_on_send_async_complete(
            (co_tcp_client_t*)event->param1, (bool)(event->param2 > 0));
        break;
    }
    case CO_NET_EVENT_ID_TCP_RECEIVE_READY:
    {
        co_tcp_client_on_receive_ready(
            (co_tcp_client_t*)event->param1, (size_t)event->param2);
        break;
    }
    case CO_NET_EVENT_ID_TCP_CLOSE:
    {
        co_tcp_client_on_close(
            (co_tcp_client_t*)event->param1);
        break;
    }
    case CO_NET_EVENT_ID_TCP_ACCEPT_ON_THREAD:
    {
        co_net_worker_on_tcp_accept(
            net_worker, (co_tcp_client_t*)event->param1);
        break;
    }
    case CO_NET_EVENT_ID_UDP_ACCEPT_ON_THREAD:
    {
        co_net_worker_on_udp_accept(
            net_worker, (co_udp_t*)event->param1);
        break;
    }
#ifndef CO_OS_WIN
    case CO_NET_EVENT_ID_UDP_SEND_ASYNC_READY:
    {
        co_udp_on_send_async_ready(
            (co_udp_t*)event->param1);
        break;
    }
#endif
    case CO_NET_EVENT_ID_UDP_SEND_ASYNC_COMPLETE:
    {
        co_udp_on_send_async_complete(
            (co_udp_t*)event->param1, (bool)(event->param2 > 0));
        break;
    }
    case CO_NET_EVENT_ID_UDP_RECEIVE_READY:
    {
        co_udp_on_receive_ready(
            (co_udp_t*)event->param1, (size_t)event->param2);
        break;
    }
    default:
        return co_event_worker_dispatch(
            &net_worker->event_worker, event);
    }

    return true;
}

bool
co_net_worker_register_tcp_server(
    co_net_worker_t* net_worker,
    co_tcp_server_t* server
)
{
    if (!co_net_selector_register(
        net_worker->net_selector,
        &server->sock, CO_SOCKET_EVENT_ACCEPT))
    {
        return false;
    }

    if (net_worker->tcp_servers == NULL)
    {
        net_worker->tcp_servers = co_list_create(NULL);
    }

    co_list_add_tail(
        net_worker->tcp_servers, server);

    return true;
}

void
co_net_worker_unregister_tcp_server(
    co_net_worker_t* net_worker,
    co_tcp_server_t* server
)
{
    if (net_worker->tcp_servers == NULL)
    {
        return;
    }

    co_list_iterator_t* it =
        co_list_find(net_worker->tcp_servers, server);

    if (it == NULL)
    {
        return;
    }

    co_list_remove_at(net_worker->tcp_servers, it);

    co_net_selector_unregister(
        net_worker->net_selector, &server->sock);
}

bool
co_net_worker_register_tcp_connector(
    co_net_worker_t* net_worker,
    co_tcp_client_t* client
)
{
    if (net_worker->tcp_clients == NULL)
    {
        net_worker->tcp_clients = co_list_create(NULL);
    }

    if (co_list_contains(
        net_worker->tcp_clients, client))
    {
        return true;
    }

    if (!co_net_selector_register(
        net_worker->net_selector,
        &client->sock, CO_SOCKET_EVENT_CONNECT))
    {
        return false;
    }

    co_list_add_tail(
        net_worker->tcp_clients, client);

    return true;
}

void
co_net_worker_unregister_tcp_connector(
    co_net_worker_t* net_worker,
    co_tcp_client_t* client
)
{
    if (net_worker->tcp_clients == NULL)
    {
        return;
    }

    co_list_iterator_t* it =
        co_list_find(net_worker->tcp_clients, client);

    if (it == NULL)
    {
        return;
    }

    co_list_remove_at(net_worker->tcp_clients, it);

    co_net_selector_unregister(
        net_worker->net_selector, &client->sock);
}

bool
co_net_worker_register_tcp_connection(
    co_net_worker_t* net_worker,
    co_tcp_client_t* client
)
{
    if (net_worker->tcp_clients == NULL)
    {
        net_worker->tcp_clients = co_list_create(NULL);
    }

    if (co_list_contains(
        net_worker->tcp_clients, client))
    {
        return true;
    }

    if (!co_net_selector_register(
        net_worker->net_selector, &client->sock,
        (CO_SOCKET_EVENT_RECEIVE | CO_SOCKET_EVENT_CLOSE)))
    {
        return false;
    }

    co_list_add_tail(
        net_worker->tcp_clients, client);

    return true;
}

void
co_net_worker_unregister_tcp_connection(
    co_net_worker_t* net_worker,
    co_tcp_client_t* client)
{
    if (net_worker->tcp_clients != NULL)
    {
        co_list_iterator_t* it =
            co_list_find(net_worker->tcp_clients, client);

        if (it != NULL)
        {
            if (client->close_timer != NULL)
            {
                co_timer_stop(client->close_timer);
                co_timer_destroy(client->close_timer);

                client->close_timer = NULL;
            }

            co_list_remove_at(net_worker->tcp_clients, it);

            co_net_selector_unregister(net_worker->net_selector, &client->sock);
        }
    }
}

void
co_net_worker_tcp_client_close_timer(
    co_thread_t* thread,
    co_timer_t* timer
)
{
    co_tcp_client_t* client =
        (co_tcp_client_t*)co_timer_get_user_data(timer);

    co_tcp_log_warning(
        &client->sock.local.net_addr,
        "<--",
        &client->sock.remote.net_addr,
        "tcp close timeout");

    client->sock.remote.is_open = false;

    co_net_worker_unregister_tcp_connection(
        (co_net_worker_t*)thread->event_worker, client);

    co_tcp_client_on_close(client);
}

bool
co_net_worker_close_tcp_client_local(
    co_net_worker_t* net_worker,
    co_tcp_client_t* client,
    uint32_t timeout_msec
)
{
    if (!client->sock.local.is_open)
    {
        return false;
    }

    client->sock.local.is_open = false;

    if (!client->sock.remote.is_open)
    {
        co_net_worker_unregister_tcp_connection(net_worker, client);

        return false;
    }

    co_socket_handle_shutdown(
        client->sock.handle,
        CO_SOCKET_SHUTDOWN_SEND);

    client->close_timer = co_timer_create(timeout_msec,
        (co_timer_fn)co_net_worker_tcp_client_close_timer,
        false, client);

    co_timer_start(client->close_timer);

    return true;
}

bool
co_net_worker_close_tcp_client_remote(
    co_net_worker_t* net_worker,
    co_tcp_client_t* client
)
{
    if (!co_list_contains(
        net_worker->tcp_clients, client))
    {
        return false;
    }

    if (!client->sock.remote.is_open)
    {
        return true;
    }

    client->sock.remote.is_open = false;

    co_net_worker_unregister_tcp_connection(net_worker, client);

    return true;
}

bool
co_net_worker_register_udp(
    co_net_worker_t* net_worker,
    co_udp_t* udp
)
{
    if (net_worker->udps == NULL)
    {
        net_worker->udps = co_list_create(NULL);
    }

#ifdef CO_OS_WIN
    if (!co_net_selector_register(
        net_worker->net_selector, &udp->sock, 0))
    {
        return false;
    }
#else
    if (!co_net_selector_register(
        net_worker->net_selector, &udp->sock, udp->sock_event_flags))
    {
        return false;
    }
#endif

    co_list_add_tail(net_worker->udps, udp);

    return true;
}

void
co_net_worker_unregister_udp(
    co_net_worker_t* net_worker,
    co_udp_t* udp
)
{
    if (net_worker->udps == NULL)
    {
        return;
    }

    co_list_iterator_t* it =
        co_list_find(net_worker->udps, udp);

    if (it == NULL)
    {
        return;
    }

    co_list_remove_at(net_worker->udps, it);

    co_net_selector_unregister(
        net_worker->net_selector, &udp->sock);
}

#ifndef CO_OS_WIN
bool
co_net_worker_set_tcp_send(
    co_net_worker_t* net_worker,
    co_tcp_client_t* client,
    bool enable
)
{
    uint32_t flags =
        (CO_SOCKET_EVENT_RECEIVE | CO_SOCKET_EVENT_CLOSE);

    if (enable)
    {
        flags |= CO_SOCKET_EVENT_SEND;
    }

    if (!co_net_selector_update(
        net_worker->net_selector, &client->sock, flags))
    {
        return false;
    }

    return true;
}

bool
co_net_worker_update_udp(
    co_net_worker_t* net_worker,
    co_udp_t* udp
)
{
    if (net_worker->udps != NULL)
    {
        if (co_list_contains(net_worker->udps, udp))
        {
            return co_net_selector_update(
                net_worker->net_selector, &udp->sock, udp->sock_event_flags);
        }
    }

    return co_net_worker_register_udp(net_worker, udp);
}
#endif // !CO_OS_WIN
