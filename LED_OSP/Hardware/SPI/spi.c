/**
 * @file spi.c
 * @brief SPI1 硬件总线、软件片选及 FreeRTOS 事务锁的实现。
 * @details GPIO 只用于配置复用及控制 PB14 片选；SCK、MOSI、MISO 由 SPI1 外设收发。
 *          单字节传输的 TXE、RXNE、BSY 各自使用独立的 1 ms 周期计数预算。
 */
#include "spi.h"
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stddef.h>

/** @brief SPI1 的固定硬件参数：模式 0、8 位、全双工、八分频、软件 NSS。 */
#define SPI_CR1_VALUE (SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_BR_1)
/** @brief 当前同步传输无法自动恢复继续执行的硬件错误。 */
#define SPI_ERROR_FLAGS (SPI_SR_MODF | SPI_SR_OVR | SPI_SR_CRCERR)
/** @brief 启动调度器后的最大事务锁等待时间，单位毫秒。 */
#define SPI_LOCK_TIMEOUT_MS 1000U

/** @brief SPI 驱动内部状态；只有持有事务锁的线程才能操作片选和数据寄存器。 */
typedef struct
{
    SemaphoreHandle_t mutex; /**< 覆盖一整笔 Flash 操作的互斥锁。 */
    TaskHandle_t owner; /**< 当前所有者；创建任务前可能为 NULL。 */
    uint8_t acquired; /**< 是否已取得互斥锁，区分启动前的 NULL 所有者。 */
    uint8_t selected; /**< PB14 当前是否已经拉低。 */
    uint8_t initialized; /**< 初始化完成标志，确保重复调用不复位硬件。 */
} spi_ctx_t;

static spi_ctx_t spi_ctx;
static spi_diagnostic_t spi_diagnostic;

/**
 * @brief 将毫秒数换算为向上取整的调度节拍。
 * @param[in] ms 非零、至多 1000 的毫秒数。
 * @return 不小于 1 的节拍数。
 */
static TickType_t spi_ms_to_ticks(uint32_t ms)
{
    uint64_t ticks = ((uint64_t)ms * (uint64_t)configTICK_RATE_HZ + 999U) / 1000U;
    return (TickType_t)(ticks == 0U ? 1U : ticks);
}

/** @brief 判断共享 DWT 周期计数器是否仍然开启。 */
static uint8_t spi_counter_enabled(void)
{
    return (CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0U &&
           (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U;
}

/** @brief 在复位之前记录失败；正常传输不清除历史记录，底层不打印日志。 */
static void spi_record_fault(spi_fault_t reason, uint32_t phase,
                             uint32_t status, uint32_t elapsed)
{
    spi_diagnostic.reason = reason;
    spi_diagnostic.phase = phase;
    spi_diagnostic.sr = status;
    spi_diagnostic.cr1 = SPI1->CR1;
    spi_diagnostic.elapsed_cycles = elapsed;
    spi_diagnostic.counter_enabled = spi_counter_enabled();
}

const spi_diagnostic_t *SPI_GetLastDiagnostic(void)
{
    return &spi_diagnostic;
}

/**
 * @brief 开启周期计数并确认计数会递增，不重置其他组件使用的 CYCCNT。
 * @return 周期计数可用返回 1，不支持或未能开启返回 0。
 */
static uint8_t spi_counter_init(void)
{
    uint32_t start;
    uint32_t i;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0U || SystemCoreClock == 0U)
    {
        return 0U;
    }
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
    start = DWT->CYCCNT;
    for (i = 0U; i < 64U; ++i)
    {
        __NOP();
    }
    return spi_counter_enabled() && (uint32_t)(DWT->CYCCNT - start) != 0U;
}

/** @brief 取得当前内核时钟下向上取整的 1 ms 周期数。 */
static uint32_t spi_timeout_cycles(void)
{
    return SystemCoreClock / 1000U + (SystemCoreClock % 1000U != 0U ? 1U : 0U);
}

/**
 * @brief 复位 SPI1 并恢复固定模式，保持片选为释放状态。
 * @note 调用者必须独占总线；用于初始化及中止异常事务，不重新配置其他外设。
 */
static void spi_restore(void)
{
    GPIOB->BSRRL = GPIO_ODR_ODR_14;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;
    RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
    SPI1->I2SCFGR = 0U;
    SPI1->CR2 = 0U;
    SPI1->CR1 = SPI_CR1_VALUE | SPI_CR1_SPE;
    spi_ctx.selected = 0U;
}

/**
 * @brief 从当前等待阶段开始计时，允许 CYCCNT 自然回绕。
 * @param[in] flag 要等待的 SR 位。
 * @param[in] expected 非零要求置位，零要求清零。
 * @param[in] budget 当前等待阶段的最大周期数。
 * @param[in] phase 诊断阶段：1=TXE，2=RXNE，3=BSY。
 * @return 条件满足返回 1；超时、硬件错误或计数失效返回 0。
 */
static uint8_t spi_wait_flag(uint32_t flag, uint8_t expected,
                             uint32_t budget, uint32_t phase)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t status;
    uint32_t elapsed;
    for (;;)
    {
        status = SPI1->SR;
        if (!spi_counter_enabled())
        {
            spi_record_fault(SPI_FAULT_COUNTER, phase, status,
                             (uint32_t)(DWT->CYCCNT - start));
            return 0U;
        }
        if ((status & SPI_ERROR_FLAGS) != 0U)
        {
            spi_record_fault(SPI_FAULT_HARDWARE, phase, status,
                             (uint32_t)(DWT->CYCCNT - start));
            return 0U;
        }
        if (((status & flag) != 0U) == (expected != 0U))
        {
            return 1U;
        }
        /* DWT 也计入其他任务运行的时间。任务恢复时外设可能早已完成；
         * 先确认标志，只有仍未完成时才判超时，避免屏幕刷新导致误报。
         * 错误标志和计数器失效仍优先拒绝，不能被完成标志掩盖。 */
        elapsed = (uint32_t)(DWT->CYCCNT - start);
        if (elapsed >= budget)
        {
            /* 抢占还可能发生在读取 SR 和检查时间之间，截止时再读一次。 */
            status = SPI1->SR;
            if (!spi_counter_enabled())
                spi_record_fault(SPI_FAULT_COUNTER, phase, status, elapsed);
            else if ((status & SPI_ERROR_FLAGS) != 0U)
                spi_record_fault(SPI_FAULT_HARDWARE, phase, status, elapsed);
            else if (((status & flag) != 0U) == (expected != 0U))
                return 1U;
            else
                spi_record_fault(SPI_FAULT_TIMEOUT, phase, status, elapsed);
            return 0U;
        }
    }
}

/** @brief 检查当前线程是否持有此总线，拒绝中断及未初始化对象。 */
static uint8_t spi_is_owner(const spi_ctx_t *ctx)
{
    return __get_IPSR() == 0U && ctx->initialized && ctx->acquired &&
           ctx->owner == xTaskGetCurrentTaskHandle();
}

/**
 * @brief 取得整笔 Flash 操作的事务锁。
 * @return 成功返回 1；中断调用、重复取得、超时或未初始化返回 0。
 * @note 启动前、调度暂停或内核中断被屏蔽时只进行一次零等待尝试。
 */
uint8_t SPI_Acquire(void)
{
    spi_ctx_t *ctx = &spi_ctx;
    TickType_t wait = 0U;

    if (__get_IPSR() != 0U || !ctx->initialized || ctx->mutex == NULL ||
        spi_is_owner(ctx))
    {
        spi_record_fault(SPI_FAULT_CONTEXT, 0U, 0U, 0U);
        return 0U;
    }
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        __get_PRIMASK() == 0U && __get_BASEPRI() == 0U)
    {
        wait = spi_ms_to_ticks(SPI_LOCK_TIMEOUT_MS);
    }
    if (xSemaphoreTake(ctx->mutex, wait) != pdTRUE)
    {
        spi_record_fault(SPI_FAULT_LOCK, 0U, 0U, 0U);
        return 0U;
    }
    ctx->owner = xTaskGetCurrentTaskHandle();
    ctx->acquired = 1U;
    return 1U;
}

/**
 * @brief 释放事务锁，退出前始终释放片选。
 * @note 只有取得锁的调用者才能释放；错误线程或中断调用不会改变总线。
 */
void SPI_Release(void)
{
    spi_ctx_t *ctx = &spi_ctx;
    if (!spi_is_owner(ctx))
    {
        return;
    }
    GPIOB->BSRRL = GPIO_ODR_ODR_14;
    ctx->selected = 0U;
    ctx->owner = NULL;
    ctx->acquired = 0U;
    (void)xSemaphoreGive(ctx->mutex);
}

/**
 * @brief 控制 PB14 软件片选，SCK 和数据引脚仍由 SPI1 控制。
 * @param[in] active 非零拉低片选，零拉高片选。
 * @pre 当前线程已取得事务锁；一次事务须先选中，再收发，最后释放片选。
 */
void SPI_Select(uint8_t active)
{
    spi_ctx_t *ctx = &spi_ctx;
    if (!spi_is_owner(ctx))
    {
        return;
    }
    if (active != 0U)
    {
        GPIOB->BSRRH = GPIO_ODR_ODR_14;
        ctx->selected = 1U;
    }
    else
    {
        GPIOB->BSRRL = GPIO_ODR_ODR_14;
        ctx->selected = 0U;
    }
}

/**
 * @brief 通过硬件 SPI1 同步收发一字节，各等待阶段独立计时。
 * @param[in] tx 待发送字节。
 * @param[out] rx 成功时写入收到的字节；失败时保持原值。
 * @return 成功返回 1；非法上下文、超时或硬件错误返回 0。
 * @note 超时中止当前事务，释放 CS 并复位 SPI1；上层须终止本次 Flash 命令。
 */
uint8_t SPI_TransferByte(uint8_t tx, uint8_t *rx)
{
    spi_ctx_t *ctx = &spi_ctx;
    uint32_t budget;
    uint8_t received;

    if (!spi_is_owner(ctx) || !ctx->selected || rx == NULL)
    {
        spi_record_fault(SPI_FAULT_CONTEXT, 0U, 0U, 0U);
        return 0U;
    }
    budget = spi_timeout_cycles();
    if (!spi_counter_enabled() || budget == 0U)
    {
        spi_record_fault(SPI_FAULT_COUNTER, 0U, 0U, 0U);
        spi_restore();
        return 0U;
    }
    if (!spi_wait_flag(SPI_SR_TXE, 1U, budget, 1U))
    {
        spi_restore();
        return 0U;
    }
    *(__IO uint8_t *)&SPI1->DR = tx;
    /* 发送前可能被其他任务抢占；RXNE 从发送之后重新计时，
     * 不把发送前的调度耗时当作本次接收等待时间。 */
    if (!spi_wait_flag(SPI_SR_RXNE, 1U, budget, 2U))
    {
        spi_restore();
        return 0U;
    }
    received = *(__IO uint8_t *)&SPI1->DR;
    if (!spi_wait_flag(SPI_SR_BSY, 0U, budget, 3U))
    {
        spi_restore();
        return 0U;
    }
    *rx = received;
    return 1U;
}

/**
 * @brief 提供 Flash 忙状态轮询所需的毫秒间隔。
 * @param[in] ms 延时间隔，零表示立即返回。
 * @note 调度器运行且中断未屏蔽时用 vTaskDelay；其余线程上下文用 DWT 忙等。
 * @note 时间依赖稳定的 SystemCoreClock 和持续运行的 DWT，调试暂停不计入忙等时间。
 */
void SPI_DelayMs(uint32_t ms)
{
    spi_ctx_t *ctx = &spi_ctx;
    uint32_t chunk;
    uint32_t start;
    uint32_t budget;

    if (!spi_is_owner(ctx))
    {
        return;
    }
    while (ms != 0U)
    {
        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
            __get_PRIMASK() == 0U && __get_BASEPRI() == 0U)
        {
            /* 分段换算，避免大毫秒值溢出节拍类型。 */
            chunk = ms > 1000U ? 1000U : ms;
            vTaskDelay(spi_ms_to_ticks(chunk));
            ms -= chunk;
        }
        else
        {
            if (!spi_counter_enabled())
            {
                return;
            }
            /* 每次只等待 1 ms，以无符号减法处理 CYCCNT 回绕。 */
            start = DWT->CYCCNT;
            budget = spi_timeout_cycles();
            while ((uint32_t)(DWT->CYCCNT - start) < budget)
            {
                if (!spi_counter_enabled())
                {
                    return;
                }
            }
            --ms;
        }
    }
}

uint8_t MySPI_Init(void)
{
    if (__get_IPSR() != 0U)
    {
        return 0U;
    }
    if (spi_ctx.initialized)
    {
        return 1U;
    }
    if (!spi_counter_init())
    {
        return 0U;
    }
    spi_ctx.mutex = xSemaphoreCreateMutex();
    if (spi_ctx.mutex == NULL)
    {
        return 0U;
    }

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    (void)RCC->AHB1ENR;
    /* 先把片选锁存值置高，再切换 GPIO 模式，避免初始化时误选中芯片。 */
    GPIOB->BSRRL = GPIO_ODR_ODR_14;
    GPIOB->OTYPER &= ~((1U << 3) | (1U << 4) | (1U << 5) | (1U << 14));
    GPIOB->OSPEEDR = (GPIOB->OSPEEDR &
        ~((3U << 6) | (3U << 8) | (3U << 10) | (3U << 28))) |
        (2U << 6) | (2U << 8) | (2U << 10) | (2U << 28);
    GPIOB->PUPDR = (GPIOB->PUPDR &
        ~((3U << 6) | (3U << 8) | (3U << 10) | (3U << 28))) |
        (2U << 6) | (1U << 8) | (1U << 28);
    GPIOB->AFR[0] = (GPIOB->AFR[0] &
        ~((15U << 12) | (15U << 16) | (15U << 20))) |
        (5U << 12) | (5U << 16) | (5U << 20);
    spi_restore();
    GPIOB->MODER = (GPIOB->MODER &
        ~((3U << 6) | (3U << 8) | (3U << 10) | (3U << 28))) |
        (2U << 6) | (2U << 8) | (2U << 10) | (1U << 28);

    spi_ctx.initialized = 1U;
    return 1U;
}
