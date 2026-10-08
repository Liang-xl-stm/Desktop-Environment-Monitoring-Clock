/**
 * @file    freertos_hooks.c
 * @brief   FreeRTOS 断言、内存分配失败和栈溢出的故障处理。
 * @details 故障信息保存在全局变量中，便于 Keil 调试查看。
 *          故障路径不调用串口、动态分配或调度接口，记录后关闭中断停机。
 */

#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"

/** @brief 故障码：0 正常；1 断言；2 堆分配失败；3 任务栈溢出。 */
volatile uint32_t g_freertos_fault_code = 0U;
/** @brief 断言所在文件，指向编译期字符串，供调试器查看。 */
const char * volatile g_freertos_fault_file = NULL;
/** @brief 断言所在行号。 */
volatile uint32_t g_freertos_fault_line = 0U;
/** @brief 栈溢出任务的名称，故障停机期间其 TCB 不再被释放。 */
const char * volatile g_freertos_fault_task = NULL;

/** @brief 保存故障码，关闭全部可屏蔽中断并停机。 */
static void FreeRTOS_FaultStop(uint32_t code)
{
    __disable_irq();
    g_freertos_fault_code = code;
    for (;;)
    {
        __NOP();
    }
}

/** @brief 处理内核或应用断言失败，保存触发位置。 */
void vApplicationAssertFailed(const char *file, uint32_t line)
{
    g_freertos_fault_file = file;
    g_freertos_fault_line = line;
    FreeRTOS_FaultStop(1U);
}

/** @brief heap_4 无法满足内存分配时，由内核调用此钩子。 */
void vApplicationMallocFailedHook(void)
{
    FreeRTOS_FaultStop(2U);
}

/** @brief 内核检测到任务栈溢出时，保存任务名称并停机。 */
void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    g_freertos_fault_task = task_name;
    FreeRTOS_FaultStop(3U);
}
