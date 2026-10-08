/** @brief 主机测试专用 FreeRTOS 类型与断言，不可加入 Keil 固件工程。 */
#ifndef TEST_EASYLOGGER_FREERTOS_H
#define TEST_EASYLOGGER_FREERTOS_H
#include <stdint.h>
#include <stddef.h>
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdFAIL 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 1U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
void EasyLogger_TestAssert(BaseType_t condition, const char *expression, int line);
#define configASSERT(expression) \
    EasyLogger_TestAssert((BaseType_t)!!(expression), #expression, __LINE__)
BaseType_t xPortIsInsideInterrupt(void);
#endif
