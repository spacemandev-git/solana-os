// The running Lua app's WebSocket, see os_socket.h.
//
// One slot, under s_lock: what the app opened (the URL until the cloud task
// takes it), the events waiting for the app and the messages waiting to go
// out. The cloud task runs the connection without the lock and checks the
// slot between steps: each step sends one queued message, then reads for up
// to POLL_MS (no wait while more are queued), so a send goes out within
// ~50 ms. Every open and close bumps the slot's generation, so a run whose
// socket the app closed only sends what was queued (handed over in s_flush),
// takes its connection down and leaves the next socket alone.

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "os_http.h"
#include "os_net.h"
#include "os_socket.h"
#include "os_wifi.h"
#include "os_ws.h"

static const char *TAG = "os_socket";

#define IN_MAX          8               // messages waiting for the app: then the host waits
#define OUT_MAX         8               // messages waiting to go out: then send() says "too fast"
#define POLL_MS         50

typedef struct msg {
    struct msg *next;
    size_t len;
    char *data;                         // after the struct (out), or os_ws_recv's buffer (in)
} msg_t;

enum { S_FREE, S_PENDING, S_OPEN, S_ENDED };

static SemaphoreHandle_t s_lock;
static volatile bool s_active;          // os_socket_active(), read without the lock
static msg_t *s_flush;                  // what a socket the app closed still sends
static uint32_t s_flush_gen;
static struct {
    uint8_t state;
    uint32_t gen;
    void *ctx;
    os_socket_wake_t wake;
    char *url, *protocols;              // S_PENDING, until the cloud task takes them
    bool open_ev;                       // OPEN not taken yet
    const char *reason;                 // S_ENDED: the CLOSE not taken yet
    int code;
    msg_t *in, *out;
    int nin, nout;
} s;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void free_msg(msg_t *m)
{
    if (m->data != (char *)(m + 1)) {
        free(m->data);
    }
    free(m);
}

static void free_list(msg_t **l)
{
    while (*l) {
        msg_t *m = *l;
        *l = m->next;
        free_msg(m);
    }
}

// Back to free; a run in progress sees the new generation.
static void reset_locked(void)
{
    free(s.url);
    free(s.protocols);
    free_list(&s.in);
    free_list(&s.out);
    uint32_t gen = s.gen + 1;
    memset(&s, 0, sizeof(s));
    s.gen = gen;
    s_active = false;
}

// Tell the app an event waits, if gen is still its socket.
static void notify(uint32_t gen)
{
    lock();
    os_socket_wake_t wake = s.gen == gen ? s.wake : NULL;
    void *ctx = s.ctx;
    unlock();
    if (wake) {
        wake(ctx);
    }
}

esp_err_t os_socket_open(const char *url, const char *protocols, os_socket_wake_t wake, void *ctx)
{
    if (!ctx || !url || strncmp(url, "wss://", 6) || strlen(url) > OS_SOCKET_URL_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char *u = strdup(url), *p = protocols ? strdup(protocols) : NULL;
    if (!u || (protocols && !p)) {
        free(u);
        free(p);
        return ESP_ERR_NO_MEM;
    }
    lock();
    if (s.state != S_FREE) {
        unlock();
        free(u);
        free(p);
        return ESP_ERR_INVALID_STATE;
    }
    s.gen++;
    s.state = S_PENDING;
    s.ctx = ctx;
    s.wake = wake;
    s.url = u;
    s.protocols = p;
    s_active = true;
    unlock();
    os_cloud_kick();
    return ESP_OK;
}

bool os_socket_next(void *ctx, os_socket_ev_t *ev)
{
    memset(ev, 0, sizeof(*ev));
    bool got = true;
    lock();
    if (!ctx || s.ctx != ctx) {
        got = false;
    } else if (s.open_ev) {
        s.open_ev = false;
        ev->kind = OS_SOCKET_EV_OPEN;
    } else if (s.in) {
        msg_t *m = s.in;
        s.in = m->next;
        s.nin--;
        ev->kind = OS_SOCKET_EV_MESSAGE;
        ev->data = m->data;                 // os_ws_recv's buffer: the taker frees it
        ev->len = m->len;
        free(m);
    } else if (s.state == S_ENDED) {
        ev->kind = OS_SOCKET_EV_CLOSE;
        ev->reason = s.reason;
        ev->code = s.code;
        reset_locked();
    } else {
        got = false;
    }
    unlock();
    return got;
}

esp_err_t os_socket_send(void *ctx, const char *text, size_t len)
{
    if (len > OS_SOCKET_MSG_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = ESP_OK;
    lock();
    if (!ctx || s.ctx != ctx || s.state != S_OPEN) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s.nout >= OUT_MAX) {
        err = ESP_ERR_NO_MEM;
    } else {
        msg_t *m = malloc(sizeof(*m) + len + 1);
        if (!m) {
            err = ESP_ERR_NO_MEM;
        } else {
            m->next = NULL;
            m->len = len;
            m->data = (char *)(m + 1);
            memcpy(m->data, text, len);
            m->data[len] = 0;
            msg_t **tail = &s.out;
            while (*tail) {
                tail = &(*tail)->next;
            }
            *tail = m;
            s.nout++;
        }
    }
    unlock();
    return err;
}

void os_socket_close(void *ctx)
{
    lock();
    if (ctx && s.ctx == ctx) {
        if (s.state == S_OPEN && s.out) {
            free_list(&s_flush);
            s_flush = s.out;
            s_flush_gen = s.gen;
            s.out = NULL;
        }
        reset_locked();
    }
    unlock();
}

bool os_socket_active(void)
{
    return s_active;
}

// ---------------------------------------------------------------------------
// The connection (cloud task)

// A connection that broke: Wi-Fi going takes it down before we notice.
static const char *broken(void)
{
    return os_wifi_state() == OS_WIFI_CONNECTED ? "lost" : "offline";
}

// One step of an open socket. NULL: go on; else why it ends, with the code to
// close with (*out_code) and the host's own (*code).
static const char *step(uint32_t gen, int *code, int *out_code)
{
    lock();
    bool mine = s.gen == gen;
    msg_t *m = mine ? s.out : NULL, *flush = NULL;
    if (m) {
        s.out = m->next;
        s.nout--;
    }
    if (!mine && s_flush_gen == gen) {
        flush = s_flush;
        s_flush = NULL;
    }
    bool more = mine && s.out, room = s.nin < IN_MAX;
    unlock();
    if (!mine) {
        // The app closed it (or exited): what it sent before still goes out.
        for (bool ok = true; flush;) {
            msg_t *next = flush->next;
            ok = ok && os_ws_send(flush->data, flush->len) == ESP_OK;
            free_msg(flush);
            flush = next;
        }
        *out_code = 1000;
        return "closed";
    }
    if (m) {
        esp_err_t err = os_ws_send(m->data, m->len);
        free_msg(m);
        if (err != ESP_OK) {
            return broken();
        }
    }
    if (os_wifi_state() != OS_WIFI_CONNECTED) {
        return "offline";
    }
    if (!room) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS)); // the app hasn't taken its messages yet
        return NULL;
    }
    char *data;
    size_t len;
    int r = os_ws_recv(&data, &len, OS_SOCKET_MSG_MAX, m || more ? 0 : POLL_MS, code);
    if (r == OS_WS_MSG) {
        msg_t *in = malloc(sizeof(*in));
        if (!in) {
            free(data);
            return "not enough memory";
        }
        *in = (msg_t){ .len = len, .data = data };
        lock();
        if (s.gen == gen) {
            msg_t **tail = &s.in;
            while (*tail) {
                tail = &(*tail)->next;
            }
            *tail = in;
            s.nin++;
            in = NULL;
        }
        unlock();
        if (in) {
            free_msg(in);
        } else {
            notify(gen);
        }
        return NULL;
    }
    if (r == OS_WS_TIMEOUT) {
        return NULL;
    }
    if (r == OS_WS_CLOSED) {
        return "host closed";
    }
    if (r == OS_WS_TOO_BIG) {
        *out_code = 1009;
        return "too big";
    }
    return broken();
}

bool os_socket_service(bool online)
{
    lock();
    if (s.state != S_PENDING) {
        unlock();
        return false;
    }
    uint32_t gen = s.gen;
    char *url = s.url, *protocols = s.protocols;
    s.url = s.protocols = NULL;
    unlock();

    const char *why = !online ? "offline" : NULL;
    int code = 0, out_code = 0;
    if (!why) {
        os_http_hangup();                   // one TLS connection at a time: the socket takes its place
        esp_err_t err = os_ws_connect(url, protocols);
        why = err == ESP_OK ? NULL : err == ESP_ERR_NO_MEM ? "not enough memory"
            : err == ESP_ERR_INVALID_ARG ? "host not allowed"
            : os_wifi_state() != OS_WIFI_CONNECTED ? "offline" : "unreachable";
    }
    free(url);
    free(protocols);
    if (!why) {
        lock();
        bool mine = s.gen == gen;
        if (mine) {
            s.state = S_OPEN;
            s.open_ev = true;
        }
        unlock();
        notify(gen);
        while (!why) {
            why = step(gen, &code, &out_code);
        }
        os_ws_close(out_code);
    }
    ESP_LOGI(TAG, "socket ended: %s", why);

    lock();
    bool mine = s.gen == gen;
    if (mine) {
        s.state = S_ENDED;
        s.reason = why;
        s.code = !strcmp(why, "host closed") ? code : 0;
        free_list(&s.out);
        s.nout = 0;
    }
    unlock();
    notify(gen);
    return true;
}

void os_socket_abort(void)
{
    lock();
    reset_locked();
    free_list(&s_flush);
    unlock();
}

void os_socket_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}
