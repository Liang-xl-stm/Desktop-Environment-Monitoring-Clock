/** Real SPI byte API: hardware completes while the calling task is preempted. */
#include "spi.h"
#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
GPIO_TypeDef test_gpio;
RCC_TypeDef test_rcc;
CoreDebug_Type test_debug;
uint32_t SystemCoreClock = 168000000U;
static SPI_TypeDef registers;
static DWT_Type counter;
static unsigned mode, injected, locked, spi_accesses, delayed_reads;
SPI_TypeDef *test_spi_registers(void)
{
    ++spi_accesses;
    if (mode == 6)
    {
        /* TXE is read first. The next SPI1 access takes the DR address, so
         * this switch occurs after the old common timer starts but before
         * the instruction that writes the outgoing byte. No byte is yet
         * in flight during the 2 ms switch. */
        if (spi_accesses == 2U && registers.DR == 0U)
        {
            injected = 1;
            counter.CYCCNT += 336000U;
        }
        if (registers.DR == 0xA5U && ++delayed_reads >= 3U)
        {
            /* A real peripheral does not complete immediately on DR write.
             * Two SR samples still observe TXE without RXNE. */
            registers.SR = SPI_SR_TXE | SPI_SR_RXNE;
            registers.DR = 0x5AU;
        }
        return &registers;
    }
    if (!injected && registers.DR == 0xA5U)
    {
        if (mode == 5) return &registers; /* Switch happens after the SR snapshot. */
        injected = 1;
        if (mode == 2) counter.CYCCNT += 336000U; /* Another task runs for 2ms. */
        if (mode != 3)
        {
            registers.SR = SPI_SR_TXE | SPI_SR_RXNE;
            registers.DR = 0x5AU;
        }
        if (mode == 4) registers.SR |= SPI_SR_OVR;
    }
    return &registers;
}
DWT_Type *test_counter(void)
{
    if (mode == 5 && !injected && registers.DR == 0xA5U)
    {
        injected = 1; counter.CYCCNT += 336000U;
        registers.SR = SPI_SR_TXE | SPI_SR_RXNE; registers.DR = 0x5AU;
    }
    if (counter.CTRL & 1U) counter.CYCCNT += 168U;
    return &counter;
}
int xTaskGetSchedulerState(void) { return taskSCHEDULER_RUNNING; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)1; }
void vTaskDelay(TickType_t ticks) { counter.CYCCNT += ticks * 168000U; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)2; }
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks)
{ (void)ticks; assert(mutex == (void *)2); if (locked) return 0; locked = 1; return 1; }
int xSemaphoreGive(SemaphoreHandle_t mutex)
{ assert(mutex == (void *)2 && locked); locked = 0; return 1; }
static uint8_t transfer(unsigned scenario, uint8_t *received)
{
    uint8_t success;
    mode = scenario; injected = 0; spi_accesses = 0; delayed_reads = 0;
    registers.DR = 0; registers.SR = SPI_SR_TXE;
    assert(SPI_Acquire()); SPI_Select(1);
    success = SPI_TransferByte(0xA5, received);
    SPI_Select(0); SPI_Release();
    assert(!locked);
    return success;
}
int main(void)
{
    uint8_t received = 0;
    const spi_diagnostic_t *diagnostic;
    spi_diagnostic_t saved_fault;
    assert(MySPI_Init());
    assert(transfer(1, &received) && received == 0x5A);
    received = 0;
    assert(transfer(2, &received) && received == 0x5A); /* Ready data survives preemption. */
    assert(transfer(5, &received) && received == 0x5A); /* Preemption after SR read. */
    assert(transfer(6, &received) && received == 0x5A); /* Preemption before DR write. */
    assert(injected && delayed_reads >= 3U); /* The delayed hardware path was exercised. */
    received = 0xEE;
    assert(!transfer(3, &received) && received == 0xEE); /* Stalled peripheral still times out. */
    diagnostic = SPI_GetLastDiagnostic();
    assert(diagnostic != NULL && diagnostic->reason == SPI_FAULT_TIMEOUT);
    assert(diagnostic->phase == 2U && diagnostic->elapsed_cycles >= 168000U);
    assert(diagnostic->sr == SPI_SR_TXE);
    assert((diagnostic->cr1 & SPI_CR1_SPE) != 0U);
    assert(diagnostic->counter_enabled == 1U);
    assert(!transfer(4, &received) && received == 0xEE); /* Ready flag cannot hide OVR. */
    diagnostic = SPI_GetLastDiagnostic();
    assert(diagnostic->reason == SPI_FAULT_HARDWARE && diagnostic->phase == 2U);
    assert((diagnostic->sr & SPI_SR_OVR) != 0U);
    assert(diagnostic->counter_enabled == 1U);
    saved_fault = *diagnostic;
    assert(transfer(1, &received) && received == 0x5A); /* Recovery releases CS and mutex. */
    assert(memcmp(&saved_fault, SPI_GetLastDiagnostic(), sizeof(saved_fault)) == 0);
    puts("PASS real SPI byte driver: task preemption, genuine stall, hardware error and recovery");
    return 0;
}
