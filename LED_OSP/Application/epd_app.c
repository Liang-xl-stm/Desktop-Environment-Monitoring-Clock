/** @file epd_app.c @brief CRC 校验后显示图片，屏幕等待不阻塞 TCP 下载。 */
#define LOG_TAG "epd"
#include "epd_app.h"
#include "EPD_4in26.h"
#include "picture_store.h"
#include "log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stddef.h>

static QueueHandle_t display_queue;
/* 整屏位于全局 RAM，不放到任务栈，也不从 32KiB FreeRTOS 堆申请。 */
static uint8_t picture_frame[PICTURE_DATA_SIZE];

/** @brief 使用原驱动完成一次全刷或启动测试，仅 EPD_Task 调用。 */
static void EPD_Render(uint16_t slot, uint8_t stored)
{
    static const uint8_t black_rectangle[64U / 8U * 32U] = {0};
    const char *stage;
    if (DEV_Module_Init() != 0)
    {
        elog_e(LOG_TAG, "GPIO/time source init failed");
        return;
    }
    stage = "controller init";
    if (EPD_4in26_Init() != 0) goto failed;
    stage = "clear";
    if (EPD_4in26_Clear() != 0) goto failed;
    stage = "image refresh";
    if (stored)
    {
        /* 参数：已读回并校验的 48000 字节整屏图像，1 白、0 黑。 */
        if (EPD_4in26_Display_Base(picture_frame) != 0) goto failed;
    }
    else
    {
        /* 参数：图像地址、x、y、宽度、高度，单位像素。 */
        if (EPD_4in26_Display_Part(black_rectangle, 32U, 32U, 64U, 32U) != 0) goto failed;
    }
    (void)EPD_4in26_Sleep();
    if (stored) elog_i(LOG_TAG, "picture slot=%u shown, sleeping", (unsigned int)slot);
    else elog_i(LOG_TAG, "test complete: 64x32 black rectangle at (32,32), sleeping");
    return;
failed:
    DEV_Module_Exit();
    elog_e(LOG_TAG, "%s failed: check BUSY/wiring/power", stage);
    /* 任务继续运行，下次显示请求会重新上电初始化。 */
}

void EPD_Task(void *argument)
{
    uint16_t slot = EPD_STARTUP_PICTURE;
    (void)argument;
    while (!Log_IsReady()) vTaskDelay(pdMS_TO_TICKS(10U));
    if (Picture_Read(slot, picture_frame, sizeof(picture_frame))) EPD_Render(slot, 1U);
#if EPD_DEMO_ENABLE
    else EPD_Render(slot, 0U);
#endif
    for (;;)
    {
        if (xQueueReceive(display_queue, &slot, portMAX_DELAY) == pdPASS)
        {
            if (Picture_Read(slot, picture_frame, sizeof(picture_frame))) EPD_Render(slot, 1U);
            else elog_e(LOG_TAG, "picture slot=%u unavailable/read/CRC failed", (unsigned int)slot);
        }
    }
}

uint8_t EPD_App_Init(void)
{
    if (display_queue != NULL) return 0U;
    display_queue = xQueueCreate(1U, sizeof(uint16_t));
    if (display_queue == NULL) return 0U;
    /* 栈 384 个 32 位字 = 1536B，优先级 1；整屏缓冲不计入这个任务栈。 */
    if (xTaskCreate(EPD_Task, "EPD", 384U, NULL, 1U, NULL) != pdPASS)
    {
        vQueueDelete(display_queue); display_queue = NULL; return 0U;
    }
    return 1U;
}

uint8_t EPD_ShowPicture(uint16_t slot)
{
    if (display_queue == NULL || slot >= PICTURE_SLOT_COUNT || __get_IPSR() != 0U) return 0U;
    return (uint8_t)(xQueueOverwrite(display_queue, &slot) == pdPASS);
}
