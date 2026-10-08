/**
 * @file test_w25q128.c
 * @brief W25Q128 协议驱动的主机回归测试，不依赖 STM32 或 FreeRTOS。
 * @details 模拟真实的页内回卷、NOR 只能将 1 写成 0、WEL 与 BUSY；
 *          注入总线锁拒绝和 SPI 失败，检查错误是否传播以及资源是否释放。
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "spi.h"
#include "w25q128.h"

#define FLASH_BYTES 0x01000000UL
#define PAGE_BYTES 256U
#define SECTOR_BYTES 4096U

/** @brief 模拟存储体，初始值为擦除后的 0xFF。 */
static uint8_t flash_memory[FLASH_BYTES];

typedef struct
{
    uint8_t locked, selected, wel, deny_wel, deny_acquire;
    uint8_t stuck_busy, stuck_after_mutation, command, accepted;
    uint32_t address, count, payload_count, busy_polls;
    uint8_t pending_page[PAGE_BYTES];
    uint32_t acquire_calls, release_calls, select_calls, deselect_calls;
    uint32_t transfer_calls, failed_calls, calls_after_failure, delay_total;
    uint32_t commands[256], programs, erases;
    uint32_t fail_at_transfer, fail_program_payload;
} flash_model_t;

static flash_model_t model;

uint8_t SPI_Acquire(void)
{
    flash_model_t *m = &model;
    m->acquire_calls++;
    if (m->deny_acquire) return 0;
    assert(!m->locked);
    m->locked = 1;
    return 1;
}

void SPI_Release(void)
{
    flash_model_t *m = &model;
    assert(m->locked && !m->selected);
    m->release_calls++;
    m->locked = 0;
}

void SPI_Select(uint8_t active)
{
    flash_model_t *m = &model;
    assert(m->locked);
    if (active)
    {
        assert(!m->selected);
        m->select_calls++;
        m->selected = 1;
        m->count = 0;
        m->command = 0;
        m->address = 0;
        m->payload_count = 0;
        m->accepted = 0;
        memset(m->pending_page, 0xFF, sizeof(m->pending_page));
        return;
    }

    m->deselect_calls++;
    if (!m->selected) return;
    m->selected = 0;
    if (m->command == 0x06 && m->accepted) m->wel = 1;
    if (m->command == 0x02 && m->accepted && m->payload_count > 0)
    {
        uint32_t base = m->address & ~(PAGE_BYTES - 1U);
        uint32_t i;
        for (i = 0; i < PAGE_BYTES; i++)
            flash_memory[base + i] &= m->pending_page[i];
        m->programs++;
        m->wel = 0;
        m->busy_polls = 2;
        if (m->stuck_after_mutation) m->stuck_busy = 1;
    }
    if (m->command == 0x20 && m->accepted && m->count == 4)
    {
        uint32_t base = m->address & ~(SECTOR_BYTES - 1U);
        memset(flash_memory + base, 0xFF, SECTOR_BYTES);
        m->erases++;
        m->wel = 0;
        m->busy_polls = 2;
        if (m->stuck_after_mutation) m->stuck_busy = 1;
    }
}

uint8_t SPI_TransferByte(uint8_t tx, uint8_t *rx)
{
    flash_model_t *m = &model;
    uint32_t index;
    assert(m->locked && m->selected && rx != NULL);
    m->transfer_calls++;
    if (m->failed_calls) m->calls_after_failure++;
    if ((m->fail_at_transfer && m->transfer_calls == m->fail_at_transfer) ||
        (m->fail_program_payload && m->command == 0x02 && m->count >= 4 &&
         m->payload_count == m->fail_program_payload))
    {
        m->failed_calls++;
        return 0;
    }

    *rx = 0;
    index = m->count++;
    if (index == 0)
    {
        m->command = tx;
        m->commands[tx]++;
        if (tx == 0x06)
            m->accepted = !m->deny_wel && !m->stuck_busy && !m->busy_polls;
        else if (tx == 0x02 || tx == 0x20)
            m->accepted = m->wel && !m->stuck_busy && !m->busy_polls;
        return 1;
    }
    if (m->command == 0x05)
    {
        *rx = (uint8_t)((m->wel ? 2U : 0U) |
                       ((m->stuck_busy || m->busy_polls) ? 1U : 0U));
        if (!m->stuck_busy && m->busy_polls) m->busy_polls--;
    }
    else if (m->command == 0x9F)
    {
        *rx = index == 1 ? 0xEF : index == 2 ? 0x40 : 0x18;
    }
    else if (m->command == 0x02 || m->command == 0x03 || m->command == 0x20)
    {
        if (index <= 3)
            m->address = (m->address << 8) | tx;
        else if (m->command == 0x03)
            *rx = flash_memory[(m->address + m->payload_count++) & 0xFFFFFFUL];
        else if (m->command == 0x02 && m->accepted)
        {
            /* 页编程跨页时真实芯片会回卷，模型不能用线性写掩盖驱动错误。 */
            uint32_t offset = (m->address + m->payload_count++) & (PAGE_BYTES - 1U);
            m->pending_page[offset] &= tx;
        }
    }
    return 1;
}

void SPI_DelayMs(uint32_t ms)
{
    flash_model_t *m = &model;
    assert(m->locked && !m->selected);
    m->delay_total += ms;
}

uint8_t MySPI_Init(void) { return 1U; }

static void assert_released(void)
{
    assert(!model.locked && !model.selected);
    assert(model.release_calls == model.acquire_calls);
}

static void reset_flash(void)
{
    memset(flash_memory, 0xFF, sizeof(flash_memory));
    memset(&model, 0, sizeof(model));
    assert(W25Q128_Init() == 1);
    assert_released();
    /* 初始化是否探测器件不影响后面针对单次接口调用的计数检查。 */
    memset(&model, 0, sizeof(model));
}

static void test_model_page_wrap_and_nor(void)
{
    uint8_t rx;
    reset_flash();
    assert(SPI_Acquire());
    SPI_Select(1);
    assert(SPI_TransferByte(0x06, &rx));
    SPI_Select(0);
    SPI_Select(1);
    assert(SPI_TransferByte(0x02, &rx));
    assert(SPI_TransferByte(0, &rx));
    assert(SPI_TransferByte(0, &rx));
    assert(SPI_TransferByte(0xFF, &rx));
    assert(SPI_TransferByte(0x5A, &rx));
    assert(SPI_TransferByte(0xA6, &rx));
    SPI_Select(0);
    SPI_Release();
    assert(flash_memory[255] == 0x5A && flash_memory[0] == 0xA6);
    assert(flash_memory[256] == 0xFF);
    assert_released();
}

static void test_ids_and_cross_page(void)
{
    uint8_t data[600], readback[600];
    uint32_t jedec = 0, acquire_before, release_before, i;
    reset_flash();
    assert(W25Q128_ReadID() == 0x4018);
    assert(W25Q128_ReadJEDECID(&jedec) && jedec == 0xEF4018UL);
    assert(!W25Q128_ReadJEDECID(NULL));
    for (i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 37U + 11U);
    acquire_before = model.acquire_calls;
    release_before = model.release_calls;
    assert(W25Q128_Write(data, 250, sizeof(data)));
    assert(model.acquire_calls == acquire_before + 1);
    assert(model.release_calls == release_before + 1);
    assert(model.programs == 4 && model.erases == 0);
    assert(W25Q128_Read(readback, 250, sizeof(readback)));
    assert(memcmp(data, readback, sizeof(data)) == 0);
    assert(flash_memory[249] == 0xFF && flash_memory[850] == 0xFF);
    assert_released();
}

static void test_large_transfer_and_last_byte(void)
{
    static uint8_t data[70000], readback[70000];
    uint32_t i, acquire_before;
    uint8_t last = 0x69, byte = 0;
    reset_flash();
    for (i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i ^ (i >> 8));
    acquire_before = model.acquire_calls;
    assert(W25Q128_Write_NoErase(data, 0x40000, sizeof(data)));
    assert(model.acquire_calls == acquire_before + 1);
    assert(model.programs == 274 && model.erases == 0);
    assert(W25Q128_Read(readback, 0x40000, sizeof(readback)));
    assert(memcmp(data, readback, sizeof(data)) == 0);
    assert(W25Q128_Write(&last, FLASH_BYTES - 1, 1));
    assert(W25Q128_Read(&byte, FLASH_BYTES - 1, 1) && byte == last);
    assert_released();
}

static void test_invalid_arguments(void)
{
    uint8_t bytes[300] = {0};
    uint32_t transfers;
    reset_flash();
    transfers = model.transfer_calls;
    assert(!W25Q128_PageProgram(bytes, 255, 2));
    assert(!W25Q128_PageProgram(bytes, 0, 257));
    assert(!W25Q128_PageProgram(bytes, 0, 0));
    assert(!W25Q128_PageProgram(NULL, 0, 1));
    assert(!W25Q128_Write(NULL, 0, 1));
    assert(!W25Q128_Write(bytes, 0, 0));
    assert(!W25Q128_Write(bytes, FLASH_BYTES - 1, 2));
    assert(!W25Q128_Write(bytes, UINT32_MAX, 1));
    assert(!W25Q128_Write_NoErase(bytes, FLASH_BYTES, 1));
    assert(!W25Q128_Read(NULL, 0, 1));
    assert(!W25Q128_Read(bytes, 0, 0));
    assert(!W25Q128_Read(bytes, FLASH_BYTES - 1, 2));
    assert(!W25Q128_Read(bytes, UINT32_MAX, 2));
    assert(!W25Q128_Erase_Sector(1));
    assert(!W25Q128_Erase_Sector(FLASH_BYTES));
    assert(model.transfer_calls == transfers);
    assert(model.select_calls == 0 && model.erases == 0 && model.programs == 0);
    assert_released();
}

static void test_explicit_erase_and_nor(void)
{
    uint8_t zero = 0, ones = 0xFF, byte;
    reset_flash();
    assert(W25Q128_Write(&zero, 0x1000, 1));
    assert(W25Q128_Write(&ones, 0x1000, 1));
    assert(W25Q128_Read(&byte, 0x1000, 1) && byte == 0);
    assert(model.erases == 0);
    flash_memory[0x0FFF] = 0x12;
    flash_memory[0x2000] = 0x34;
    assert(W25Q128_Erase_Sector(0x1000));
    assert(flash_memory[0x1000] == 0xFF && flash_memory[0x1FFF] == 0xFF);
    assert(flash_memory[0x0FFF] == 0x12 && flash_memory[0x2000] == 0x34);
    assert(model.erases == 1);
    assert_released();
}

static void test_wel_rejection_and_busy_timeout(void)
{
    uint8_t byte = 0x5A;
    reset_flash();
    model.deny_wel = 1;
    assert(!W25Q128_Write_Enable());
    assert(!W25Q128_PageProgram(&byte, 1200, 1));
    assert(!W25Q128_Erase_Sector(0));
    assert(model.commands[0x02] == 0 && model.commands[0x20] == 0);
    assert(flash_memory[1200] == 0xFF);
    assert_released();

    reset_flash();
    model.stuck_busy = 1;
    assert(!W25Q128_Wait_Busy());
    assert(!W25Q128_PageProgram(&byte, 0, 1));
    assert(model.commands[0x02] == 0 && model.delay_total > 0);
    assert(model.transfer_calls < 1000000UL);
    assert_released();

    reset_flash();
    model.stuck_after_mutation = 1;
    assert(!W25Q128_PageProgram(&byte, 0, 1));
    assert(model.programs == 1 && flash_memory[0] == byte);
    assert_released();

    reset_flash();
    flash_memory[0] = 0;
    model.stuck_after_mutation = 1;
    assert(!W25Q128_Erase_Sector(0));
    assert(model.erases == 1 && flash_memory[0] == 0xFF);
    assert_released();
}

static void test_acquire_rejection(void)
{
    uint8_t byte = 0x5A;
    reset_flash();
    model.deny_acquire = 1;
    assert(!W25Q128_Read(&byte, 0, 1));
    assert(!W25Q128_Write(&byte, 0, 1));
    assert(!W25Q128_Erase_Sector(0));
    assert(model.acquire_calls == 3 && model.release_calls == 0);
    assert(model.transfer_calls == 0 && model.select_calls == 0);
    assert(!model.locked && !model.selected);
}

static void test_transfer_failure_cleanup(void)
{
    uint8_t data[600], readback[8];
    memset(data, 0x5A, sizeof(data));
    reset_flash();
    model.fail_at_transfer = 1;
    assert(!W25Q128_Read(readback, 0, sizeof(readback)));
    assert(model.failed_calls == 1 && model.calls_after_failure == 0);
    assert_released();

    reset_flash();
    model.fail_at_transfer = 1;
    assert(!W25Q128_Erase_Sector(0));
    assert(model.failed_calls == 1 && model.calls_after_failure == 0);
    assert(model.commands[0x20] == 0);
    assert_released();

    reset_flash();
    /* 第一页已送出两字节时失败，不能继续本页或开始后续页。 */
    model.fail_program_payload = 2;
    assert(!W25Q128_Write(data, 250, sizeof(data)));
    assert(model.failed_calls == 1 && model.calls_after_failure == 0);
    assert(model.commands[0x02] == 1 && model.erases == 0);
    assert(flash_memory[250] == 0x5A && flash_memory[251] == 0x5A);
    assert(flash_memory[252] == 0xFF && flash_memory[256] == 0xFF);
    assert(model.acquire_calls == 1 && model.release_calls == 1);
    assert_released();
}

int main(void)
{
    test_model_page_wrap_and_nor();
    test_ids_and_cross_page();
    test_large_transfer_and_last_byte();
    test_invalid_arguments();
    test_explicit_erase_and_nor();
    test_wel_rejection_and_busy_timeout();
    test_acquire_rejection();
    test_transfer_failure_cleanup();
    puts("W25Q128 hardware-SPI protocol regression tests PASS");
    return 0;
}
