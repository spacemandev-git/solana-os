#pragma once

// FreeRTOS API for the emulator. Tasks are cooperative fibers (Emscripten
// ASYNCIFY) run by the scheduler loop in platform/freertos_emu.c. A task only
// gives up the CPU at a blocking call (delay, queue/semaphore wait), which is
// exactly where a firmware task would block on the badge, so firmware code
// runs unchanged. Critical sections are no-ops: nothing preempts a task.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t StackType_t;

#define pdTRUE          1
#define pdFALSE         0
#define pdPASS          1
#define pdFAIL          0
#define errQUEUE_FULL   0
#define errQUEUE_EMPTY  0

#define configTICK_RATE_HZ      CONFIG_FREERTOS_HZ
#define portTICK_PERIOD_MS      (1000 / configTICK_RATE_HZ)
#define portMAX_DELAY           ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms)       ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000))
#define pdTICKS_TO_MS(t)        ((uint32_t)(((uint64_t)(t) * 1000) / configTICK_RATE_HZ))
#define configMAX_PRIORITIES    25
#define tskIDLE_PRIORITY        0
#define tskNO_AFFINITY          0x7FFFFFFF
#define configMINIMAL_STACK_SIZE 768

typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED    { 0 }
#define portENTER_CRITICAL(m)           ((void)(m))
#define portEXIT_CRITICAL(m)            ((void)(m))
#define portENTER_CRITICAL_ISR(m)       ((void)(m))
#define portEXIT_CRITICAL_ISR(m)        ((void)(m))
#define portENTER_CRITICAL_SAFE(m)      ((void)(m))
#define portEXIT_CRITICAL_SAFE(m)       ((void)(m))
#define taskENTER_CRITICAL(m)           ((void)(m))
#define taskEXIT_CRITICAL(m)            ((void)(m))
#define portYIELD_FROM_ISR(x)           ((void)(x))
#define portYIELD()                     taskYIELD()
#define spinlock_initialize(m)          ((void)(m))
#define IRAM_ATTR
#define DRAM_ATTR

void taskYIELD(void);

#include "freertos/task.h"
