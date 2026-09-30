#pragma once

// Badge <-> server link (server/API.md). Once Wi-Fi has an IP, a background
// task says hello to the server, which assigns the badge an ID and, until
// someone claims it, a claim URL that the "My Badge" app shows as a QR code.
// Portable: the emulator runs this same file over its own os_http transport.

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    OS_CLOUD_OFFLINE,       // no Wi-Fi yet, or no server configured
    OS_CLOUD_CONNECTING,    // hello in flight
    OS_CLOUD_UNCLAIMED,     // have an ID and a claim URL
    OS_CLOUD_CLAIMED,       // linked to a user account
    OS_CLOUD_ERROR,         // last hello failed; retrying
} os_cloud_state_t;

typedef struct {
    os_cloud_state_t state;
    char id[16];            // server-assigned badge ID, "" until the first hello
    char claim_url[160];    // valid in OS_CLOUD_UNCLAIMED
    char owner[64];         // "@username", or the display name, or a short wallet, when claimed
    char server[96];        // configured server origin, "" = not set
    const char *error;      // static string, valid in OS_CLOUD_ERROR
    int http_status;        // status of the last response, 0 = none (network error)
    int retry_s;            // seconds until the next hello, -1 = not scheduled
    bool busy;              // a hello is in flight (any state)
} os_cloud_info_t;

void os_cloud_init(void);                   // after os_wifi_init()
void os_cloud_get_info(os_cloud_info_t *out);
void os_cloud_refresh(void);                // say hello now instead of at the next poll
void os_cloud_set_server(const char *url);  // saved to NVS; "" clears
