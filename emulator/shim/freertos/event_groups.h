#pragma once

#include "freertos/FreeRTOS.h"

typedef uint32_t EventBits_t;
typedef struct emu_evgroup *EventGroupHandle_t;

EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t g);
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t bits);
EventBits_t xEventGroupGetBits(EventGroupHandle_t g);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits, BaseType_t clear, BaseType_t all,
                                TickType_t ticks);
#define xEventGroupSetBitsFromISR(g, b, w) xEventGroupSetBits(g, b)
