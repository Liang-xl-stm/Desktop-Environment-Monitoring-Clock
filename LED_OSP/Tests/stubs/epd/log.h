#ifndef TEST_EPD_LOG_H
#define TEST_EPD_LOG_H
#include <stdint.h>
uint8_t Log_IsReady(void);
void Test_EpdLog(int error, const char *tag, const char *format, ...);
#define elog_i(tag, ...) Test_EpdLog(0, tag, __VA_ARGS__)
#define elog_e(tag, ...) Test_EpdLog(1, tag, __VA_ARGS__)
#endif
