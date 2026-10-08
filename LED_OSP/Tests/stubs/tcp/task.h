#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
TickType_t xTaskGetTickCount(void);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint16_t stack, void *arg, UBaseType_t priority, TaskHandle_t *handle);
BaseType_t xTaskNotifyGive(TaskHandle_t handle);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
