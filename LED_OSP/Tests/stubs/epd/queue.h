#ifndef TEST_EPD_QUEUE_H
#define TEST_EPD_QUEUE_H
#include "FreeRTOS.h"
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count, unsigned size);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item);
void vQueueDelete(QueueHandle_t queue);
#endif
