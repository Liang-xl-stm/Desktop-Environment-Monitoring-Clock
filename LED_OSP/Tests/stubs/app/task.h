#ifndef TEST_APP_TASK_H
#define TEST_APP_TASK_H
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(TaskFunction_t run, const char *name, uint16_t stack,
                      void *argument, UBaseType_t priority, TaskHandle_t *handle);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
void vTaskDelayUntil(TickType_t *previous, TickType_t period);
#endif
