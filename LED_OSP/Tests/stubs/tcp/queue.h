#include "FreeRTOS.h"
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count, unsigned size);
void vQueueDelete(QueueHandle_t q);
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t wait);
BaseType_t xQueueSend(QueueHandle_t q, const void *data, TickType_t wait);
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q);
