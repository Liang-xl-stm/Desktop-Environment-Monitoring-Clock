/** @brief 主机测试递归互斥锁接口；实现检查所有权及 Take/Give 配对。 */
#ifndef TEST_EASYLOGGER_SEMPHR_H
#define TEST_EASYLOGGER_SEMPHR_H
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t wait);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex);
void vSemaphoreDelete(SemaphoreHandle_t mutex);
#endif
