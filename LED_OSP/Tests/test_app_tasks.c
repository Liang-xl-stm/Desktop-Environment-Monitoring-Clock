/** 独立运行真实应用任务入口，网络任务从不执行，检查 LED 和回显仍能工作。 */
#include "app.h"
#include "task.h"
#include "epd_config.h"
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>

static TaskFunction_t entries[4];
static unsigned created, toggles, delays, network_runs, echoed, reads;
static uint8_t logger_ready;
static jmp_buf stop;

BaseType_t xTaskCreate(TaskFunction_t run, const char *name, uint16_t stack,
                      void *argument, UBaseType_t priority, TaskHandle_t *handle)
{
    static const char *names[] = {"LED", "Console", "App", "EPD"};
    assert(created < 4 && strcmp(name, names[created]) == 0);
    assert(argument == NULL && handle == NULL && stack >= 128);
    assert(priority == (created == 2 ? 2U : 1U));
    entries[created++] = run;
    return pdPASS;
}
TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelayUntil(TickType_t *previous, TickType_t period)
{
    assert(period == 500 && *previous == delays * 500);
    if (++delays == 3) longjmp(stop, 1);
    *previous += period;
}
void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 1);
    longjmp(stop, 1);
}
void LED_Toggle(void) { ++toggles; }
void ESP8266_Task(void *argument) { (void)argument; ++network_runs; }
void EPD_Task(void *argument) { (void)argument; }
uint8_t EPD_App_Init(void) { return (uint8_t)(xTaskCreate(EPD_Task, "EPD", 384U, NULL, 1U, NULL) == pdPASS); }
uint8_t Log_IsReady(void) { return logger_ready; }
uint16_t USART1_Receive(uint8_t *data, uint16_t capacity)
{
    static const uint8_t raw[] = {0x00, 0xFF, 'A', '\n'};
    assert(capacity >= sizeof(raw)); ++reads;
    memcpy(data, raw, sizeof(raw)); return sizeof(raw);
}
uint8_t Log_WriteRaw(const uint8_t *data, uint32_t length)
{
    assert(length == 4 && data[0] == 0 && data[1] == 0xFF && data[2] == 'A');
    ++echoed; return 1;
}
int main(void)
{
    assert(App_Init() && created == (EPD_TASK_ENABLE ? 4U : 3U));
    if (setjmp(stop) == 0) entries[0](NULL);
    assert(toggles == 2 && network_runs == 0);
    /* 日志未准备时不消费控制台；准备后原样回显二进制。 */
    if (setjmp(stop) == 0) entries[1](NULL);
    assert(reads == 0 && echoed == 0);
    logger_ready = 1;
    if (setjmp(stop) == 0) entries[1](NULL);
    assert(reads == 1 && echoed == 1 && network_runs == 0);
    puts("PASS independent LED and console tasks without network startup");
    return 0;
}
