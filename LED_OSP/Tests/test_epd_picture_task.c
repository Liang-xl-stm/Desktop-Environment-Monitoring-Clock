/** Real persistent screen task: stored startup, repeated requests and failure recovery. */
#include "epd_app.h"
#include "EPD_4in26.h"
#include "picture_store.h"
#include "task.h"
#include "queue.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
GPIO_TypeDef test_gpiof;
DWT_Type test_dwt;
CoreDebug_Type test_debug;
uint32_t SystemCoreClock;
unsigned test_ipsr, test_primask, test_basepri;
static unsigned stage, fail_stage, exits, sleeps, errors, ready_calls, frames, parts, queued;
static unsigned startup_image, bad_slot, expected_slot, request;
static TaskFunction_t entry;
static jmp_buf stop;
static int next(unsigned s)
{ assert(++stage == s); if (stage == fail_stage) { fail_stage = 0; return -1; } return 0; }
int DEV_Module_Init(void) { stage = 0; return next(1); }
int EPD_4in26_Init(void) { return next(2); }
int EPD_4in26_Clear(void) { return next(3); }
int EPD_4in26_Display_Part(const UBYTE *p, UWORD x, UWORD y, UWORD w, UWORD h)
{ assert(p[0] == 0 && x == 32 && y == 32 && w == 64 && h == 32); ++parts; return next(4); }
int EPD_4in26_Display_Base(const UBYTE *p)
{ unsigned i; for (i = 0; i < 48000; ++i) assert(p[i] == (uint8_t)expected_slot); ++frames; return next(4); }
int EPD_4in26_Sleep(void) { ++sleeps; return 0; }
void DEV_Module_Exit(void) { ++exits; }
uint8_t Picture_Read(uint32_t slot, uint8_t *data, uint32_t capacity)
{ assert(capacity == 48000); if ((!slot && !startup_image) || slot == bad_slot) return 0; memset(data, (int)slot, capacity); expected_slot = slot; return 1; }
uint8_t Log_IsReady(void) { return (uint8_t)(++ready_calls > 1); }
void vTaskDelay(TickType_t ticks) { assert(ticks == 10 && ready_calls == 1); }
void vTaskDelete(void *task) { (void)task; assert(0 && "screen task must survive refresh errors"); }
void Test_EpdLog(int error, const char *tag, const char *format, ...)
{ assert(strcmp(tag, "epd") == 0 && format); if (error) ++errors; }
QueueHandle_t xQueueCreate(unsigned count, unsigned size)
{ assert(count == 1 && size == sizeof(uint16_t)); return (void *)1; }
void vQueueDelete(QueueHandle_t q) { (void)q; }
BaseType_t xTaskCreate(TaskFunction_t run, const char *name, uint16_t stack, void *arg,
                      UBaseType_t priority, TaskHandle_t *handle)
{ assert(strcmp(name, "EPD") == 0 && stack >= 384 && !arg && priority == 1 && !handle); entry = run; return pdPASS; }
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item)
{ assert(q == (void *)1); request = *(const uint16_t *)item; return pdPASS; }
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    assert(q == (void *)1 && wait == portMAX_DELAY);
    if (queued == 2) longjmp(stop, 1);
    assert(EPD_ShowPicture((uint16_t)(7 + queued)));
    *(uint16_t *)item = (uint16_t)request; ++queued; return pdPASS;
}
int main(void)
{
    unsigned scenario;
    assert(!EPD_ShowPicture(0));
    assert(EPD_App_Init() && entry);
    assert(!EPD_ShowPicture(256));
    test_ipsr = 1; assert(!EPD_ShowPicture(0)); test_ipsr = 0;
    for (scenario = 0; scenario < 6; ++scenario)
    {
        stage = exits = sleeps = errors = ready_calls = frames = parts = queued = 0;
        startup_image = scenario == 1; bad_slot = scenario == 2 ? 8 : 999;
        fail_stage = scenario >= 3 ? scenario - 2 : 0;
        if (setjmp(stop) == 0) entry(NULL);
        assert(queued == 2);
        if (scenario == 0) assert(parts == 1 && frames == 2 && sleeps == 3 && !errors);
        if (scenario == 1) assert(parts == 0 && frames == 3 && sleeps == 3 && !errors);
        if (scenario == 2) assert(parts == 1 && frames == 1 && sleeps == 2 && errors == 1);
        if (scenario >= 3) assert(frames == 2 && sleeps == 2 && errors == 1);
    }
    puts("PASS EPD task: stored startup, repeated requests, corrupt reads and refresh failure recovery");
    return 0;
}
