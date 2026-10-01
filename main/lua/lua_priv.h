#pragma once

// Internals shared by the Lua host's files. lua_host.h is the public side.
//
// Threads: the script runs on the "lua" task (lua_host.c), which holds the
// LVGL lock around every callback. The OS task feeds it keys, API answers
// arrive as OS-task callbacks (os_api) and mesh messages on the mesh task;
// both only queue events (lh_ev_t) and fill call records under lh_mx.

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "core/os.h"
#include "core/ui.h"
#include "vendor/lua.h"
#include "vendor/lauxlib.h"
#include "lua_host.h"
#include "lua_pkg.h"

#define LH_OBJ_MAX          64          // UI objects at a time
#define LH_TEXT_MAX         200         // bytes per label; longer text ends in "..."
#define LH_TEXT_TOTAL       (6 * 1024)  // label text per app (LVGL keeps it on the system heap)
#define LH_TIMER_MAX        8
#define LH_TIMER_MIN_MS     20
#define LH_TICK_MS          50          // on_tick period
#define LH_STORAGE_MAX      (16 * 1024) // data.json
#define LH_STORAGE_SAVE_MS  1000        // storage.set writes at most this often
#define LH_MESH_PER_S       4
#define LH_MESH_MAX         200         // bytes of JSON per mesh.send
#define LH_CALLS_MAX        16          // server calls waiting or in flight per app
#define LH_CALLS_SENT       3           // at once in os_api, whose queue the OS shares
#define LH_JSON_DEPTH       16          // nesting; bounds the decoder's recursion
#define LH_LED_COUNT        8

typedef enum { LH_LABEL, LH_RECT, LH_BAR, LH_GRADIENT, LH_LIST, LH_KIND_COUNT } lh_kind_t;

typedef struct {
    lv_obj_t *obj;              // NULL: free slot
    ui_list_t *list;            // LH_LIST
    int items_ref, select_ref;  // LH_LIST: registry refs
    uint32_t order;             // creation order: the newest visible list gets Up/Down
    uint16_t gen;               // bumped when freed, so stale handles fail
    uint16_t text_len;          // LH_LABEL: bytes counted in lh_t.text_total
    uint8_t kind;
} lh_obj_t;

typedef struct {
    int id;                     // 0: free
    int fn_ref;
    uint32_t period;            // 0: once
    int64_t due;
} lh_timer_t;

// One server request (lua_net.c). The Lua task owns the list; the OS-task
// answer callback finds a record by its slot and fills it in under lh_mx.
typedef struct lh_call {
    struct lh_call *next;
    uint32_t token;             // os_confirm ctx: a number, never a pointer
    int8_t slot;                // os_api ctx while sent: &lh_net's slot array
    int cb_ref;
    uint8_t op;
    uint8_t state;              // LH_CALL_*
    bool done;
    int status;                 // HTTP status, 0 = no answer
    char *body;
    const char *err;            // local outcome: "denied", "offline", ...
    char *method, *path, *req;  // until it's sent
} lh_call_t;

enum { LH_CALL_QUEUED, LH_CALL_CONFIRM, LH_CALL_SENT };

typedef struct lh {
    lua_State *L;
    lua_pkg_t pkg;
    uint32_t gen;               // launch number; events carry it

    // Limits
    size_t mem, mem_peak;
    bool mem_hit;               // the allocator refused to go over the cap
    uint32_t steps, steps_peak;
    bool killed;                // over the instruction budget
    char kill_msg[64];          // "main.lua:12: Script took too long"
    size_t heap_before;         // system heap free at launch
    char err[96];               // why the script stopped, "" if it didn't
    int64_t t0;                 // ms at launch

    // Task and lifecycle
    TaskHandle_t task;
    volatile bool quit;         // the OS is closing the app
    bool exit_req;              // sys.exit()
    bool ui_off;                // shutting down without the LVGL lock: UI calls do nothing
    bool failed;                // the error screen is up
    bool finished, orphan;      // task end handshake (under lh_mx)

    // UI
    lv_obj_t *area, *hint, *modal, *loading;
    ui_kb_t *kb;
    int modal_ref;              // ui.confirm / ui.input callback
    lh_obj_t objs[LH_OBJ_MAX];
    uint32_t obj_order;
    size_t text_total;          // label text bytes on screen
    int key_refs[3];            // "title", "sub", "right": row lookups that never allocate

    // LEDs, shown at the end of each callback
    uint32_t led[LH_LED_COUNT];
    bool led_dirty, led_used;

    // Timers
    lh_timer_t timers[LH_TIMER_MAX];
    int timer_seq;
    int64_t next_tick;

    // storage.*
    int store_ref;
    char *store_pending;        // encoded data.json waiting to be written
    size_t store_len;
    int64_t store_saved;

    // mesh
    int mesh_sub;
    int64_t mesh_sent[LH_MESH_PER_S];

    // Server calls
    lh_call_t *calls;
} lh_t;

// Figures from the last run, for "lua mem".
typedef struct {
    char id[LUA_PKG_ID_MAX + 1];
    size_t mem_peak, mem_end;   // Lua heap; mem_end is what lua_close left (0)
    uint32_t steps_peak;        // most VM instructions in one callback
    uint32_t stack_free;        // lua task stack never used, bytes
    size_t heap_before;         // system heap free when the app started
    char err[96];               // last error, "" if none
} lh_stats_t;

extern lh_stats_t lh_stats;

// Events for the Lua task.
typedef enum { LH_EV_KEY, LH_EV_WAKE, LH_EV_MESH, LH_EV_QUIT } lh_ev_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t key;
    bool repeat;
    uint32_t gen;
    char *data;                 // LH_EV_MESH: "<from id>\0<json>", freed by the receiver
} lh_ev_t;

extern SemaphoreHandle_t lh_mx;     // guards call records, the mesh filter, lh_cur
extern lh_t *lh_cur;                // the running session (OS task view)

static inline lh_t *lh_get(lua_State *L)
{
    return *(lh_t **)lua_getextraspace(L);
}

int64_t lh_now(void);
bool lh_post(const lh_ev_t *ev);                // to the Lua task; false if full
void lh_need_perm(lua_State *L, uint32_t perm, const char *module);

// Runs fn(arg) protected, as one callback: a fresh instruction budget, LEDs
// shown after. fn builds the arguments and lua_call()s the script, so even
// "out of memory" while pushing them is caught. On error the app stops and
// shows the error screen (LVGL lock held); returns false.
bool lh_callback(lh_t *s, lua_CFunction fn, void *arg);
void lh_cmd_register(void);                             // lua_cmd.c

// lua_ui.c
void lh_open_ui(lua_State *L);
// Protected (inside a key callback): dialogs and the focused list take their
// keys first and may call the script. true: consumed.
bool lh_ui_key(lua_State *L, lh_t *s, os_key_t key, bool repeat);
void lh_ui_clear(lh_t *s);                              // delete every script object
void lh_ui_close_dialog(lh_t *s);                       // ui.confirm / ui.input, without calling back
void lh_ui_release(lh_t *s);                            // forget LVGL pointers (OS cleaned them)
lv_color_t lh_color(lua_State *L, int idx);

// lua_sys.c: led, timer, sys, badge, json, storage, wifi
void lh_open_sys(lua_State *L);
void lh_led_flush(lh_t *s);
void lh_led_off(void);
int64_t lh_timers_next(lh_t *s);                        // earliest due time, or INT64_MAX
void lh_timers_run(lh_t *s);
bool lh_storage_load(lh_t *s);
void lh_storage_flush(lh_t *s, bool force);

// lua_net.c: mesh, lamps, items, profile, quest
void lh_open_net(lua_State *L);
void lh_calls_run(lh_t *s);                             // answer finished calls
void lh_net_stop(lh_t *s);                              // OS task, after the Lua task ended
void lh_calls_free(lh_t *s);
void lh_mesh_event(lh_t *s, char *data);
void lh_mesh_start(lh_t *s);                            // OS task
void lh_mesh_mute(void);                                // stop queueing messages
void lh_mesh_stop(void);                                // OS task

// lua_json.c: JSON <-> Lua. Encoding never raises Lua errors (it may run
// with C memory to free); decoding may (out of memory).
typedef struct {
    char *p;
    size_t len, cap, max;
    const char *err;
} lh_jbuf_t;

bool lh_json_encode(lua_State *L, int idx, lh_jbuf_t *b);   // b->p is malloc'd; free it
const char *lh_json_decode(lua_State *L, const char *s, size_t len);  // NULL: value pushed
void lh_push_jbuf(lua_State *L, lh_jbuf_t *b);      // push b as a string and free it
void lh_open_json(lua_State *L);
