// FreeRTOS for the browser: every task is an Emscripten fiber (ASYNCIFY).
//
// The page has one thread and must never block, so tasks are cooperative. The
// scheduler loop (emu_main.c) calls emu_sched_run(), which switches into each
// runnable task; the task runs until it blocks (vTaskDelay, a queue or
// semaphore wait, ...) and switches back. Those are the same points where a
// badge task would block, so firmware keeps its FreeRTOS structure unchanged.
// Nothing preempts a task, which is why critical sections are no-ops.

#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include <emscripten/fiber.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "emu.h"

static const char *TAG = "freertos";

// wasm frames are bigger than RISC-V ones, and screenshots render on the OS
// task, so give every task a roomy stack instead of its badge size.
#define TASK_C_STACK        (160 * 1024)
#define TASK_ASYNC_STACK    (64 * 1024)
#define TICK_US             (1000000LL / configTICK_RATE_HZ)
#define MAX_PASSES          16

struct emu_task {
    emscripten_fiber_t fiber;
    void *c_stack, *async_stack;
    TaskFunction_t fn;
    void *arg;
    char name[16];
    UBaseType_t prio;
    int64_t wake_us;            // INT64_MAX: no timeout
    emu_ready_fn ready;         // what the task waits for, NULL = just the time
    void *ready_obj;
    bool dead;
    uint32_t notify_value;
    bool notify_pending;
    EventGroupHandle_t wait_group;
    EventBits_t wait_bits;
    bool wait_all;
    struct emu_task *next;
};

static emscripten_fiber_t s_main_fiber;
static uint8_t s_main_async[TASK_ASYNC_STACK];
static struct emu_task *s_tasks, *s_current;

int64_t emu_ticks_to_deadline(uint32_t ticks)
{
    if (ticks == portMAX_DELAY) {
        return INT64_MAX;
    }
    return emu_now_us() + (int64_t)ticks * TICK_US;
}

bool emu_in_task(void)
{
    return s_current != NULL;
}

static void yield_to_scheduler(void)
{
    struct emu_task *t = s_current;
    emscripten_fiber_swap(&t->fiber, &s_main_fiber);
}

bool emu_block_until(emu_ready_fn ready, void *obj, int64_t deadline_us)
{
    for (;;) {
        if (ready && ready(obj)) {
            return true;
        }
        if (!s_current || emu_now_us() >= deadline_us) {
            // Outside a task nothing can wait; callers treat this as a timeout.
            return false;
        }
        s_current->ready = ready;
        s_current->ready_obj = obj;
        s_current->wake_us = deadline_us;
        yield_to_scheduler();
        s_current->ready = NULL;
        s_current->ready_obj = NULL;
    }
}

static bool runnable(const struct emu_task *t, int64_t now)
{
    return !t->dead && (now >= t->wake_us || (t->ready && t->ready(t->ready_obj)));
}

static void reap(void)
{
    struct emu_task **pp = &s_tasks;
    while (*pp) {
        struct emu_task *t = *pp;
        if (t->dead) {
            *pp = t->next;
            free(t->c_stack);
            free(t->async_stack);
            free(t);
        } else {
            pp = &t->next;
        }
    }
}

void emu_sched_init(void)
{
    emscripten_fiber_init_from_current_context(&s_main_fiber, s_main_async, sizeof(s_main_async));
}

void emu_sched_run(void)
{
    for (int pass = 0; pass < MAX_PASSES; pass++) {
        bool ran = false;
        for (struct emu_task *t = s_tasks; t; t = t->next) {
            if (!runnable(t, emu_now_us())) {
                continue;
            }
            s_current = t;
            emscripten_fiber_swap(&s_main_fiber, &t->fiber);
            s_current = NULL;
            ran = true;
        }
        reap();
        if (!ran) {
            break;
        }
    }
}

int64_t emu_sched_next_wake_us(void)
{
    int64_t next = INT64_MAX, now = emu_now_us();
    for (struct emu_task *t = s_tasks; t; t = t->next) {
        if (t->dead) {
            continue;
        }
        if (t->ready && t->ready(t->ready_obj)) {
            return now;
        }
        if (t->wake_us < next) {
            next = t->wake_us;
        }
    }
    return next;
}

// ---------------------------------------------------------------------------
// Tasks

static void trampoline(void *arg)
{
    struct emu_task *t = arg;
    t->fn(t->arg);
    // FreeRTOS tasks must not return; treat it like vTaskDelete(NULL).
    vTaskDelete(NULL);
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out)
{
    (void)stack;
    struct emu_task *t = calloc(1, sizeof(*t));
    if (!t) {
        return pdFAIL;
    }
    // wasm code assumes a 16-byte aligned stack pointer (LLVM folds frame
    // offsets into ORs), and malloc only guarantees 8: align explicitly.
    t->c_stack = aligned_alloc(16, TASK_C_STACK);
    t->async_stack = malloc(TASK_ASYNC_STACK);
    if (!t->c_stack || !t->async_stack) {
        free(t->c_stack);
        free(t->async_stack);
        free(t);
        return pdFAIL;
    }
    t->fn = fn;
    t->arg = arg;
    t->prio = prio;
    strncpy(t->name, name ? name : "task", sizeof(t->name) - 1);
    emscripten_fiber_init(&t->fiber, trampoline, t, t->c_stack, TASK_C_STACK, t->async_stack, TASK_ASYNC_STACK);

    // Keep the list in priority order so a woken high-priority task runs first.
    struct emu_task **pp = &s_tasks;
    while (*pp && (*pp)->prio >= prio) {
        pp = &(*pp)->next;
    }
    t->next = *pp;
    *pp = t;
    if (out) {
        *out = t;
    }
    ESP_LOGD(TAG, "task %s created", t->name);
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    (void)core;
    return xTaskCreate(fn, name, stack, arg, prio, out);
}

void vTaskDelete(TaskHandle_t t)
{
    if (!t) {
        t = s_current;
    }
    if (!t) {
        return;
    }
    t->dead = true;
    if (t == s_current) {
        yield_to_scheduler();       // never resumed; reaped on the main stack
        abort();
    }
}

void vTaskDelay(TickType_t ticks)
{
    emu_block_until(NULL, NULL, ticks ? emu_ticks_to_deadline(ticks) : emu_now_us());
}

void taskYIELD(void)
{
    if (s_current) {
        s_current->ready = NULL;
        s_current->wake_us = emu_now_us();
        yield_to_scheduler();
    }
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(emu_now_us() / TICK_US);
}

BaseType_t xTaskDelayUntil(TickType_t *prev, TickType_t inc)
{
    TickType_t target = *prev + inc;
    TickType_t now = xTaskGetTickCount();
    *prev = target;
    if ((int32_t)(target - now) <= 0) {
        return pdFALSE;
    }
    vTaskDelay(target - now);
    return pdTRUE;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return s_current;
}

char *pcTaskGetName(TaskHandle_t t)
{
    t = t ? t : s_current;
    return t ? t->name : "main";
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t)
{
    (void)t;
    return TASK_C_STACK / 2;
}

void vTaskSuspendAll(void)
{
}

BaseType_t xTaskResumeAll(void)
{
    return pdFALSE;
}

// ---- Notifications ----

static bool notify_ready(void *obj)
{
    return ((struct emu_task *)obj)->notify_value != 0;
}

static bool notify_pending(void *obj)
{
    return ((struct emu_task *)obj)->notify_pending;
}

BaseType_t xTaskGenericNotify(TaskHandle_t t, uint32_t value, eNotifyAction action)
{
    if (!t) {
        return pdFAIL;
    }
    switch (action) {
    case eSetBits:              t->notify_value |= value; break;
    case eIncrement:            t->notify_value++; break;
    case eSetValueWithOverwrite: t->notify_value = value; break;
    case eSetValueWithoutOverwrite:
        if (t->notify_pending) {
            return pdFAIL;
        }
        t->notify_value = value;
        break;
    default: break;
    }
    t->notify_pending = true;
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    struct emu_task *t = s_current;
    if (!t) {
        return 0;
    }
    emu_block_until(notify_ready, t, emu_ticks_to_deadline(ticks));
    uint32_t v = t->notify_value;
    if (v) {
        t->notify_value = clear ? 0 : v - 1;
    }
    t->notify_pending = false;
    return v;
}

BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t *value, TickType_t ticks)
{
    struct emu_task *t = s_current;
    if (!t) {
        return pdFALSE;
    }
    if (!t->notify_pending) {
        t->notify_value &= ~clear_on_entry;
    }
    bool got = emu_block_until(notify_pending, t, emu_ticks_to_deadline(ticks));
    if (value) {
        *value = t->notify_value;
    }
    if (got) {
        t->notify_value &= ~clear_on_exit;
        t->notify_pending = false;
    }
    return got ? pdTRUE : pdFALSE;
}

// ---------------------------------------------------------------------------
// Queues

struct emu_queue {
    uint8_t *buf;
    UBaseType_t len, item, head, count;
};

static bool q_can_recv(void *o)
{
    return ((struct emu_queue *)o)->count > 0;
}

static bool q_can_send(void *o)
{
    struct emu_queue *q = o;
    return q->count < q->len;
}

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size)
{
    struct emu_queue *q = calloc(1, sizeof(*q));
    if (!q) {
        return NULL;
    }
    q->buf = calloc(len ? len : 1, item_size ? item_size : 1);
    if (!q->buf) {
        free(q);
        return NULL;
    }
    q->len = len;
    q->item = item_size;
    return q;
}

void vQueueDelete(QueueHandle_t q)
{
    if (q) {
        free(q->buf);
        free(q);
    }
}

BaseType_t xQueueGenericSend(QueueHandle_t q, const void *item, TickType_t ticks, int front)
{
    if (!emu_block_until(q_can_send, q, emu_ticks_to_deadline(ticks))) {
        return errQUEUE_FULL;
    }
    UBaseType_t slot;
    if (front) {
        q->head = (q->head + q->len - 1) % q->len;
        slot = q->head;
    } else {
        slot = (q->head + q->count) % q->len;
    }
    memcpy(q->buf + slot * q->item, item, q->item);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item)
{
    if (q->count == q->len) {
        q->head = (q->head + 1) % q->len;
        q->count--;
    }
    return xQueueGenericSend(q, item, 0, 0);
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    if (!emu_block_until(q_can_recv, q, emu_ticks_to_deadline(ticks))) {
        return pdFALSE;
    }
    memcpy(item, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->len;
    q->count--;
    return pdTRUE;
}

BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t ticks)
{
    if (!emu_block_until(q_can_recv, q, emu_ticks_to_deadline(ticks))) {
        return pdFALSE;
    }
    memcpy(item, q->buf + q->head * q->item, q->item);
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    return q->count;
}

UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q)
{
    return q->len - q->count;
}

BaseType_t xQueueReset(QueueHandle_t q)
{
    q->head = q->count = 0;
    return pdPASS;
}

// ---------------------------------------------------------------------------
// Semaphores and mutexes

struct emu_sem {
    int count, max;
    bool mutex;
    struct emu_task *owner;
    int depth;                  // recursive mutex nesting
};

static bool sem_ready(void *o)
{
    return ((struct emu_sem *)o)->count > 0;
}

SemaphoreHandle_t emu_sem_create(int count, int max, bool mutex)
{
    struct emu_sem *s = calloc(1, sizeof(*s));
    if (s) {
        s->count = count;
        s->max = max;
        s->mutex = mutex;
    }
    return s;
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    free(s);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    if (!s || !emu_block_until(sem_ready, s, emu_ticks_to_deadline(ticks))) {
        return pdFALSE;
    }
    s->count--;
    s->owner = s_current;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (!s || s->count >= s->max) {
        return pdFALSE;
    }
    s->count++;
    s->owner = NULL;
    return pdTRUE;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks)
{
    if (s->depth && s->owner == s_current) {
        s->depth++;
        return pdTRUE;
    }
    if (!xSemaphoreTake(s, ticks)) {
        return pdFALSE;
    }
    s->depth = 1;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s)
{
    if (!s->depth || s->owner != s_current) {
        return pdFALSE;
    }
    if (--s->depth == 0) {
        xSemaphoreGive(s);
    }
    return pdTRUE;
}

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s)
{
    return s->count;
}

// ---------------------------------------------------------------------------
// Event groups

struct emu_evgroup {
    EventBits_t bits;
};

static bool group_ready(void *o)
{
    struct emu_task *t = o;
    EventBits_t have = t->wait_group->bits & t->wait_bits;
    return t->wait_all ? have == t->wait_bits : have != 0;
}

EventGroupHandle_t xEventGroupCreate(void)
{
    return calloc(1, sizeof(struct emu_evgroup));
}

void vEventGroupDelete(EventGroupHandle_t g)
{
    free(g);
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits)
{
    g->bits |= bits;
    return g->bits;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits)
{
    EventBits_t old = g->bits;
    g->bits &= ~bits;
    return old;
}

EventBits_t xEventGroupGetBits(EventGroupHandle_t g)
{
    return g->bits;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits, BaseType_t clear, BaseType_t all,
                                TickType_t ticks)
{
    struct emu_task self = {0}, *t = s_current ? s_current : &self;
    t->wait_group = g;
    t->wait_bits = bits;
    t->wait_all = all;
    bool ok = emu_block_until(group_ready, t, emu_ticks_to_deadline(ticks));
    EventBits_t now = g->bits;
    if (ok && clear) {
        g->bits &= ~bits;
    }
    t->wait_group = NULL;
    return now;
}
