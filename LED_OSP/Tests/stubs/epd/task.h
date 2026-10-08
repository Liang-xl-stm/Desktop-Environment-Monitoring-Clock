#ifndef TEST_EPD_TASK_H
#define TEST_EPD_TASK_H
#include "FreeRTOS.h"
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING 2
BaseType_t xTaskGetSchedulerState(void);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *task);
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, uint16_t stack, void *argument,
                      UBaseType_t priority, TaskHandle_t *handle);
#endif
