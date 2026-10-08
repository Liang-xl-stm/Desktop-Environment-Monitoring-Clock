/**
 * @file    FreeRTOSConfig.h
 * @brief   STM32F407ZG 的 FreeRTOS 内核配置。
 * @details 使用 ARMCC 5 的 RVDS/ARM_CM4F 端口，系统节拍为 1 ms。
 *          内核源码保持原样，板级配置集中在本文件。
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

/** @brief CMSIS 提供的当前内核时钟，main 在启动调度器前更新。 */
extern uint32_t SystemCoreClock;

/* 调度：单核抢占式，任务优先级为 0～4，数值越大越优先。 */
#define configUSE_PREEMPTION                     1
#define configUSE_TIME_SLICING                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION   1
#define configCPU_CLOCK_HZ                       (SystemCoreClock)
#define configTICK_RATE_HZ                       1000U
#define configTICK_TYPE_WIDTH_IN_BITS            TICK_TYPE_WIDTH_32_BITS
#define configMAX_PRIORITIES                     5U
#define configMAX_TASK_NAME_LEN                  16U
#define configIDLE_SHOULD_YIELD                   1
#define configUSE_TICKLESS_IDLE                  0

/* 栈大小按 32 位字计；heap_4 的堆大小按字节计。 */
#define configMINIMAL_STACK_SIZE                 128U
#define configTOTAL_HEAP_SIZE                    (32U * 1024U)
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configSUPPORT_STATIC_ALLOCATION          0
#define configAPPLICATION_ALLOCATED_HEAP         0

/* 提供后续通信任务所需的同步机制；本次不创建软件定时器任务。 */
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configUSE_TASK_NOTIFICATIONS             1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    1U
#define configQUEUE_REGISTRY_SIZE                8U
#define configUSE_QUEUE_SETS                     0
#define configUSE_TIMERS                         0
#define configUSE_CO_ROUTINES                    0

/* 故障检查开启；未使用的 tick/idle 钩子及运行时间统计关闭。 */
#define configUSE_MALLOC_FAILED_HOOK             1
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configUSE_TRACE_FACILITY                 0
#define configGENERATE_RUN_TIME_STATS            0
#define configUSE_STATS_FORMATTING_FUNCTIONS     0
#define configUSE_NEWLIB_REENTRANT               0

/* STM32F407 实现四位中断优先级；main 配置全部为抢占优先级。
 * 中断数值越小越紧急。调用 RTOS FromISR API 的中断必须使用 5～15。
 */
/* 这些宏同时用于 ARMCC 内嵌汇编，整数不能带 C 的 U 后缀或类型转换。 */
#define configPRIO_BITS                          4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY  15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* 将官方端口函数映射到 ST 启动文件中的异常入口。 */
#define vPortSVCHandler                          SVC_Handler
#define xPortPendSVHandler                       PendSV_Handler
#define xPortSysTickHandler                      SysTick_Handler

/* 应用及后续通信移植需要的任务接口。 */
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskDelayUntil                  1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_uxTaskGetStackHighWaterMark       1
#define INCLUDE_eTaskGetState                    1

/** @brief 断言故障入口，记录位置并停机；实现位于 freertos_hooks.c。 */
void vApplicationAssertFailed(const char *file, uint32_t line);

#define configASSERT(condition)                                      \
    do                                                               \
    {                                                                \
        if (!(condition))                                            \
        {                                                            \
            vApplicationAssertFailed(__FILE__, (uint32_t)__LINE__);    \
        }                                                            \
    } while (0)

#endif /* FREERTOS_CONFIG_H */
