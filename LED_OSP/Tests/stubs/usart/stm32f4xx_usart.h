#ifndef TEST_USART_SPL_USART_H
#define TEST_USART_SPL_USART_H
#include "stm32f4xx.h"
typedef struct
{
    uint32_t USART_BaudRate, USART_WordLength, USART_StopBits, USART_Parity;
    uint32_t USART_HardwareFlowControl, USART_Mode;
} USART_InitTypeDef;
#define USART_WordLength_8b 0U
#define USART_StopBits_1 0U
#define USART_Parity_No 0U
#define USART_HardwareFlowControl_None 0U
#define USART_Mode_Tx 1U
#define USART_Mode_Rx 2U
#define USART_IT_RXNE 1U
#define USART_IT_IDLE 2U
#define USART_IT_ERR 4U
static inline void USART_DeInit(USART_TypeDef *uart)
{ uart->SR = USART_SR_TXE; uart->DR = 0U; }
static inline void USART_StructInit(USART_InitTypeDef *config)
{ config->USART_BaudRate = 9600U; }
static inline void USART_Init(USART_TypeDef *uart, USART_InitTypeDef *config)
{ (void)uart; (void)config; }
static inline void USART_ITConfig(USART_TypeDef *uart, uint32_t interrupt, FunctionalState enabled)
{ (void)uart; (void)interrupt; (void)enabled; }
static inline void USART_Cmd(USART_TypeDef *uart, FunctionalState enabled)
{ (void)uart; (void)enabled; }
#endif
