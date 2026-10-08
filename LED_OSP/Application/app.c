/** @file app.c @brief 独立应用任务；LED 和墨水屏不等待 Wi-Fi/TCP 连接。 */
#include "app.h"
#include "led.h"
#include "usart.h"
#include "log.h"
#include "esp_app.h"
#include "epd_app.h"
#include "epd_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>

/** LED 每 500ms 切换一次；独立任务，不等待网络初始化。 */
static void LED_Task(void *argument)
{
    TickType_t last_wake = xTaskGetTickCount();
    (void)argument;
    for (;;)
    {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(500));
        LED_Toggle();
    }
}

/** USART1 原样回显；与日志共享输出锁，USART3 只由 ESP_RX 读取。 */
static void Console_Task(void *argument)
{
    uint8_t buffer[32];
    uint16_t received;
    (void)argument;
    for (;;)
    {
        if (Log_IsReady())
        {
            received = USART1_Receive(buffer, sizeof(buffer));
            if (received != 0U) (void)Log_WriteRaw(buffer, received);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

uint8_t App_Init(void)
{
    /* 栈单位为 32 位字：LED 512B、Console 1024B、网络 3072B。 */
    if (xTaskCreate(LED_Task, "LED", 128U, NULL, 1U, NULL) != pdPASS) return 0U;
    if (xTaskCreate(Console_Task, "Console", 256U, NULL, 1U, NULL) != pdPASS) return 0U;
    if (xTaskCreate(ESP8266_Task, "App", 768U, NULL, 2U, NULL) != pdPASS) return 0U;
#if EPD_TASK_ENABLE
    if (!EPD_App_Init()) return 0U;
#endif
    return 1U;
}
