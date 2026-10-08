#ifndef TEST_EPD_GPIO_H
#define TEST_EPD_GPIO_H
#include "stm32f4xx.h"
typedef enum { Bit_RESET, Bit_SET } BitAction;
typedef struct { uint32_t GPIO_Pin, GPIO_Mode, GPIO_OType, GPIO_Speed, GPIO_PuPd; } GPIO_InitTypeDef;
#define GPIO_Mode_OUT 1
#define GPIO_Mode_IN 0
#define GPIO_OType_PP 0
#define GPIO_Speed_50MHz 2
#define GPIO_PuPd_NOPULL 0
#define GPIO_PuPd_UP 1
void GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *gpio);
void GPIO_WriteBit(GPIO_TypeDef *port, uint16_t pin, BitAction value);
void GPIO_SetBits(GPIO_TypeDef *port, uint16_t pin);
void GPIO_ResetBits(GPIO_TypeDef *port, uint16_t pin);
uint8_t GPIO_ReadInputDataBit(GPIO_TypeDef *port, uint16_t pin);
#endif
