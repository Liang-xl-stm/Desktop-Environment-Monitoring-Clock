/** @file led.h @brief PF9 板载 LED，低电平点亮。 */
#ifndef HARDWARE_LED_H
#define HARDWARE_LED_H
#include <stdint.h>
/** 初始化并关闭 LED，无参数、无返回值。 */
void LED_Init(void);
/** @param on 0 关闭，非零点亮；须先调用 LED_Init。 */
void LED_Set(uint8_t on);
/** 切换 PF9 LED 的亮灭状态。 */
void LED_Toggle(void);
#endif
