/**
 * @file DEV_Config.h
 * @brief 4.26 寸电子纸的 STM32F407 底层硬件接口声明。
 * @details 使用独立软件 SPI；接线及启动测试开关见 USER/epd_config.h。
 *          同一块屏的初始化、刷新、休眠必须由同一个任务顺序调用，不能在中断中刷新。
 */
#ifndef __DEV_CONFIG_H
#define __DEV_CONFIG_H

/* STM32F4 标准外设库、屏幕接线及定长整数类型。 */
#include "stm32f4xx.h"
#include "epd_config.h"
#include <stdint.h>

/* Waveshare 示例兼容用的无符号数据类型别名。 */
#define UBYTE   uint8_t
#define UWORD   uint16_t
#define UDOUBLE uint32_t

/* 将端口和引脚打包为底层读写函数所需的两个实参。 */
#define EPD_RST_PIN  EPD_RST_PORT, EPD_RST_GPIO_PIN
#define EPD_DC_PIN   EPD_DC_PORT, EPD_DC_GPIO_PIN
#define EPD_BUSY_PIN EPD_BUSY_PORT, EPD_BUSY_GPIO_PIN
#define EPD_CS_PIN   EPD_CS_PORT, EPD_CS_GPIO_PIN

/* 将驱动层通用读写接口映射到底层 GPIO 实现。 */
#define DEV_Digital_Write(_pin, _value) DEV_Digital_WritePin(_pin, _value)
#define DEV_Digital_Read(_pin)          DEV_Digital_ReadPin(_pin)

/** @brief 延时指定毫秒数；任务运行时让出 CPU，不修改 FreeRTOS 的 SysTick。
 *  @param ms 毫秒数，0 立即返回；启动前使用 DWT 等待，启动后使用任务延时。
 *  @note 调度暂停或中断屏蔽时回退为忙等；刷新操作本身仍必须在普通任务中执行。
 */
void DEV_Delay_ms(uint32_t ms);

/** @brief 设置指定 GPIO 输出电平。
 *  @param port GPIO 端口，例如 GPIOF。
 *  @param pin GPIO 引脚掩码，例如 GPIO_Pin_4。
 *  @param value 非零输出高电平，零输出低电平。
 */
void DEV_Digital_WritePin(GPIO_TypeDef *port, uint16_t pin, UBYTE value);

/** @brief 读取指定 GPIO 输入电平。
 *  @param port GPIO 端口。
 *  @param pin GPIO 引脚掩码。
 *  @return 高电平返回 1，低电平返回 0。
 */
UBYTE DEV_Digital_ReadPin(GPIO_TypeDef *port, uint16_t pin);

/** @brief 通过 GPIO 模拟 SPI 发送一个字节，按高位优先发送。 */
void DEV_SPI_WriteByte(UBYTE value);

/** @brief 通过 GPIO 模拟 SPI 连续发送多个字节。
 *  @param value 待发送缓冲区；len 非零时必须指向有效内存。
 *  @param len 待发送字节数。
 */
void DEV_SPI_Write_nByte(UBYTE *value, UDOUBLE len);

/** @brief 配置电子纸接口引脚并打开面板电源。
 *  @return 成功返回 0；中断调用或周期计数不可用返回 -1。
 */
int DEV_Module_Init(void);

/** @brief 将接口输出和面板电源使能脚拉低。 */
void DEV_Module_Exit(void);

#endif
