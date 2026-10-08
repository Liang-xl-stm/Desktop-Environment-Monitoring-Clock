/** @brief 主机测试模拟任务信息、节拍和调度器状态。 */
#ifndef TEST_EASYLOGGER_TASK_H
#define TEST_EASYLOGGER_TASK_H
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_SUSPENDED 1
#define taskSCHEDULER_RUNNING 2
BaseType_t xTaskGetSchedulerState(void);
TickType_t xTaskGetTickCount(void);
const char *pcTaskGetName(TaskHandle_t task);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
#endif
