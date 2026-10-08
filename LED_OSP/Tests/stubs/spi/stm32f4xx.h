#include <stdint.h>
#define __IO volatile
typedef struct { uint32_t CR1, CR2, SR, DR, I2SCFGR; } SPI_TypeDef;
typedef struct { uint32_t BSRRL, BSRRH, OTYPER, OSPEEDR, PUPDR, AFR[2], MODER; } GPIO_TypeDef;
typedef struct { uint32_t APB2ENR, APB2RSTR, AHB1ENR; } RCC_TypeDef;
typedef struct { uint32_t CTRL, CYCCNT; } DWT_Type;
typedef struct { uint32_t DEMCR; } CoreDebug_Type;
extern GPIO_TypeDef test_gpio;
extern RCC_TypeDef test_rcc;
extern CoreDebug_Type test_debug;
extern uint32_t SystemCoreClock;
SPI_TypeDef *test_spi_registers(void);
DWT_Type *test_counter(void);
#define SPI1 test_spi_registers()
#define GPIOB (&test_gpio)
#define RCC (&test_rcc)
#define CoreDebug (&test_debug)
#define DWT test_counter()
#define CoreDebug_DEMCR_TRCENA_Msk (1U << 24)
#define DWT_CTRL_NOCYCCNT_Msk (1U << 25)
#define DWT_CTRL_CYCCNTENA_Msk 1U
#define SPI_SR_TXE (1U << 1)
#define SPI_SR_RXNE 1U
#define SPI_SR_BSY (1U << 7)
#define SPI_SR_MODF (1U << 5)
#define SPI_SR_OVR (1U << 6)
#define SPI_SR_CRCERR (1U << 4)
#define SPI_CR1_MSTR (1U << 2)
#define SPI_CR1_SSM (1U << 9)
#define SPI_CR1_SSI (1U << 8)
#define SPI_CR1_BR_1 (1U << 4)
#define SPI_CR1_SPE (1U << 6)
#define GPIO_ODR_ODR_14 (1U << 14)
#define RCC_APB2ENR_SPI1EN (1U << 12)
#define RCC_APB2RSTR_SPI1RST (1U << 12)
#define RCC_AHB1ENR_GPIOBEN (1U << 1)
static inline unsigned __get_IPSR(void) { return 0; }
static inline unsigned __get_PRIMASK(void) { return 0; }
static inline unsigned __get_BASEPRI(void) { return 0; }
static inline void __DSB(void) { }
static inline void __ISB(void) { }
static inline void __NOP(void) { }
