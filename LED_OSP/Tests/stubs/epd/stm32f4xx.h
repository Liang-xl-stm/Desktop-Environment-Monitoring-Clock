#ifndef TEST_EPD_STM32_H
#define TEST_EPD_STM32_H
#include <stdint.h>
typedef struct { uint32_t output; } GPIO_TypeDef;
typedef struct { uint32_t CTRL, CYCCNT; } DWT_Type;
typedef struct { uint32_t DEMCR; } CoreDebug_Type;
extern GPIO_TypeDef test_gpiof;
extern DWT_Type test_dwt;
extern CoreDebug_Type test_debug;
extern uint32_t SystemCoreClock;
extern unsigned test_ipsr, test_primask, test_basepri;
#define GPIOF (&test_gpiof)
#define DWT (&test_dwt)
#define CoreDebug (&test_debug)
#define CoreDebug_DEMCR_TRCENA_Msk (1UL << 24)
#define DWT_CTRL_CYCCNTENA_Msk 1U
#define DWT_CTRL_NOCYCCNT_Msk (1UL << 25)
#define GPIO_Pin_0 1U
#define GPIO_Pin_1 2U
#define GPIO_Pin_2 4U
#define GPIO_Pin_3 8U
#define GPIO_Pin_4 16U
#define GPIO_Pin_5 32U
#define GPIO_Pin_6 64U
#define GPIO_Pin_7 128U
#define ENABLE 1
static inline unsigned __get_IPSR(void) { return test_ipsr; }
static inline unsigned __get_PRIMASK(void) { return test_primask; }
static inline unsigned __get_BASEPRI(void) { return test_basepri; }
/* Simulate a running cycle counter, including natural 32-bit wraparound. */
static inline void __NOP(void) { if (DWT->CTRL & 1U) DWT->CYCCNT += 168U; }
static inline void __DSB(void) { }
static inline void __ISB(void) { }
#endif
