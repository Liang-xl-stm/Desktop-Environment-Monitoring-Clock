/** @file main.c @brief 程序入口：初始化硬件、创建应用任务、启动调度器。 */
#include "stm32f4xx.h"
#include "misc.h"
#include "led.h"
#include "usart.h"
#include "w25q128.h"
#include "app.h"
#include "FreeRTOS.h"
#include "task.h"

int main(void)
{
    uint8_t result;

    SystemCoreClockUpdate();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    LED_Init();
    USART1_Init(115200U);   /* PA9/PA10：电脑日志串口 */
    USART3_Init(115200U);   /* PB10/PB11：ESP8266 */
    result = W25Q128_Init();  /* SPI1 和 PB14 片选 */
    configASSERT(result != 0U);
    result = App_Init();      /* LED、回显、Wi-Fi/TCP 三个任务 */
    configASSERT(result != 0U);

    vTaskStartScheduler();
    configASSERT(0);
    for (;;) { __NOP(); }
}
