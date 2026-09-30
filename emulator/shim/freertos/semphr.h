#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct emu_sem *SemaphoreHandle_t;

SemaphoreHandle_t emu_sem_create(int count, int max, bool mutex);
#define xSemaphoreCreateMutex()             emu_sem_create(1, 1, true)
#define xSemaphoreCreateRecursiveMutex()    emu_sem_create(1, 1, true)
#define xSemaphoreCreateBinary()            emu_sem_create(0, 1, false)
#define xSemaphoreCreateCounting(max, init) emu_sem_create(init, max, false)
void vSemaphoreDelete(SemaphoreHandle_t s);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s);
#define xSemaphoreGiveFromISR(s, w)     xSemaphoreGive(s)
#define xSemaphoreTakeFromISR(s, w)     xSemaphoreTake(s, 0)
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s);
