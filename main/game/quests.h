#pragma once

// Quests: talk to characters at beacons (docs/MESH.md §4/5, server/API.md
// "Beacon relay"). The badge sends RELAY requests to a beacon over ESP-NOW;
// the beacon forwards them to the server, so quests work with Wi-Fi off.
// The service runs the relay client (one request in flight, one retry after
// 3 s, a ping every 10 s while talking), applies dialog effects (toast, LED,
// !LAMPs, page, temporary app download into /sdcard/hh/apps/<id>/) and keeps
// the quest log. (os_mesh toasts when a beacon comes into range.)
// OS task only (see game.h).

#include <stdbool.h>
#include <stdint.h>
#include "core/os_mesh.h"
#include "game.h"

#define QUEST_OPTS_MAX  4
#define QUEST_LOG_MAX   8

typedef struct {
    char beacon[OS_MESH_ID_LEN + 1];
    char name[25];                      // character
    char title[33];
    uint32_t color;
    char quest_title[49];               // "" when no quest business
    char text[404];
    char opts[QUEST_OPTS_MAX][33];
    int nopts;
    bool end;                           // conversation over: SELECT closes
    bool waiting;                       // a request is in flight
    bool shown;                         // at least one answer arrived
    char error[48];                     // last request failed
} quest_dialog_t;

typedef enum { QDL_NONE, QDL_RUNNING, QDL_DONE, QDL_FAILED } quest_dl_state_t;

typedef struct {
    quest_dl_state_t state;
    char id[25];
    int off, total;
    char error[48];
} quest_download_t;

typedef struct {
    char id[33];
    char title[49];
    char status[12];                    // active | available | completed | locked
    char step[49];                      // current step title
    char at[25];                        // where: character name
} quest_entry_t;

// Talking. talk() opens the conversation with a beacon's character.
bool quests_talk(const char *beacon_id, const char **why);
void quests_choose(int option);
void quests_hangup(void);
const quest_dialog_t *quests_dialog(void);  // NULL when not talking

const quest_download_t *quests_download(void);

// Quest log: GET /api/device/quests when online, else a relay "log" through
// the nearest beacon, else the cached copy. refresh() starts a fetch.
void quests_log_refresh(void);
bool quests_log_busy(void);
int quests_log(const quest_entry_t **out);
bool quests_log_saved(void);                 // showing the cached copy, not a fresh answer
int64_t quests_log_at(void);                // unix ms the shown data was fetched, 0 = unknown
const char *quests_log_error(void);         // "" when fine
void quests_log_free(void);                 // the app is closing
