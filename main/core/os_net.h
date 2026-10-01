#pragma once

// Internal glue between os_cloud and os_api, not for apps. The cloud task is
// the badge's only network worker: it says hello, runs the device API queue
// and flushes the outbox, so there is never more than one HTTP request (and
// one TLS session, ~30 KB) in flight.

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
