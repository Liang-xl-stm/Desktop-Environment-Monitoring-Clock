#ifndef TEST_USART_STM32F4XX_H
#define TEST_USART_STM32F4XX_H
#include <stdint.h>
typedef struct { uint32_t SR, DR; } USART_TypeDef;
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef int IRQn_Type;
typedef enum { DISABLE = 0, ENABLE = 1 } FunctionalState;
extern USART_TypeDef test_usart1, test_usart3;
extern GPIO_TypeDef test_gpioa, test_gpiob;
#define USART1 (&test_usart1)
#define USART3 (&test_usart3)
#define GPIOA (&test_gpioa)
#define GPIOB (&test_gpiob)
#define USART1_IRQn 37
#define USART3_IRQn 39
#define USART_SR_PE (1U << 0)
#define USART_SR_FE (1U << 1)
#define USART_SR_NE (1U << 2)
#define USART_SR_ORE (1U << 3)
#define USART_SR_IDLE (1U << 4)
#define USART_SR_RXNE (1U << 5)
#define USART_SR_TXE (1U << 7)
static inline void __DMB(void) { }
static inline void NVIC_DisableIRQ(IRQn_Type irq) { (void)irq; }
static inline void NVIC_EnableIRQ(IRQn_Type irq) { (void)irq; }
static inline void NVIC_ClearPendingIRQ(IRQn_Type irq) { (void)irq; }
static inline void NVIC_SetPriority(IRQn_Type irq, unsigned priority)
{ (void)irq; (void)priority; }
#endif
