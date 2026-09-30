#pragma once

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
typedef struct emu_task *TaskHandle_t;

typedef enum { eNoAction, eSetBits, eIncrement, eSetValueWithOverwrite, eSetValueWithoutOverwrite } eNotifyAction;

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
#define xTaskCreateWithCaps(fn, name, stack, arg, prio, out, caps) xTaskCreate(fn, name, stack, arg, prio, out)
void vTaskDelete(TaskHandle_t t);
#define vTaskDeleteWithCaps(t) vTaskDelete(t)
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskDelayUntil(TickType_t *prev, TickType_t inc);
#define vTaskDelayUntil(prev, inc) ((void)xTaskDelayUntil(prev, inc))
TickType_t xTaskGetTickCount(void);
#define xTaskGetTickCountFromISR() xTaskGetTickCount()
TaskHandle_t xTaskGetCurrentTaskHandle(void);
char *pcTaskGetName(TaskHandle_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);

// Direct-to-task notifications
BaseType_t xTaskGenericNotify(TaskHandle_t t, uint32_t value, eNotifyAction action);
#define xTaskNotify(t, v, a)        xTaskGenericNotify(t, v, a)
#define xTaskNotifyGive(t)          xTaskGenericNotify(t, 0, eIncrement)
#define xTaskNotifyFromISR(t, v, a, w)  xTaskGenericNotify(t, v, a)
#define vTaskNotifyGiveFromISR(t, w)    ((void)xTaskGenericNotify(t, 0, eIncrement))
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t *value, TickType_t ticks);
