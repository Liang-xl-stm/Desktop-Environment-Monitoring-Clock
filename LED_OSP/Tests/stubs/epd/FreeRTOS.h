#ifndef TEST_EPD_RTOS_H
#define TEST_EPD_RTOS_H
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define configTICK_RATE_HZ 1000U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#endif
