#pragma once

// Badge <-> server link (server/API.md). Once Wi-Fi has an IP, a background
// task says hello to the server, which assigns the badge an ID and, until
// someone claims it, a claim URL that the "My Badge" app shows as a QR code.
// Portable: the emulator runs this same file over its own os_http transport.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OS_CLOUD_OFFLINE,       // no Wi-Fi yet, or no server configured
    OS_CLOUD_CONNECTING,    // hello in flight
    OS_CLOUD_UNCLAIMED,     // have an ID and a claim URL
    OS_CLOUD_CLAIMED,       // linked to a user account
    OS_CLOUD_ERROR,         // last hello failed; retrying
    OS_CLOUD_LINKED,        // the server knows this badge as claimed but it lost its secret
                            // (NVS erased): the owner must release it; see `message`
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
    // Owner's game state from the last hello (server/API.md), valid when claimed.
    // Kept in NVS, so it is there offline (Wi-Fi is off at every boot).
    char user[9];           // owner tag; a change wipes /sdcard/hh (new owner)
    int lamps;              // !LAMPs balance
    int level;              // X Dex Level
    int packs;              // unopened packs
    bool has_key;           // mesh key stored
    int mesh_channel;       // from hello; default CONFIG_SOLANA_OS_MESH_CHANNEL
    bool claimed;           // linked to an owner (as of the last hello, possibly an earlier boot)
    int64_t synced_ms;      // esp_timer ms of the last good hello this boot, 0 = none yet
    char message[160];      // OS_CLOUD_LINKED: the server's explanation (ASCII), else ""
    bool welcome_needs_x;   // hello "welcome":"needs_x": the owner's welcome waits for a linked X account
    // Software update offered by the last good hello (server/API.md `update`,
    // core/os_ota.h): version "" = none. Not kept across boots.
    char update[32];
    int update_size;        // bytes of the image
    char update_sha[65];    // its SHA-256, hex
} os_cloud_info_t;

void os_cloud_init(void);                   // after os_wifi_init()
void os_cloud_get_info(os_cloud_info_t *out);
void os_cloud_refresh(void);                // say hello now instead of at the next poll
void os_cloud_set_server(const char *url);  // saved to NVS; "" clears

// For os_api / os_mesh (any task).
bool os_cloud_bearer(char *out, size_t n);  // badge secret; false if none
bool os_cloud_mesh_key(uint8_t out[16]);    // false if none
int64_t os_cloud_time_ms(void);             // server clock estimate (unix ms), 0 = never synced
void os_cloud_note_lamps(int lamps, int level);  // newer figures from another API answer
void os_cloud_note_packs(int packs);        // e.g. after opening or buying a pack

// Factory reset (docs/HHGTS.md): if online, POST /api/device/reset; then wipe
// /sdcard/hh, erase NVS and reboot. Runs on the cloud task; returns at once.
void os_cloud_factory_reset(void);
