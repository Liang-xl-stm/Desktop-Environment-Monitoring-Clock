/**
 * @file usart.c
 * @brief 串口驱动 USART1、USART3 的轮询发送和中断接收实现。
 * @details 两路串口共用内部实现，每路拥有独立的硬件上下文和接收缓存。
 *          RXNE 中断保存字节，IDLE 中断发布数据；主循环只读取软件缓存。
 *          USART1 使用 PA9/PA10，USART3 使用 PB10/PB11，均配置为 8N1。
 */

#include "usart.h"
#include "stm32f4xx_usart.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx.h"
#include <stdio.h>

/** @brief 环形缓存大小；保留一个空位，有效容量为 255 字节。 */
#define UART_RX_BUFFER_SIZE 256U

/**
 * @brief 一路串口的硬件及接收状态。
 * @details 中断独占写位置和发布位置，主循环独占读位置。
 *          每个有效字节立即发布，任务无需等到 IDLE 或缓存满才能读取。
 *          volatile 使共享状态每次都实际访问，DMB 保证数据与位置的发布顺序。
 */
typedef struct
{
    USART_TypeDef *usart; /**< 本实例对应的 USART 寄存器。 */
    uint32_t baudrate; /**< 本实例最近一次初始化的波特率。 */
    volatile uint8_t rx_buf[UART_RX_BUFFER_SIZE]; /**< 中断接收环形缓存。 */
    volatile uint16_t rx_write_pos; /**< 中断下一次写入的位置。 */
    volatile uint16_t rx_read_pos; /**< 主循环下一次读取的位置。 */
    volatile uint16_t rx_ready_pos; /**< 已发布数据的末尾，读取时不包含此位置。 */
    volatile uint32_t rx_overflow_count; /**< 软件缓存满时丢弃的新字节数。 */
    volatile uint32_t rx_overrun_count; /**< 硬件 ORE 事件数，不代表精确丢失字节数。 */
    volatile uint32_t rx_error_count; /**< 帧、噪声或校验错误的事件数。 */
} uart_ctx_t;

/** @brief USART1 接收状态，生命周期覆盖整个固件运行期间。 */
static uart_ctx_t uart1_ctx =
{
    .usart = USART1,
    .baudrate = 115200
};

/** @brief USART3 接收状态，与 USART1 的缓存和位置完全独立。 */
static uart_ctx_t uart3_ctx =
{
    .usart = USART3,
    .baudrate = 115200
};

/**
 * @brief 轮询 TXE，依次将数据写入本路发送寄存器。
 * @param[in] uart 本路串口的内部接收状态。
 * @param[in] data 待发送的数据，长度非零时必须有效。
 * @param[in] length 待发送的字节数。
 * @note 阻塞调用者，但不关闭接收中断。返回时最后一个字节可能仍在发送。
 */
static void uart_send(uart_ctx_t *uart, const uint8_t *data, uint32_t length)
{

    if (data == NULL || length == 0U) return;

    for (uint32_t i = 0; i < length; i++)
    {
        while (!(uart->usart->SR & USART_SR_TXE));
        uart->usart->DR = data[i];
    }
}

/**
 * @brief 非阻塞取出本路已经发布的数据。
 * @param[in] uart 本路串口的内部接收状态。
 * @param[out] data 输出缓冲，容量至少为 length 字节。
 * @param[in] length 本次最多读取的字节数。
 * @return 本次实际复制的字节数；暂无已发布数据时返回 0。
 * @note 仅由主循环调用。复制期间不关闭中断，也不读取硬件 DR。
 */
static uint16_t uart_receive(uart_ctx_t *uart, uint8_t *data, uint16_t length)
{
    uint16_t received = 0;
    uint16_t read_pos = uart->rx_read_pos;
    uint16_t ready_pos = uart->rx_ready_pos;

    if (data == NULL || length == 0U) return 0U;

    /* 先取得发布位置，再读取对应的数据；本次不追赶后来发布的数据。 */
    __DMB();
    while (received < length && read_pos != ready_pos)
    {
        data[received] = uart->rx_buf[read_pos];
        received++;
        read_pos = (read_pos + 1U) % UART_RX_BUFFER_SIZE;
    }

    /* 复制完成后才释放缓存空间，防止中断覆盖正在复制的数据。 */
    __DMB();
    uart->rx_read_pos = read_pos;
    return received;
}

/**
 * @brief 中断中将一个有效字节保存到本路环形缓存。
 * @note 满缓存时丢弃新字节，保留尚未消费的数据；中断不修改读位置。
 */
static void uart_push_byte(uart_ctx_t *uart, uint8_t byte)
{
    uint16_t next_pos = (uart->rx_write_pos + 1U) % UART_RX_BUFFER_SIZE;

    if (next_pos == uart->rx_read_pos)
    {
        uart->rx_overflow_count++;
        return;
    }

    uart->rx_buf[uart->rx_write_pos] = byte;
    uart->rx_write_pos = next_pos;

    /* 每个字节写完就发布，不等待 IDLE 或缓存满。
     * ESP8266 的连续应答包含 AT 头和二进制正文，可超过 255 字节；
     * 等到满缓存才发布时，下一个字节可能在接收任务醒来前被丢弃。
     * DMB 确保缓冲内容先可见，再让任务读取这个新的结束位置。 */
    __DMB();
    uart->rx_ready_pos = next_pos;
}

/**
 * @brief 将 C 标准库的字符输出重定向到 USART1。
 * @param[in] ch 待输出的字符，发送时转换为一个无符号字节。
 * @param[in] stream 标准库传入的流对象；本实现统一输出到 USART1。
 * @return 实际发送的无符号字符值。
 * @note 适用于当前启用的 MicroLIB，printf 会逐字符调用本函数。
 *       须先调用 USART1_Init；仅在任务中使用，发送采用轮询方式。
 */
int fputc(int ch, FILE *stream)
{
    uint8_t byte = (uint8_t)ch;

    (void)stream;
    USART1_Send(&byte, 1U);
    return (int)byte;
}

/**
 * @brief 初始化本文件支持的 USART1 或 USART3 硬件及接收状态。
 * @param[in] uart 指向 uart1_ctx 或 uart3_ctx。
 * @param[in] baudrate 非零、在当前外设时钟下可用的波特率。
 * @note 重新初始化会丢弃本路尚未读取的数据，不影响另一串口。
 */
static void uart_hw_init(uart_ctx_t *uart, uint32_t baudrate)
{
    GPIO_InitTypeDef gpio;
    USART_InitTypeDef usart;
    GPIO_TypeDef *gpio_port;
    uint16_t gpio_pins;
    IRQn_Type irq = (uart->usart == USART1) ? USART1_IRQn : USART3_IRQn;
    volatile uint32_t clear_status;

    /* 配置及重置缓存期间，不允许本路接收中断访问状态。 */
    NVIC_DisableIRQ(irq);

    if (uart->usart == USART1)
    {
        /* USART1 在 APB2，PA9/PA10 复用为 AF7。 */
        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
        gpio_port = GPIOA;
        gpio_pins = GPIO_Pin_9 | GPIO_Pin_10;
        GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
        GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);
    }
    else
    {
        /* USART3 在 APB1，按原理图使用 PB10/PB11 的 AF7。 */
        RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
        RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
        gpio_port = GPIOB;
        gpio_pins = GPIO_Pin_10 | GPIO_Pin_11;
        GPIO_PinAFConfig(GPIOB, GPIO_PinSource10, GPIO_AF_USART3);
        GPIO_PinAFConfig(GPIOB, GPIO_PinSource11, GPIO_AF_USART3);
    }

    /* 时钟已开启；复位本路 USART，清除原有中断和 DMA 请求配置。 */
    USART_DeInit(uart->usart);
    uart->baudrate = baudrate;
    uart->rx_write_pos = 0;
    uart->rx_read_pos = 0;
    uart->rx_ready_pos = 0;
    uart->rx_overflow_count = 0;
    uart->rx_overrun_count = 0;
    uart->rx_error_count = 0;

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin   = gpio_pins;
    gpio.GPIO_Mode  = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_PuPd  = GPIO_PuPd_UP;
    GPIO_Init(gpio_port, &gpio);

    USART_StructInit(&usart);
    usart.USART_BaudRate            = baudrate;
    usart.USART_WordLength          = USART_WordLength_8b;
    usart.USART_StopBits            = USART_StopBits_1;
    usart.USART_Parity              = USART_Parity_No;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(uart->usart, &usart);

    /* 先读 SR 再读 DR，清除残留接收、空闲和错误状态。 */
    clear_status = uart->usart->SR;
    clear_status = uart->usart->DR;
    (void)clear_status;
    NVIC_ClearPendingIRQ(irq);

    /* CMSIS 逻辑优先级为 5，不修改全局中断优先级分组。 */
    NVIC_SetPriority(irq, 5);
    USART_ITConfig(uart->usart, USART_IT_RXNE, ENABLE);
    USART_ITConfig(uart->usart, USART_IT_IDLE, ENABLE);
    USART_ITConfig(uart->usart, USART_IT_ERR, ENABLE);
    NVIC_EnableIRQ(irq);

    /* 状态和中断入口都准备完毕后，最后使能本路串口。 */
    USART_Cmd(uart->usart, ENABLE);
}

void USART1_Init(uint32_t baudrate)
{
    if (baudrate != 0U) uart_hw_init(&uart1_ctx, baudrate);
}

void USART3_Init(uint32_t baudrate)
{
    if (baudrate != 0U) uart_hw_init(&uart3_ctx, baudrate);
}

void USART1_Send(const uint8_t *data, uint32_t length)
{
    uart_send(&uart1_ctx, data, length);
}

void USART3_Send(const uint8_t *data, uint32_t length)
{
    uart_send(&uart3_ctx, data, length);
}

uint16_t USART1_Receive(uint8_t *data, uint16_t capacity)
{
    return uart_receive(&uart1_ctx, data, capacity);
}

uint16_t USART3_Receive(uint8_t *data, uint16_t capacity)
{
    return uart_receive(&uart3_ctx, data, capacity);
}

/**
 * @brief 两路 USART 共用的接收中断处理逻辑。
 * @note 每次保存一份 SR 状态，再只读一次 DR，避免 RXNE 与 IDLE 分支重复读 DR。
 */
static void uart_irq_handler(uart_ctx_t *uart)
{
    uint32_t status = uart->usart->SR;
    uint8_t byte;
    const uint32_t error_flags = USART_SR_FE | USART_SR_NE | USART_SR_PE;
    const uint32_t receive_flags = USART_SR_RXNE | USART_SR_IDLE |
                                   USART_SR_ORE | error_flags;

    if ((status & receive_flags) == 0)
    {
        return;
    }

    /* SR 已读取，随后读 DR 清除 RXNE、IDLE 以及接收错误状态。 */
    byte = (uint8_t)uart->usart->DR;

    if (status & USART_SR_ORE)
    {
        /* 硬件已丢失新数据，另行统计，不与软件缓存满混用。 */
        uart->rx_overrun_count++;
    }
    if (status & error_flags)
    {
        uart->rx_error_count++;
    }

    /* 无帧、噪声、校验错误才保存；仅有 ORE 时保留 DR 中原有字节。 */
    if ((status & USART_SR_RXNE) && !(status & error_flags))
    {
        uart_push_byte(uart, byte);
    }

    /* 先存字节再发布，因此 RXNE 和 IDLE 同时出现时也包含该字节。 */
    if (status & USART_SR_IDLE)
    {
        __DMB();
        uart->rx_ready_pos = uart->rx_write_pos;
    }
}

/** @brief USART1 中断入口，仅处理 USART1 的接收状态。 */
void USART1_IRQHandler(void)
{
    uart_irq_handler(&uart1_ctx);
}

/** @brief USART3 中断入口，仅处理 USART3 的接收状态。 */
void USART3_IRQHandler(void)
{
    uart_irq_handler(&uart3_ctx);
}
