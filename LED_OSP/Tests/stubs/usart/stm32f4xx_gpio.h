#ifndef TEST_USART_SPL_GPIO_H
#define TEST_USART_SPL_GPIO_H
#include "stm32f4xx.h"
typedef struct
{
    uint32_t GPIO_Pin, GPIO_Mode, GPIO_OType, GPIO_Speed, GPIO_PuPd;
} GPIO_InitTypeDef;
#define GPIO_Pin_9 (1U << 9)
#define GPIO_Pin_10 (1U << 10)
#define GPIO_Pin_11 (1U << 11)
#define GPIO_PinSource9 9U
#define GPIO_PinSource10 10U
#define GPIO_PinSource11 11U
#define GPIO_AF_USART1 7U
#define GPIO_AF_USART3 7U
#define GPIO_Mode_AF 2U
#define GPIO_OType_PP 0U
#define GPIO_Speed_50MHz 2U
#define GPIO_PuPd_UP 1U
static inline void GPIO_StructInit(GPIO_InitTypeDef *config) { config->GPIO_Pin = 0U; }
static inline void GPIO_Init(GPIO_TypeDef *gpio, GPIO_InitTypeDef *config)
{ (void)gpio; (void)config; }
static inline void GPIO_PinAFConfig(GPIO_TypeDef *gpio, uint16_t pin, uint8_t af)
{ (void)gpio; (void)pin; (void)af; }
#endif
