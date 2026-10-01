#pragma once

// X Dex: swap cards with nearby badges (docs/MESH.md §2). Runs in the
// background so requests from other badges wait (60 s) while the X Dex app is
// closed (os_mesh raises the toast); the app shows them and starts swaps. Collected cards go to
// /sdcard/hh/xdex.jsonl (RAM without a card) and upload to
// POST /api/device/xdex, directly when online, else through the outbox.
// OS task only (see game.h).

#include <stdbool.h>
#include <stdint.h>
#include "core/os_mesh.h"
#include "game.h"

#define XDEX_PENDING_MAX    4
#define XDEX_REQ_TTL_MS     60000       // MESH.md: requests stay pending for 60 s

typedef struct {
    char id[OS_MESH_ID_LEN + 1];        // badge id
    char n[GAME_CARD_NAME];             // owner label
    char x[GAME_CARD_X];                // X handle, "" if none
    int lv;
    int64_t at;                         // unix ms collected (0 = unknown)
    int bounty;                         // !LAMPs it paid, -1 = not known yet
} xdex_card_t;

typedef struct {
    xdex_card_t card;                   // the requester's card
    char nonce[17];                     // their nonce
    char mine[17];                      // our nonce once accepted
    int64_t at;                         // received / last answered (game_now_ms)
    int tries;                          // "ok" sends while waiting for "fin"
    bool accepted;
} xdex_req_t;

typedef enum {
    XDEX_IDLE,
    XDEX_ASKING,        // we sent a request and wait for "ok" / "no"
    XDEX_FINISHING,     // we accepted and wait for "fin"
    XDEX_DECLINED,
    XDEX_NO_ANSWER,
    XDEX_FAILED,        // the other badge stopped answering halfway
    XDEX_SWAPPED,
} xdex_phase_t;

typedef enum {
    XDEX_UP_NONE,
    XDEX_UP_SENDING,
    XDEX_UP_DONE,       // bounty / fresh valid
    XDEX_UP_QUEUED,     // in the outbox until Wi-Fi is on
    XDEX_UP_REFUSED,    // the server said no (error)
} xdex_upload_t;

typedef struct {
    xdex_phase_t phase;
    char peer[OS_MESH_ID_LEN + 1];
    char label[GAME_CARD_NAME];
    int64_t at;                         // phase change (game_now_ms)
    xdex_upload_t upload;
    int bounty;
    bool fresh;                         // a badge we hadn't collected before
    char error[48];
} xdex_swap_t;

// Incoming requests, newest first.
int xdex_pending_count(void);
const xdex_req_t *xdex_pending(int i);
void xdex_accept(const char *peer_id);
void xdex_decline(const char *peer_id);

// Ask a nearby badge for its card. false (and *why set) when we can't.
bool xdex_request(const char *peer_id, const char **why);

// The swap in progress or the last one, for the X Dex app.
const xdex_swap_t *xdex_last(void);

// Cards collected on this badge (SD file, else RAM), newest first, one per
// badge. Returns the count; *out is malloc'd (free it), NULL when empty.
int xdex_cards_load(xdex_card_t **out, int max);
