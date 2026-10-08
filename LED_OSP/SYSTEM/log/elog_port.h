#ifndef SYSTEM_ELOG_PORT_H
#define SYSTEM_ELOG_PORT_H
#include <stdint.h>
/** 日志内部移植接口：在运行中任务创建锁，成功返回 1；应用调用 Log_Init 即可。 */
uint8_t Elog_Port_Init(void);
#endif
