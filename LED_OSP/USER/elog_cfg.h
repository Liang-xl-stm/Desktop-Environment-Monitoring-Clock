/**
 * @file elog_cfg.h
 * @brief EasyLogger 的 STM32/FreeRTOS 同步串口配置。
 * @details 使用 256 字节行缓冲，由 USART1 同步发送；
 *          不启用异步、pthread、缓冲输出、DMA 或 ANSI 颜色。
 */
#ifndef ELOG_CFG_H
#define ELOG_CFG_H

/** @brief 编译启用日志输出；初始化前核心不会发送日志。 */
#define ELOG_OUTPUT_ENABLE
/** @brief 编译保留 Assert/Error/Warn/Info/Debug，裁掉 Verbose。 */
#define ELOG_OUTPUT_LVL             ELOG_LVL_DEBUG
/** @brief 启用库内部参数断言，错误参数不能继续执行。 */
#define ELOG_ASSERT_ENABLE
/** @brief 全局单行缓冲容量，长日志按库规则截断。 */
#define ELOG_LINE_BUF_SIZE          256
/** @brief 库记录行号时允许的字符数。 */
#define ELOG_LINE_NUM_MAX_LEN       10U
/** @brief 日志过滤标签的最大字符数，不含结尾零。 */
#define ELOG_FILTER_TAG_MAX_LEN     16U
/** @brief 日志过滤关键字最大字符数，不含结尾零。 */
#define ELOG_FILTER_KW_MAX_LEN      16U
/** @brief 允许独立设置过滤等级的标签数量。 */
#define ELOG_FILTER_TAG_LVL_MAX_NUM 5U
/** @brief 普通格式日志以 CRLF 结束，适配串口调试助手。 */
#define ELOG_NEWLINE_SIGN           "\r\n"

/* 有意不定义 ELOG_COLOR_ENABLE、ELOG_ASYNC_OUTPUT_ENABLE、
 * ELOG_ASYNC_OUTPUT_USING_PTHREAD、ELOG_BUF_OUTPUT_ENABLE。
 * 库使用 #ifdef 检查开关，定义为 0 仍会启用对应功能。
 */

#endif /* ELOG_CFG_H */
