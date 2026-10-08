#ifndef TEST_APP_FREERTOS_H
#define TEST_APP_FREERTOS_H
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#endif
