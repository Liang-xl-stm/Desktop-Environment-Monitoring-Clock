/** Run real software SPI and screen driver against simulated GPIO/RTOS. */
#include "EPD_4in26.h"
#include "stm32f4xx_gpio.h"
#include "task.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

GPIO_TypeDef test_gpiof;
DWT_Type test_dwt;
CoreDebug_Type test_debug;
uint32_t SystemCoreClock = 168000000U;
unsigned test_ipsr, test_primask, test_basepri;
static unsigned configured, writes, scheduler, blocked, busy_reads;
static int busy;
static uint8_t shift, bits, command, last_byte;
static unsigned lengths[256], sums[256];

BaseType_t xTaskGetSchedulerState(void) { return (BaseType_t)scheduler; }
void vTaskDelay(TickType_t ticks) { assert(ticks > 0); blocked += ticks; }
void RCC_AHB1PeriphClockCmd(unsigned mask, int enable)
{ assert(mask == 32U && enable == ENABLE); }
void GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *gpio)
{ assert(port == GPIOF); configured |= gpio->GPIO_Pin; }
void GPIO_WriteBit(GPIO_TypeDef *port, uint16_t pin, BitAction value)
{
    unsigned old;
    assert(port == GPIOF); ++writes;
    old = port->output;
    if (value) port->output |= pin; else port->output &= ~pin;
    /* Observe bytes on the wire: rising CLK while CS is selected. */
    if (pin == GPIO_Pin_1 && value && !(old & pin) && !(port->output & GPIO_Pin_2))
    {
        shift = (uint8_t)((shift << 1) | ((port->output & GPIO_Pin_0) != 0));
        if (++bits == 8)
        {
            last_byte = shift; bits = 0;
            if (!(port->output & GPIO_Pin_3)) command = shift;
            else { ++lengths[command]; sums[command] += shift; }
        }
    }
}
void GPIO_SetBits(GPIO_TypeDef *port, uint16_t pin) { GPIO_WriteBit(port, pin, Bit_SET); }
void GPIO_ResetBits(GPIO_TypeDef *port, uint16_t pin) { GPIO_WriteBit(port, pin, Bit_RESET); }
uint8_t GPIO_ReadInputDataBit(GPIO_TypeDef *port, uint16_t pin)
{ assert(port == GPIOF && pin == GPIO_Pin_5); ++busy_reads; return (uint8_t)busy; }

int main(void)
{
    static const uint8_t black[32] = {0};
    unsigned before;
    uint32_t start;
    /* PF6 is already the ESP8266 reset pin; screen init/exit must never touch it. */
    test_gpiof.output = GPIO_Pin_6;
    test_dwt.CYCCNT = 0xFFFFFF00U;
    assert(DEV_Module_Init() == 0);
    assert((configured & GPIO_Pin_6) == 0);
    assert((configured & GPIO_Pin_7) != 0);
    assert(test_gpiof.output & GPIO_Pin_6);
    assert(test_dwt.CTRL & DWT_CTRL_CYCCNTENA_Msk);

    /* Start before a wrap: delay must use elapsed unsigned cycles. */
    test_dwt.CYCCNT = 0xFFFFFF00U; start = test_dwt.CYCCNT;
    DEV_Delay_ms(2);
    assert((uint32_t)(test_dwt.CYCCNT - start) >= 336000U && blocked == 0);
    scheduler = taskSCHEDULER_RUNNING;
    DEV_Delay_ms(2);
    assert(blocked >= 2 && blocked <= 3);
    before = blocked; test_primask = 1;
    DEV_Delay_ms(1);
    assert(blocked == before); test_primask = 0;

    /* Decode an asymmetric byte to detect bit order / clock-edge mistakes. */
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(0xA6);
    DEV_Digital_Write(EPD_CS_PIN, 1);
    assert(last_byte == 0xA6 && bits == 0);
    assert(EPD_4in26_Init() == 0);
    memset(lengths, 0, sizeof(lengths)); memset(sums, 0, sizeof(sums));
    assert(EPD_4in26_Clear() == 0);
    assert(lengths[0x24] == 48000 && lengths[0x26] == 48000);
    assert(sums[0x24] == 48000 * 255U && sums[0x26] == 48000 * 255U);
    memset(lengths, 0, sizeof(lengths));
    assert(EPD_4in26_Display_Part(black, 32, 32, 16, 16) == 0);
    assert(lengths[0x24] == sizeof(black));
    before = writes;
    assert(EPD_4in26_Display_Part(black, 799, 0, 16, 16) == -1);
    assert(writes == before);
    busy = 1; busy_reads = 0;
    assert(EPD_4in26_ReadBusy() == -1 && busy_reads == 1500);
    busy = 0;
    assert(EPD_4in26_Sleep() == 0);
    DEV_Module_Exit();
    assert(test_gpiof.output & GPIO_Pin_6);
    assert(!(test_gpiof.output & GPIO_Pin_7));
    /* A missing time source must fail before changing any GPIO. */
    before = writes;
    test_dwt.CTRL = DWT_CTRL_NOCYCCNT_Msk;
    assert(DEV_Module_Init() == -1 && writes == before);
    puts("PASS EPD: PF6 isolation, RTOS/cycle delay, SPI bytes, frame sizes, BUSY timeout");
    return 0;
}
