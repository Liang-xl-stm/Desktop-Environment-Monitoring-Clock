/** @file usart.h
 * @brief USART1 控制台、USART3 ESP8266；8N1，中断接收、轮询发送。
 */
#ifndef USART_DRIVER_H
#define USART_DRIVER_H
#include <stdint.h>
/** @param baudrate 波特率，例如 115200；重新初始化会清空本路接收缓存。 */
void USART1_Init(uint32_t baudrate); /* PA9 TX / PA10 RX */
void USART3_Init(uint32_t baudrate); /* PB10 TX / PB11 RX */
/** @param data 数据首地址；@param length 实际字节数，允许含 0x00。
 * @note 任务内调用，返回时最后一个字节可能仍在发送。
 *       USART1 的多任务日志/回显统一使用 Log_WriteRaw 或 elog_i 等宏。
 */
void USART1_Send(const uint8_t *data, uint32_t length);
void USART3_Send(const uint8_t *data, uint32_t length);
/** @param data 输出缓冲；@param capacity 最多读取字节数。
 * @return 本次实际读取长度；无已发布数据或参数无效返回 0。
 * @note 中断在每个有效字节入缓存后立即发布，不需要等待 IDLE。
 *       不等待新字节；USART3 只能由 ESP_RX 任务读取。
 */
uint16_t USART1_Receive(uint8_t *data, uint16_t capacity);
uint16_t USART3_Receive(uint8_t *data, uint16_t capacity);
#endif
