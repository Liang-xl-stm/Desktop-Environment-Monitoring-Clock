/**
 * @file lwesp_ll_stm32f407.c
 * @brief LwESP 的 USART3 通信适配与 ESP8266 硬件复位控制。
 * @note USART3 使用 PB10/PB11；模块低电平有效的 RST/KEY 连接 PF6。
 */

#include "system/lwesp_ll.h"
#include "bsp_uart.h"
#include "lwesp/lwesp_mem.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_gpio.h"

/* 库的动态分配内存池，共 16 KB，保证至少 4 字节对齐。 */
static uint32_t esp_memory[4096];
static uint8_t esp_memory_ready;
static uint32_t esp_uart_baudrate;
/** @brief PF6 仅配置一次，避免重复初始化提前释放正在进行的复位。 */
static uint8_t esp_reset_gpio_ready;

static uart_device_t *esp_uart;

/**
 * @brief 首次调用时配置 PF6，默认释放模块复位。
 * @note 库在复位过程中可能再次调用 LL 初始化，此时必须保留 PF6 电平。
 */
static void esp_uart_reset_gpio_init(void)
{
    GPIO_InitTypeDef gpio;

    if (esp_reset_gpio_ready)
    {
        return;
    }

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOF, ENABLE);
    /* 先预置高电平，再切换为输出，避免产生意外的低电平复位脉冲。 */
    GPIO_SetBits(GPIOF, GPIO_Pin_6);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = GPIO_Pin_6;
    gpio.GPIO_Mode  = GPIO_Mode_OUT;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(GPIOF, &gpio);
    esp_reset_gpio_ready = 1;
}

/**
 * @brief 按 LwESP 的调用约定控制 ESP8266 硬件复位。
 * @param[in] state 1：拉低 PF6 进入复位；0：拉高 PF6 释放复位。
 * @return 1 表示已完成本次电平控制。
 * @pre GPIO 已初始化，FreeRTOS 调度器已运行，由任务上下文调用。
 * @note 拉低后保持 100 毫秒；释放后立即返回，由库等待 ready 响应。
 */
static uint8_t esp_uart_reset(uint8_t state)
{
    if (state)
    {
        GPIO_ResetBits(GPIOF, GPIO_Pin_6);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    else
    {
        GPIO_SetBits(GPIOF, GPIO_Pin_6);
    }
    return 1;
}

static size_t esp_uart_send(const void *data, size_t len)
{
	if(data == NULL || len == 0)
	{
		return 0;
	}
	UART_Send(esp_uart, (const uint8_t *)data, (uint32_t) len);
	return len;
}

lwespr_t lwesp_ll_init(lwesp_ll_t *ll)
{
    /* 内存池只能注册一次。 */
    if (!esp_memory_ready)
    {
        const lwesp_mem_region_t regions[] =
        {
            {esp_memory, sizeof(esp_memory)}
        };
        uint8_t success;

        success = lwesp_mem_assignmemory(regions, 1);
        configASSERT(success != 0);

        if (!success)
        {
            return lwespERRMEM;
        }
        esp_memory_ready = 1;
    }

    esp_uart_reset_gpio_init();

    /* 相同波特率下避免重复初始化、清空接收缓存。 */
    if (esp_uart == NULL || esp_uart_baudrate != ll->uart.baudrate)
    {
        esp_uart = BSP_UART3_Init(ll->uart.baudrate);
        esp_uart_baudrate = ll->uart.baudrate;
    }

    ll->send_fn = esp_uart_send;
    ll->reset_fn = esp_uart_reset;
    return lwespOK;
}
