/** @file test_usart_rx.c @brief 用真实驱动验证连续 RXNE 发布及双路缓存隔离。 */
#include "stm32f4xx.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

USART_TypeDef test_usart1, test_usart3;
GPIO_TypeDef test_gpioa, test_gpiob;
void USART1_IRQHandler(void);
void USART3_IRQHandler(void);
static unsigned checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

/** 模拟硬件保存 SR/DR 后调用中断；调用结束表示这次硬件状态已消费。 */
static void push(USART_TypeDef *uart, uint8_t byte, uint32_t status)
{
    uart->DR = byte; uart->SR = status | USART_SR_TXE;
    if (uart == USART3) USART3_IRQHandler(); else USART1_IRQHandler();
    uart->SR = USART_SR_TXE;
}

static void test_immediate_publish(void)
{
    uint8_t data[16] = {0};
    USART3_Init(115200U);
    push(USART3, 0x50U, USART_SR_RXNE);
    CHECK(USART3_Receive(data, sizeof(data)) == 1U);
    CHECK(data[0] == 0x50U);
    CHECK(USART3_Receive(data, sizeof(data)) == 0U);
}

/**
 * 连续 CIPRECVDATA：32 字节 AT 头 + 256 字节正文 + 6 字节 OK 尾。
 * 115200 波特率下约每 1ms 收到 11.52 字节，本例每 12 字节模拟一次接收任务。
 * 全程不发 IDLE，必须在缓存尚未满时即可消费，且多包跨环形边界不丢字节。
 */
static void test_continuous_reply(void)
{
    enum { HEADER = 32, BODY = 256, TAIL = 6, PACKET = HEADER + BODY + TAIL, TOTAL = PACKET * 10 };
    uint8_t expected[TOTAL], actual[TOTAL], buffer[64];
    size_t offset, count = 0U;
    uint16_t received;
    USART3_Init(115200U);
    for (offset = 0U; offset < TOTAL; ++offset)
    {
        size_t position = offset % PACKET;
        expected[offset] = (uint8_t)(position < HEADER ? (0x20U + position) :
            position < HEADER + BODY ? position - HEADER : (0x80U + position));
    }
    for (offset = 0U; offset < TOTAL; ++offset)
    {
        push(USART3, expected[offset], USART_SR_RXNE);
        if (offset % 12U == 11U)
        {
            while ((received = USART3_Receive(buffer, sizeof(buffer))) != 0U)
            {
                if (count + received <= sizeof(actual)) memcpy(actual + count, buffer, received);
                count += received;
            }
        }
    }
    while ((received = USART3_Receive(buffer, sizeof(buffer))) != 0U)
    {
        if (count + received <= sizeof(actual)) memcpy(actual + count, buffer, received);
        count += received;
    }
    if (count != TOTAL) fprintf(stderr, "Continuous stream received %lu/%u bytes without IDLE\n", (unsigned long)count, TOTAL);
    CHECK(count == TOTAL);
    CHECK(count == TOTAL && memcmp(actual, expected, TOTAL) == 0);
}

static void test_idle_errors_and_independence(void)
{
    uint8_t data[16];
    USART1_Init(115200U); USART3_Init(115200U);
    push(USART1, 0xA1U, USART_SR_RXNE | USART_SR_IDLE);
    push(USART3, 0xB3U, USART_SR_RXNE | USART_SR_IDLE);
    CHECK(USART3_Receive(data, sizeof(data)) == 1U && data[0] == 0xB3U);
    CHECK(USART1_Receive(data, sizeof(data)) == 1U && data[0] == 0xA1U);
    /* IDLE 单独发生不得把无效 DR 值当作正文再次收入。 */
    push(USART3, 0xCCU, USART_SR_IDLE);
    CHECK(USART3_Receive(data, sizeof(data)) == 0U);
    push(USART3, 0xDDU, USART_SR_RXNE | USART_SR_FE | USART_SR_IDLE);
    CHECK(USART3_Receive(data, sizeof(data)) == 0U);
    /* 仅 ORE 保留 DR 中仍有效的旧字节，不应将其再丢弃。 */
    push(USART3, 0xEEU, USART_SR_RXNE | USART_SR_ORE | USART_SR_IDLE);
    CHECK(USART3_Receive(data, sizeof(data)) == 1U && data[0] == 0xEEU);
    push(USART1, 0x10U, USART_SR_RXNE);
    push(USART3, 0x30U, USART_SR_RXNE);
    USART1_Init(115200U);
    CHECK(USART1_Receive(data, sizeof(data)) == 0U);
    CHECK(USART3_Receive(data, sizeof(data)) == 1U && data[0] == 0x30U);
}

static void test_capacity_and_wrap(void)
{
    uint8_t data[300];
    unsigned i;
    uint16_t received;
    USART3_Init(115200U);
    for (i = 0U; i < 300U; ++i) push(USART3, (uint8_t)i, USART_SR_RXNE);
    received = USART3_Receive(data, sizeof(data));
    CHECK(received == 255U);
    for (i = 0U; i < received; ++i) CHECK(data[i] == (uint8_t)i);
    CHECK(USART3_Receive(data, sizeof(data)) == 0U);
    /* 消费满缓存后，跨下标 255 -> 0 的新数据应即时可见。 */
    for (i = 0U; i < 32U; ++i) push(USART3, (uint8_t)(0xA0U + i), USART_SR_RXNE);
    received = USART3_Receive(data, sizeof(data));
    CHECK(received == 32U);
    for (i = 0U; i < received; ++i) CHECK(data[i] == (uint8_t)(0xA0U + i));
    CHECK(USART3_Receive(NULL, 1U) == 0U);
    CHECK(USART3_Receive(data, 0U) == 0U);
}

int main(void)
{
    test_immediate_publish(); test_continuous_reply();
    test_idle_errors_and_independence(); test_capacity_and_wrap();
    printf("USART RX: %u checks, %u failures\n", checks, failures);
    return failures != 0U;
}
