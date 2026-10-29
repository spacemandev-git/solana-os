#pragma once

// Internal glue between os_cloud, os_api and os_socket, not for apps. The
// cloud task is the badge's only network worker: it says hello, runs the
// device API queue, flushes the outbox and runs a Lua app's socket, so there
// is never more than one HTTP request (and one TLS session, ~30 KB) in flight.

#include <stdbool.h>
#include <stddef.h>

// os_cloud.c
void os_cloud_kick(void);                   // API work queued: wake the cloud task
bool os_cloud_registered(void);             // server set and badge id + secret known
void os_cloud_server(char *out, size_t n);  // configured origin, "" if none

// os_api.c, called from the cloud task.
void os_api_init(void);
// Run one queued request, else send one outbox entry. online: Wi-Fi joined
// and registered; when false, queued requests fail with status 0 instead.
// Returns true if it did something (call again before sleeping).
bool os_api_service(bool online);
void os_outbox_clear(void);                 // owner change / factory reset

// os_socket.c, called from the cloud task.
void os_socket_init(void);
// Run the socket an app opened until it ends, in place of the server
// connection. online: Wi-Fi joined and registered; when false it ends at once
// ("offline"). Returns true if there was one.
bool os_socket_service(bool online);
void os_socket_abort(void);                 // factory reset: end it without events
