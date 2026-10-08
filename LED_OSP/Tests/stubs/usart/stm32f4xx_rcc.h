#ifndef TEST_USART_SPL_RCC_H
#define TEST_USART_SPL_RCC_H
#include "stm32f4xx.h"
#define RCC_AHB1Periph_GPIOA 1U
#define RCC_AHB1Periph_GPIOB 2U
#define RCC_APB2Periph_USART1 1U
#define RCC_APB1Periph_USART3 1U
static inline void RCC_AHB1PeriphClockCmd(uint32_t peripheral, FunctionalState enabled)
{ (void)peripheral; (void)enabled; }
static inline void RCC_APB2PeriphClockCmd(uint32_t peripheral, FunctionalState enabled)
{ (void)peripheral; (void)enabled; }
static inline void RCC_APB1PeriphClockCmd(uint32_t peripheral, FunctionalState enabled)
{ (void)peripheral; (void)enabled; }
#endif
