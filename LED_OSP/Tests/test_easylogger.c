/**
 * @file test_easylogger.c
 * @brief 使用真实 EasyLogger 核心和移植层的独立主机测试。
 * @details 仅模拟 FreeRTOS 与 UART；不访问真实 MCU 寄存器。
 *          每个命名场景在新进程运行，避免伪造日志服务静态状态重置。
 */
#define LOG_TAG "HOST"
#define LOG_LVL 4
#include "elog.h"
#include "elog_port.h"
#include "log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/* EasyLogger 原核心通过这些移植回调调用 BSP，没有统一公开回调头文件。 */
void elog_port_output(const char *log, size_t size);
void elog_port_output_lock(void);
void elog_port_output_unlock(void);

#if defined(ELOG_ASYNC_OUTPUT_ENABLE) || defined(ELOG_ASYNC_OUTPUT_USING_PTHREAD)
#error "This port is required to use native FreeRTOS synchronous logging."
#endif
#if defined(ELOG_BUF_OUTPUT_ENABLE) || defined(ELOG_COLOR_ENABLE)
#error "Buffered output and ANSI color are disabled for this UART port."
#endif

static unsigned checks, create_calls, take_calls, give_calls, delete_calls;
static unsigned uart_calls, tick_reads, task_name_reads;
static unsigned lock_depth;
static unsigned expected_uart_depth = 1U;
static int create_succeeds = 1;
static int take_succeeds = 1;
static int inside_interrupt;
static int check_format_lock;
static int binary_output_expected;
static BaseType_t scheduler_state = taskSCHEDULER_RUNNING;
static TickType_t tick_count = 12345U;
static TaskHandle_t current_task = (TaskHandle_t)(uintptr_t)0x10U;
static TaskHandle_t mutex_owner;
static const char *task_name = "AppTask";
static unsigned mutex_token;
static uint32_t last_uart_length;

/** @brief 独立输出接收区，包含前后保护字节。 */
static struct {
    uint8_t before[16];
    char bytes[512];
    uint8_t after[16];
} captured;

static jmp_buf assertion_target;
static int expect_assertion;
static unsigned assertion_count;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
        exit(1); \
    } \
} while (0)



/** @brief 允许测试捕获非法调用断言；正常路径断言失败立即判测试失败。 */
void EasyLogger_TestAssert(BaseType_t condition, const char *expression, int line)
{
    if (condition) {
        return;
    }
    ++assertion_count;
    if (expect_assertion) {
        expect_assertion = 0;
        longjmp(assertion_target, 1);
    }
    fprintf(stderr, "Unexpected FreeRTOS assertion at port line %d: %s\n",
            line, expression);
    exit(1);
}

BaseType_t xPortIsInsideInterrupt(void)
{
    return inside_interrupt ? pdTRUE : pdFALSE;
}

BaseType_t xTaskGetSchedulerState(void)
{
    return scheduler_state;
}

TickType_t xTaskGetTickCount(void)
{
    ++tick_reads;
    if (check_format_lock) {
        CHECK(lock_depth > 0U && mutex_owner == current_task);
    }
    return tick_count;
}

const char *pcTaskGetName(TaskHandle_t task)
{
    ++task_name_reads;
    CHECK(task == NULL || task == current_task);
    if (check_format_lock) {
        CHECK(lock_depth > 0U && mutex_owner == current_task);
    }
    return task_name;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return current_task;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    ++create_calls;
    CHECK(!inside_interrupt && scheduler_state == taskSCHEDULER_RUNNING);
    return create_succeeds ? (SemaphoreHandle_t)&mutex_token : NULL;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, TickType_t wait)
{
    ++take_calls;
    CHECK(mutex == (SemaphoreHandle_t)&mutex_token);
    CHECK(!inside_interrupt && scheduler_state == taskSCHEDULER_RUNNING);
    CHECK(wait == portMAX_DELAY);
    if (!take_succeeds) {
        return pdFALSE;
    }
    CHECK(lock_depth == 0U || mutex_owner == current_task);
    mutex_owner = current_task;
    ++lock_depth;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex)
{
    ++give_calls;
    CHECK(mutex == (SemaphoreHandle_t)&mutex_token);
    CHECK(lock_depth > 0U && mutex_owner == current_task);
    --lock_depth;
    if (lock_depth == 0U) {
        mutex_owner = NULL;
    }
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    CHECK(mutex == (SemaphoreHandle_t)&mutex_token);
    ++delete_calls;
}

/**
 * @brief 模拟 USART1 输出，检查 UART 整行发送期间日志锁仍然被持有。
 * @note 函数只复制实际长度，不扫描未发送的缓冲容量。
 */
void USART1_Send(const uint8_t *data, uint32_t length)
{
    unsigned i;
    CHECK(data != NULL && length > 0U && length < sizeof(captured.bytes));
    if (!binary_output_expected) {
        CHECK(length <= ELOG_LINE_BUF_SIZE);
    }
    CHECK(lock_depth == expected_uart_depth && mutex_owner == current_task);
    CHECK(!inside_interrupt && scheduler_state == taskSCHEDULER_RUNNING);
    ++uart_calls;
    last_uart_length = length;
    memcpy(captured.bytes, data, length);
    captured.bytes[length] = '\0';
    for (i = 0U; i < sizeof(captured.before); ++i) {
        CHECK(captured.before[i] == 0xA5U && captured.after[i] == 0xA5U);
    }
}

/** @brief 每个观察窗口只重置 mock 计数，不篡改被测服务的静态状态。 */
static void reset_observations(void)
{
    CHECK(lock_depth == 0U);
    memset(&captured, 0xA5, sizeof(captured));
    captured.bytes[0] = '\0';
    last_uart_length = 0U;
    take_calls = give_calls = uart_calls = tick_reads = task_name_reads = 0U;
}

/** @brief 正常格式化日志保持 Take/Give 配对，正文不含 NUL，并以 CRLF 结束。 */
static void check_complete_line(const char *body)
{
    /* 核心可能在查询标签过滤器时另行持锁，不能把内部次数写死为 1。 */
    CHECK(uart_calls == 1U && take_calls > 0U && take_calls == give_calls);
    CHECK(lock_depth == 0U);
    CHECK(last_uart_length >= 2U && last_uart_length <= ELOG_LINE_BUF_SIZE);
    CHECK(captured.bytes[last_uart_length - 2U] == '\r');
    CHECK(captured.bytes[last_uart_length - 1U] == '\n');
    CHECK(strlen(captured.bytes) == last_uart_length);
    if (body != NULL) {
        CHECK(strstr(captured.bytes, body) != NULL);
    }
}

static void expect_direct_output_assert(void)
{
    unsigned before = assertion_count;
    if (setjmp(assertion_target) == 0) {
        expect_assertion = 1;
        elog_port_output("illegal\r\n", 9U);
        expect_assertion = 0;
        CHECK(0); /* 非法直接输出不能悄悄绕过锁或绑定。 */
    }
    CHECK(assertion_count == before + 1U);
    CHECK(uart_calls == 0U && lock_depth == 0U);
}

/** @brief 初始化前不输出；无效设备和非任务上下文也不能创建日志资源。 */
static void test_not_ready(void)
{
    reset_observations();
    log_i("before ready");
    CHECK(uart_calls == 0U && take_calls == 0U && give_calls == 0U);
    CHECK(!Log_WriteRaw((const uint8_t *)"x", 1U));
    CHECK(!Log_WriteRaw(NULL, 0U));
    CHECK(create_calls == 0U && uart_calls == 0U);
    scheduler_state = taskSCHEDULER_NOT_STARTED;
    CHECK(!Log_Init());
    scheduler_state = taskSCHEDULER_SUSPENDED;
    CHECK(!Log_Init());
    scheduler_state = taskSCHEDULER_RUNNING;
    inside_interrupt = 1;
    CHECK(!Log_Init());
    inside_interrupt = 0;
    CHECK(create_calls == 0U && uart_calls == 0U);
    expect_direct_output_assert();
}

/** @brief 创建互斥失败不会发布半初始化绑定，资源恢复后可以重试。 */
static void test_mutex_init_failure(void)
{
    reset_observations();
    create_succeeds = 0;
    CHECK(!Log_Init());
    CHECK(create_calls == 1U && uart_calls == 0U);
    log_e("must stay silent after allocation failure");
    CHECK(uart_calls == 0U && take_calls == 0U);
    create_succeeds = 1;
    CHECK(Log_Init());
    CHECK(create_calls == 2U && uart_calls == 1U);
    CHECK(take_calls == give_calls && lock_depth == 0U);
}

static void test_repeated_initialization(void)
{
    unsigned initial_create_calls = create_calls;
    reset_observations();
    CHECK(Log_Init());
    CHECK(Elog_Port_Init());
    CHECK(create_calls == initial_create_calls);
    CHECK(uart_calls == 0U && take_calls == 0U);
    CHECK(create_calls == initial_create_calls && uart_calls == 0U);
}

/** @brief 真正的 log_e/log_i/log_d 宏完成格式化、标签、时间及任务名输出。 */
static void test_formatted_levels(void)
{
    reset_observations();
    check_format_lock = 1;
    log_e("error=%d", 7);
    check_complete_line("error=7");
    CHECK(strstr(captured.bytes, "E/HOST") != NULL);
    CHECK(strstr(captured.bytes, "[12345 AppTask]") != NULL);
    CHECK(tick_reads > 0U && task_name_reads > 0U);

    reset_observations();
    log_i("bytes=%u crc=%08lX", 513U, 0xCBF43926UL);
    check_complete_line("bytes=513 crc=CBF43926");
    CHECK(strstr(captured.bytes, "I/HOST") != NULL);
    CHECK(strstr(captured.bytes, "12345") != NULL);
    CHECK(strstr(captured.bytes, "AppTask") != NULL);

    reset_observations();
    log_d("debug=%s", "visible");
    check_complete_line("debug=visible");
    CHECK(strstr(captured.bytes, "D/HOST") != NULL);
}

/** @brief 日志等级过滤在格式化和 UART 发送之前生效。 */
static void test_level_filtering(void)
{
    elog_set_filter_lvl(ELOG_LVL_WARN);
    reset_observations();
    log_i("hidden info");
    log_d("hidden debug");
    CHECK(uart_calls == 0U && take_calls == 0U && give_calls == 0U);
    log_e("visible error");
    check_complete_line("visible error");

    elog_set_filter_lvl(ELOG_LVL_INFO);
    reset_observations();
    log_d("still hidden debug");
    CHECK(uart_calls == 0U && take_calls == 0U);
    log_i("visible info");
    check_complete_line("visible info");

    elog_set_filter_lvl(ELOG_LVL_DEBUG);
    reset_observations();
    log_d("restored debug");
    check_complete_line("restored debug");
    reset_observations();
    log_v("compiled-out verbose");
    CHECK(uart_calls == 0U && take_calls == 0U);
}

/** @brief 超长正文及标签会被截断；后续正常日志不受影响。 */
static void test_long_lines(void)
{
    char large[4097];
    memset(large, 'X', sizeof(large) - 1U);
    large[sizeof(large) - 1U] = '\0';
    reset_observations();
    log_i("%s", large);
    check_complete_line(NULL);
    CHECK(last_uart_length == ELOG_LINE_BUF_SIZE);
    CHECK(strstr(captured.bytes, "I/HOST") != NULL);
    CHECK(strstr(captured.bytes, "XXXXXXXXXXXXXXXX") != NULL);

    reset_observations();
    elog_i(large, "%s", large);
    check_complete_line(NULL);
    CHECK(last_uart_length == ELOG_LINE_BUF_SIZE);

    reset_observations();
    log_i("after truncation=%u", 42U);
    check_complete_line("after truncation=42");
    CHECK(strstr(captured.bytes, "12345") != NULL);
}

/** @brief 递归调用仍由同一任务持锁，并在最外层退出时完全释放。 */
static void test_recursive_lock(void)
{
    reset_observations();
    elog_port_output_lock();
    CHECK(lock_depth == 1U);
    expected_uart_depth = 2U;
    log_i("recursive");
    CHECK(uart_calls == 1U && take_calls > 1U && take_calls == give_calls + 1U);
    CHECK(lock_depth == 1U);
    elog_port_output_unlock();
    expected_uart_depth = 1U;
    CHECK(lock_depth == 0U && take_calls == give_calls);
}

/** @brief 时戳上限可格式化；没有任务名时使用有效空串。 */
static void test_time_and_task_edge_cases(void)
{
    reset_observations();
    tick_count = UINT32_MAX;
    log_i("tick wrap boundary");
    check_complete_line("tick wrap boundary");
    CHECK(strstr(captured.bytes, "4294967295") != NULL);
    tick_count = 12345U;

    reset_observations();
    task_name = NULL;
    log_i("missing task name");
    check_complete_line("missing task name");
    CHECK(strstr(captured.bytes, "12345") != NULL);
    task_name = "AppTask";
}

/** @brief 回显也使用同一锁，保留二进制和长度，不追加格式或 CRLF。 */
static void test_binary_raw_service(void)
{
    uint8_t data[300];
    unsigned i;
    for (i = 0U; i < sizeof(data); ++i) {
        data[i] = (uint8_t)(i * 19U);
    }
    data[0] = 0x00U;
    data[1] = 0xFFU;
    data[2] = 0x25U; /* '%' 也不能被当成 printf 格式。 */
    data[sizeof(data) - 2U] = 0x00U;
    data[sizeof(data) - 1U] = 0xFFU;
    reset_observations();
    CHECK(!Log_WriteRaw(NULL, 1U));
    CHECK(Log_WriteRaw(NULL, 0U));
    CHECK(uart_calls == 0U && take_calls == 0U && give_calls == 0U);

    binary_output_expected = 1;
    CHECK(Log_WriteRaw(data, sizeof(data)));
    binary_output_expected = 0;
    CHECK(uart_calls == 1U && take_calls > 0U && take_calls == give_calls);
    CHECK(lock_depth == 0U && last_uart_length == sizeof(data));
    CHECK(memcmp(captured.bytes, data, sizeof(data)) == 0);
    CHECK(tick_reads == 0U && task_name_reads == 0U); /* 没有格式化步骤。 */
}

static void test_logging(void)
{
    reset_observations();
    CHECK(Log_Init());
    CHECK(create_calls == 1U && uart_calls == 1U);
    CHECK(take_calls == give_calls && lock_depth == 0U);
    test_repeated_initialization();
    test_formatted_levels();
    test_level_filtering();
    test_long_lines();
    test_recursive_lock();
    test_time_and_task_edge_cases();
    test_binary_raw_service();
    reset_observations();
    expect_direct_output_assert(); /* 绑定成功也不能无锁直接发送。 */
    CHECK(delete_calls == 0U);
}

/** @brief 已初始化后从 ISR 调用日志也必须断言，不能阻塞或输出。 */
static void test_isr_rejection(void)
{
    unsigned old_assertions;
    reset_observations();
    CHECK(Log_Init());
    reset_observations();
    inside_interrupt = 1;
    CHECK(!Log_Init());
    old_assertions = assertion_count;
    if (setjmp(assertion_target) == 0) {
        expect_assertion = 1;
        log_i("forbidden ISR log");
        expect_assertion = 0;
        CHECK(0);
    }
    CHECK(assertion_count == old_assertions + 1U);
    CHECK(take_calls == 0U && give_calls == 0U && uart_calls == 0U);
    CHECK(lock_depth == 0U);
}

/** @brief 锁获取失败发生在格式化及 UART 输出之前。 */
static void test_lock_failure(void)
{
    unsigned old_assertions;
    reset_observations();
    CHECK(Log_Init());
    reset_observations();
    take_succeeds = 0;
    old_assertions = assertion_count;
    if (setjmp(assertion_target) == 0) {
        expect_assertion = 1;
        log_i("cannot get lock");
        expect_assertion = 0;
        CHECK(0);
    }
    CHECK(assertion_count == old_assertions + 1U);
    CHECK(take_calls == 1U && give_calls == 0U && uart_calls == 0U);
    CHECK(tick_reads == 0U && task_name_reads == 0U && lock_depth == 0U);
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"not_ready", test_not_ready},
        {"mutex_failure", test_mutex_init_failure},
        {"logging", test_logging},
        {"isr_rejection", test_isr_rejection},
        {"lock_failure", test_lock_failure}
    };
    unsigned i;
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            puts(cases[i].name);
        }
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: test_easylogger CASE or --list\n");
        return 2;
    }
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            printf("PASS EasyLogger %s (%u checks)\n", cases[i].name, checks);
            return 0;
        }
    }
    fprintf(stderr, "Unknown case: %s\n", argv[1]);
    return 2;
}
