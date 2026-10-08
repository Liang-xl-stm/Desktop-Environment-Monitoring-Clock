/**
 * @file    stm32f4xx_conf.h
 * @brief   项目使用的 STM32F4 标准外设库配置入口。
 * @details 汇总外设驱动头文件，并根据 USE_FULL_ASSERT 配置参数检查宏。
 *          包含头文件只提供声明和宏定义，外设初始化仍由对应代码执行。
 */

#ifndef STM32F4XX_CONF_H
#define STM32F4XX_CONF_H

/* 标准外设库模块接口：供项目按需调用，具体驱动实现在 Libraries 中。 */
#include "stm32f4xx_adc.h"
#include "stm32f4xx_can.h"
#include "stm32f4xx_crc.h"
#include "stm32f4xx_cryp.h"
#include "stm32f4xx_dac.h"
#include "stm32f4xx_dbgmcu.h"
#include "stm32f4xx_dcmi.h"
#include "stm32f4xx_dma.h"
#include "stm32f4xx_exti.h"
#include "stm32f4xx_flash.h"
#include "stm32f4xx_fsmc.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_hash.h"
#include "stm32f4xx_i2c.h"
#include "stm32f4xx_iwdg.h"
#include "stm32f4xx_pwr.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_rng.h"
#include "stm32f4xx_rtc.h"
#include "stm32f4xx_sdio.h"
#include "stm32f4xx_spi.h"
#include "stm32f4xx_syscfg.h"
#include "stm32f4xx_tim.h"
#include "stm32f4xx_usart.h"
#include "stm32f4xx_wwdg.h"
/* 内核相关的 NVIC 与 SysTick 标准库接口。 */
#include "misc.h"

/* 定义 USE_FULL_ASSERT 时，标准库会对传入参数执行断言检查。 */
#ifdef USE_FULL_ASSERT
/**
 * @brief     参数断言失败时调用的处理函数，由项目提供实现。
 * @param[in] file 触发断言的源文件名。
 * @param[in] line 触发断言的源文件行号。
 * @return    无。
 * @note      此处仅声明；启用完整断言后需要提供该函数的定义。
 */
void assert_failed(uint8_t *file, uint32_t line);
/**
 * @brief 验证表达式，结果为假时报告断言所在的文件和行号。
 * @param expr 待检查的参数条件表达式。
 */
#define assert_param(expr) ((expr) ? (void)0 : assert_failed((uint8_t *)__FILE__, __LINE__))
#else
/**
 * @brief 未启用完整断言时忽略参数检查，表达式不会被求值。
 * @param expr 保留调用形式的参数条件表达式。
 */
#define assert_param(expr) ((void)0)
#endif /* USE_FULL_ASSERT */

#endif /* STM32F4XX_CONF_H */
