/**
 * @file log.c
 * @brief 统一初始化日志资源及等级/标签/时间/任务名格式。
 */
#include "log.h"
#include "elog_port.h"
#include "elog.h"
#include <stddef.h>

/* 原库公开头文件没有声明这三个移植/内部符号；
 * 在此按原核心的函数类型声明，原样输出复用相同格式化/发送互斥。
 */
extern void elog_output_lock(void);
extern void elog_output_unlock(void);
extern void elog_port_output(const char *log, size_t size);

/** @brief 日志完成初始化后发布此标志，控制台任务才能输出。 */
static volatile uint8_t log_initialized;

/**
 * @brief 在运行中任务首次建立并启动同步串口日志。
 * @return 1 初始化成功或重复调用；0 参数、上下文或资源失败。
 * @note 顺序为：UART/互斥绑定 -> 核心初始化 -> 各级格式与过滤配置 -> 启动。
 */
uint8_t Log_Init(void)
{
    uint8_t level;
    ElogErrCode result;
    const size_t format = ELOG_FMT_LVL | ELOG_FMT_TAG |
                          ELOG_FMT_TIME | ELOG_FMT_T_INFO;

    /* 绑定检查也保证当前在运行中任务，不能在 ISR 或启动调度前初始化。 */
    if (!Elog_Port_Init())
    {
        return 0U;
    }
    if (log_initialized)
    {
        return 1U;
    }
    result = elog_init();
    if (result != ELOG_NO_ERR)
    {
        return 0U;
    }
    for (level = ELOG_LVL_ASSERT; level <= ELOG_LVL_VERBOSE; ++level)
    {
        elog_set_fmt(level, format);
    }
    elog_set_filter_lvl(ELOG_LVL_DEBUG);
    /* 确保格式、UART、互斥已经完整准备后，启动并输出库初始化信息。 */
    elog_start();
    log_initialized = 1U;
    return 1U;
}

/**
 * @brief 通过日志互斥量同步发送一批不经格式化的原始数据。
 * @param[in] data 本批原始数据；非零长度时必须有效，允许包含零字节。
 * @param[in] length 实际有效长度；零长度无需锁，也不访问 data。
 * @return 1 同步发送完成或零长度；0 未初始化或数据无效。
 * @note 调用方必须处于运行中的任务，数据在返回前不得被覆盖。
 */
uint8_t Log_WriteRaw(const uint8_t *data,
                             uint32_t length)
{
    if (!log_initialized ||
        (length > 0U && data == NULL))
    {
        return 0U;
    }
    if (length == 0U)
    {
        return 1U;
    }
    /* 和所有 EasyLogger 格式日志使用同一把递归互斥，整批发送期间不释放。 */
    elog_output_lock();
    elog_port_output((const char *)data, (size_t)length);
    elog_output_unlock();
    return 1U;
}

uint8_t Log_IsReady(void)
{
    return log_initialized;
}
