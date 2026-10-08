/**
 * @file DEV_Config.c
 * @brief STM32F407 GPIO、软件 SPI 及电子纸供电控制实现。
 */
#include "DEV_Config.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_rcc.h"
#include "FreeRTOS.h"
#include "task.h"

/** @brief 使用共享 DWT 周期计数实现微秒时序，不复位其他驱动使用的 CYCCNT。 */
static void DEV_Delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = (uint32_t)(((uint64_t)SystemCoreClock * us + 999999U) / 1000000U);
    while ((uint32_t)(DWT->CYCCNT - start) < cycles)
    {
        __NOP();
    }
}

void DEV_Delay_ms(uint32_t ms)
{
    uint32_t chunk;
    TickType_t ticks;

    while (ms != 0U)
    {
        /* 分段避免大延时的周期数或节拍换算溢出。 */
        chunk = ms > 1000U ? 1000U : ms;
        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
            __get_IPSR() == 0U && __get_PRIMASK() == 0U && __get_BASEPRI() == 0U)
        {
            /* 加一个节拍，避免调用恰好在节拍边沿时实际延时短于要求。 */
            ticks = (TickType_t)(((uint64_t)chunk * configTICK_RATE_HZ + 999U) / 1000U + 1U);
            vTaskDelay(ticks);
        }
        else
        {
            while (chunk-- != 0U) DEV_Delay_us(1000U);
        }
        ms -= ms > 1000U ? 1000U : ms;
    }
}

/** @brief 设置指定 GPIO 输出电平。
 *  @param port GPIO 端口。
 *  @param pin GPIO 引脚掩码。
 *  @param value 非零置位，零复位。
 */
void DEV_Digital_WritePin(GPIO_TypeDef *port, uint16_t pin, UBYTE value)
{
    GPIO_WriteBit(port, pin, value ? Bit_SET : Bit_RESET);
}

/** @brief 读取指定 GPIO 输入电平。
 *  @param port GPIO 端口。
 *  @param pin GPIO 引脚掩码。
 *  @return 高电平为 1，低电平为 0。
 */
UBYTE DEV_Digital_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    return (GPIO_ReadInputDataBit(port, pin) == Bit_SET) ? 1U : 0U;
}

/** @brief 通过 GPIO 模拟 SPI 发送一个字节。
 *  @details 数据按 MSB 优先发送；SCK 空闲为低，控制器在时钟上升沿采样数据。
 *           本函数不控制 CS 和 DC，调用方需先设置好命令/数据状态及片选。
 *  @param value 待发送的 8 位数据。
 */
void DEV_SPI_WriteByte(UBYTE value)
{
    UBYTE bit;

    for (bit = 0x80U; bit != 0U; bit >>= 1) {
        GPIO_ResetBits(EPD_SCK_PORT, EPD_SCK_GPIO_PIN);
        GPIO_WriteBit(EPD_MOSI_PORT, EPD_MOSI_GPIO_PIN,
                      (value & bit) ? Bit_SET : Bit_RESET);
        DEV_Delay_us(1U);
        GPIO_SetBits(EPD_SCK_PORT, EPD_SCK_GPIO_PIN);
        DEV_Delay_us(1U);
    }

    GPIO_ResetBits(EPD_SCK_PORT, EPD_SCK_GPIO_PIN);
}

/** @brief 通过 GPIO 模拟 SPI 连续发送缓冲区。
 *  @param value 待发送缓冲区；len 大于零时不得为空指针。
 *  @param len 待发送字节数。
 */
void DEV_SPI_Write_nByte(UBYTE *value, UDOUBLE len)
{
    while (len != 0U) {
        DEV_SPI_WriteByte(*value);
        value++;
        len--;
    }
}

/** @brief 初始化电子纸 GPIO，并使能面板电源。
 *  @details GPIOF 统一使用推挽输出；BUSY 配置为上拉输入。PWR、RST 拉高后面板上电。
 *  @return 成功返回 0；中断调用或周期计数不可用返回 -1。
 */
int DEV_Module_Init(void)
{
    GPIO_InitTypeDef gpio;
    uint32_t start;
    uint32_t i;

    if (__get_IPSR() != 0U || SystemCoreClock == 0U) return -1;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U) return -1;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
    start = DWT->CYCCNT;
    for (i = 0U; i < 64U; ++i) __NOP();
    if ((uint32_t)(DWT->CYCCNT - start) == 0U) return -1;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOF, ENABLE);

    gpio.GPIO_Mode = GPIO_Mode_OUT;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;

    gpio.GPIO_Pin = EPD_RST_GPIO_PIN | EPD_DC_GPIO_PIN | EPD_CS_GPIO_PIN |
                    EPD_SCK_GPIO_PIN | EPD_PWR_GPIO_PIN | EPD_MOSI_GPIO_PIN;
    GPIO_Init(GPIOF, &gpio);

    gpio.GPIO_Mode = GPIO_Mode_IN;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;
    gpio.GPIO_Pin = EPD_BUSY_GPIO_PIN;
    GPIO_Init(GPIOF, &gpio);

    GPIO_ResetBits(EPD_DC_PORT, EPD_DC_GPIO_PIN);
    GPIO_SetBits(EPD_CS_PORT, EPD_CS_GPIO_PIN);
    GPIO_ResetBits(EPD_SCK_PORT, EPD_SCK_GPIO_PIN);
    GPIO_ResetBits(EPD_MOSI_PORT, EPD_MOSI_GPIO_PIN);
    GPIO_SetBits(EPD_PWR_PORT, EPD_PWR_GPIO_PIN);
    GPIO_SetBits(EPD_RST_PORT, EPD_RST_GPIO_PIN);

    return 0;
}

/** @brief 关闭面板电源并将接口输出置为低电平。
 *  @details 调用前应确保没有电子纸刷新操作正在进行。
 */
void DEV_Module_Exit(void)
{
    GPIO_ResetBits(EPD_DC_PORT, EPD_DC_GPIO_PIN);
    GPIO_SetBits(EPD_CS_PORT, EPD_CS_GPIO_PIN);
    GPIO_ResetBits(EPD_SCK_PORT, EPD_SCK_GPIO_PIN);
    GPIO_ResetBits(EPD_MOSI_PORT, EPD_MOSI_GPIO_PIN);
    GPIO_ResetBits(EPD_PWR_PORT, EPD_PWR_GPIO_PIN);
    GPIO_ResetBits(EPD_RST_PORT, EPD_RST_GPIO_PIN);
}
