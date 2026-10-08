/**
 * @file elog_port.c
 * @brief EasyLogger 的原生 FreeRTOS、同步 UART 移植接口。
 * @details 递归互斥覆盖核心格式化及整条 UART 输出。
 *          不操作硬件初始化、不使用 ISR 发送、DMA 或异步日志线程。
 */

#include "elog_port.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "elog.h"
#include <stddef.h>
#include <stdio.h>

/** @brief 保护日志共享缓冲、格式化及发送的递归互斥量。 */
static SemaphoreHandle_t log_mutex = NULL;
/** @brief 当前互斥持有任务，用于拒绝没有持锁的直接输出。 */
static TaskHandle_t log_owner = NULL;
/** @brief 当前持有任务的递归层数。 */
static uint32_t log_lock_depth = 0U;
/** @brief 时间格式缓冲，仅在日志互斥持有期间访问。 */
static char log_time[24];

/**
 * @brief 检查当前是否为允许阻塞日志的运行中任务上下文。
 * @return 1 为任务且调度器运行；0 为 ISR、启动前或调度暂停期间。
 * @note 先检查 ISR，避免在中断里调用阻塞型 RTOS 接口。
 */
static uint8_t log_task_context(void)
{
    if (xPortIsInsideInterrupt() != pdFALSE)
    {
        return 0U;
    }
    return (uint8_t)(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
}

/**
 * @brief 检查输出资源和当前任务持锁情况。
 * @return 1 当前任务可以安全输出；0 未绑定、上下文错误或未持锁。
 * @note 错误调用交由各入口触发 configASSERT，不能偷偷发送到其他串口。
 */
static uint8_t log_output_allowed(void)
{
    return (uint8_t)(log_mutex != NULL &&
                     log_task_context() && log_lock_depth > 0U &&
                     log_owner == xTaskGetCurrentTaskHandle());
}

/**
 * @brief 在运行中任务首次创建 USART1 日志递归互斥量。
 * @return 1 成功或重复初始化；0 上下文或互斥资源失败。
 */
uint8_t Elog_Port_Init(void)
{
    SemaphoreHandle_t mutex;

    if (!log_task_context())
    {
        return 0U;
    }
    if (log_mutex != NULL)
    {
        return 1U;
    }
    mutex = xSemaphoreCreateRecursiveMutex();
    if (mutex == NULL)
    {
        return 0U;
    }
    /* 资源都创建成功后才发布互斥资源，避免输出访问半初始化状态。 */
    log_mutex = mutex;
    return 1U;
}

/**
 * @brief 核心初始化时确认 USART1 日志互斥资源已经建立。
 * @return ELOG_NO_ERR 资源完整；其他值使 elog_init 拒绝启动。
 * @note ElogErrCode 原库只命名了成功值，此处显式转换一个非零失败码。
 */
ElogErrCode elog_port_init(void)
{
    if (log_mutex == NULL || !log_task_context())
    {
        return (ElogErrCode)1U;
    }
    return ELOG_NO_ERR;
}

/**
 * @brief 核心反初始化回调，本固件保留日志资源到系统重启。
 * @return ELOG_NO_ERR，资源保留策略不执行可能失败的销毁操作。
 * @note 运行中不销毁可能被其他任务等待的互斥量，也不重新USART1。
 */
ElogErrCode elog_port_deinit(void)
{
    /* 本工程由 Log_Init 一次初始化，资源生命周期覆盖整个运行期。 */
    return ELOG_NO_ERR;
}

/**
 * @brief 把核心已经格式化的整条日志同步发送到USART1。
 * @param[in] log 日志起始地址，至少 size 字节，函数返回前必须有效。
 * @param[in] size 实际日志字节数，不能用缓冲容量代替有效长度。
 * @return 无；非法上下文、无锁或未绑定时触发 configASSERT，不执行发送。
 * @note 必须由核心在已持有日志互斥时调用，不能直接绕过核心调用。
 */
void elog_port_output(const char *log, size_t size)
{
    uint8_t allowed;

    if (size == 0U)
    {
        return;
    }
    allowed = (uint8_t)(log != NULL && size <= UINT32_MAX && log_output_allowed());
    configASSERT(allowed != 0U);
    if (!allowed)
    {
        return;
    }
    USART1_Send((const uint8_t *)log, (uint32_t)size);
}

/**
 * @brief 获取递归互斥，保护核心共享行缓冲的格式化和发送。
 * @return 无；未初始化、ISR 或获取失败触发 FreeRTOS 断言。
 * @note 库在格式化前调用，不能只保护最后 USART1_Send 的字节发送。
 */
void elog_port_output_lock(void)
{
    uint8_t allowed = (uint8_t)(log_mutex != NULL &&
                               log_task_context());
    BaseType_t result;
    TaskHandle_t current;

    configASSERT(allowed != 0U);
    if (!allowed)
    {
        return;
    }
    result = xSemaphoreTakeRecursive(log_mutex, portMAX_DELAY);
    configASSERT(result == pdTRUE);
    if (result != pdTRUE)
    {
        return;
    }
    current = xTaskGetCurrentTaskHandle();
    configASSERT(log_lock_depth == 0U || log_owner == current);
    configASSERT(log_lock_depth < UINT32_MAX);
    log_owner = current;
    log_lock_depth++;
}

/**
 * @brief 释放当前任务持有的一层递归互斥。
 * @return 无；非持有任务、错误上下文或释放失败触发 FreeRTOS 断言。
 */
void elog_port_output_unlock(void)
{
    uint8_t allowed = log_output_allowed();
    BaseType_t result;

    configASSERT(allowed != 0U);
    if (!allowed)
    {
        return;
    }
    /* 在释放最终一层前清理持有记录，避免被随后取得互斥的任务覆盖。 */
    log_lock_depth--;
    if (log_lock_depth == 0U)
    {
        log_owner = NULL;
    }
    result = xSemaphoreGiveRecursive(log_mutex);
    configASSERT(result == pdTRUE);
}

/**
 * @brief 提供本次日志时间，单位为毫秒，从 RTOS tick 转换。
 * @return 指向持久时间缓冲的字符串；异常路径返回空串，不返回栈地址。
 * @note 核心已经持锁才调用；uint32_t 毫秒计数允许自然回绕。
 */
const char *elog_port_get_time(void)
{
    uint32_t milliseconds;
    uint8_t allowed = log_output_allowed();

    configASSERT(allowed != 0U);
    if (!allowed)
    {
        return "";
    }
    milliseconds = (uint32_t)xTaskGetTickCount() * (uint32_t)portTICK_PERIOD_MS;
    (void)snprintf(log_time, sizeof(log_time), "%lu", (unsigned long)milliseconds);
    return log_time;
}

/**
 * @brief 提供进程信息，当前 MCU 没有独立进程概念。
 * @return 固定返回有效空字符串。
 */
const char *elog_port_get_p_info(void)
{
    return "";
}

/**
 * @brief 提供当前产生日志的 FreeRTOS 任务名。
 * @return 当前任务名；异常路径或名字为空时返回有效空字符串。
 * @note 核心已持锁才调用，任务对象需在本次日志输出期间保持有效。
 */
const char *elog_port_get_t_info(void)
{
    const char *name;
    uint8_t allowed = log_output_allowed();

    configASSERT(allowed != 0U);
    if (!allowed)
    {
        return "";
    }
    name = pcTaskGetName(NULL);
    return name != NULL ? name : "";
}
