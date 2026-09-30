#pragma once

#include "freertos/FreeRTOS.h"

typedef struct emu_queue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
void vQueueDelete(QueueHandle_t q);
BaseType_t xQueueGenericSend(QueueHandle_t q, const void *item, TickType_t ticks, int front);
#define xQueueSend(q, item, t)          xQueueGenericSend(q, item, t, 0)
#define xQueueSendToBack(q, item, t)    xQueueGenericSend(q, item, t, 0)
#define xQueueSendToFront(q, item, t)   xQueueGenericSend(q, item, t, 1)
#define xQueueSendFromISR(q, item, w)   xQueueGenericSend(q, item, 0, 0)
#define xQueueSendToBackFromISR(q, item, w) xQueueGenericSend(q, item, 0, 0)
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
#define xQueueReceiveFromISR(q, item, w) xQueueReceive(q, item, 0)
BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t ticks);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q);
BaseType_t xQueueReset(QueueHandle_t q);
