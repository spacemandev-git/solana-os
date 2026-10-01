#pragma once

// Badge-to-badge trades (docs/MESH.md §3). The service owns the state machine
// and keeps invites (60 s) while the Trade app is closed (os_mesh raises the
// toast); the app drives it. Signed trades
// are recorded in /sdcard/hh/trades.jsonl and uploaded to
// POST /api/device/trades (directly when online, else through the outbox);
// pending ones are polled with GET /api/device/trades/:tid until they settle.
// OS task only (see game.h).

#include <stdbool.h>
#include <stdint.h>
#include "core/os_mesh.h"
#include "game.h"

#define TRADE_MAX_ITEMS     6           // per side: keeps offers and terms in one mesh message
#define TRADE_MAX_PAGES     6           // page copies per side
#define TRADE_ID_LEN        48          // page ids: server allows 48
#define TRADE_NAME_LEN      40
#define TRADE_INVITES_MAX   3
#define TRADE_INVITE_TTL_MS 60000

typedef struct {
    char id[TRADE_ID_LEN + 1];
    char name[TRADE_NAME_LEN + 1];      // display only
} trade_thing_t;

typedef struct {
    int lamps;
    int nitems, npages;
    trade_thing_t items[TRADE_MAX_ITEMS];
    trade_thing_t pages[TRADE_MAX_PAGES];   // one entry per copy
} trade_bundle_t;

typedef enum {
    TRADE_IDLE,
    TRADE_INVITING,     // we invited, waiting for join / decline
    TRADE_OPEN,         // both sides building offers
    TRADE_SIGNING,      // terms sent (inviter) or received; waiting for the other signature
    TRADE_DONE,         // both signed: recorded and uploading
    TRADE_CANCELLED,
} trade_phase_t;

typedef struct {
    trade_phase_t phase;
    bool inviter;                       // we are A
    char tid[17];
    char peer[OS_MESH_ID_LEN + 1];
    char label[GAME_CARD_NAME];
    trade_bundle_t mine, theirs;
    int my_rev, their_rev;
    bool i_locked, they_locked;         // valid for the current revisions
    bool can_sign;                      // inviter, both locked: trade_sign() sends our signature
    bool settled, rejected;             // the server's answer, once it came (TRADE_DONE)
    char reason[48];                    // why it was cancelled or rejected
} trade_view_t;

typedef struct {
    char tid[17];
    char peer[OS_MESH_ID_LEN + 1];
    char label[GAME_CARD_NAME];
} trade_invite_t;

// Incoming invites, newest first.
int trade_invite_count(void);
const trade_invite_t *trade_invite(int i);
bool trade_join(const char *tid, const char **why);
void trade_decline(const char *tid);

bool trade_invite_peer(const char *peer_id, const char **why);

// The trade in progress (or the one that just ended); NULL when idle.
const trade_view_t *trade_current(void);
void trade_dismiss(void);               // forget a finished/cancelled trade
void trade_cancel(void);                // allowed until we sign

// Offer edits (TRADE_OPEN). Each one is a new revision and clears both locks.
bool trade_set_lamps(int lamps);
bool trade_add(bool page, const char *id, const char *name, const char **why);
void trade_remove(bool page, int index);
int trade_count_of(bool page, const char *id);  // copies of id in my offer
// Lock = happy with both offers as they are now. For the joiner (B) a lock
// is final: B signs as soon as valid terms arrive. The inviter (A) signs with
// trade_sign() once both are locked (after the user confirms). Once a
// signature is out the trade can settle, whatever happens next.
void trade_lock(bool on);
void trade_sign(void);

// Recorded trades, newest first (malloc'd *out, free it).
typedef enum { TRADE_REC_PENDING, TRADE_REC_UNCONFIRMED, TRADE_REC_SETTLED, TRADE_REC_REJECTED } trade_rec_status_t;
typedef struct {
    char tid[17];
    char label[GAME_CARD_NAME];
    char gave[48], got[48];
    char reason[40];
    trade_rec_status_t status;
    int64_t at;
} trade_rec_t;
int trade_records_load(trade_rec_t **out, int max);
void trade_poll_now(void);              // check pending trades with the server now
