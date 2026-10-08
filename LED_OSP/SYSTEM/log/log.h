/** @file log.h @brief EasyLogger 固定输出至 USART1。 */
#ifndef SYSTEM_LOG_H
#define SYSTEM_LOG_H
#include <stdint.h>
#include "elog.h"
/** 在调度器运行后的任务中调用；无参数，成功返回 1，失败返回 0。
 * 之后直接使用 elog_i("标签", "格式", 参数)、elog_e 等宏输出。
 */
uint8_t Log_Init(void);
/** @return 1 日志可用，0 尚未初始化。其他任务须等待可用后输出日志/回显。 */
uint8_t Log_IsReady(void);
/** @param data 原始数据地址；@param length 实际字节数，允许含零字节。
 * @return 1 输出完成或零长度，0 未初始化或参数错误。
 * @note 仅在运行中任务调用，整批持有日志锁，不添加格式或换行。
 */
uint8_t Log_WriteRaw(const uint8_t *data, uint32_t length);
#endif
