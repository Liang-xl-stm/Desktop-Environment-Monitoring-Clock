/**
 * @file w25q128.c
 * @brief W25Q128 标准 SPI 指令及连续读写实现。
 * @details 公开操作仅取得一次总线锁，内部帮助函数沿用该锁。
 *          所有 SPI 传输都检查成功标志，失败后释放片选及总线锁。
 */

#include "w25q128.h"
#include "spi.h"
#include <stddef.h>

/** @brief SPI1 初始化成功标志；不向应用暴露设备对象。 */
static uint8_t flash_initialized;

/** @brief 检查非空缓冲、非零长度及完整范围，减法比较避免地址加法溢出。 */
static uint8_t valid_range(const void *buffer, uint32_t addr, uint32_t length)
{
    return buffer != NULL && length != 0U && addr < W25Q128_CAPACITY &&
           length <= W25Q128_CAPACITY - addr;
}

/** @brief 取得总线锁；未初始化或取得锁失败时返回 0。 */
static uint8_t acquire_flash(void)
{
    return flash_initialized && SPI_Acquire();
}

/** @brief 结束完整操作；先解除片选，再释放已经取得的总线锁。 */
static void release_flash(void)
{
    SPI_Select(0U);
    SPI_Release();
}

/** @brief 发送一字节并丢弃接收数据，保留底层成功或失败标志。 */
static uint8_t send_byte(uint8_t byte)
{
    uint8_t ignored;
    return SPI_TransferByte(byte, &ignored);
}

/** @brief 发送 24 位字节地址，遇到任意一字节失败立即停止。 */
static uint8_t send_address(uint32_t addr)
{
    return send_byte((uint8_t)(addr >> 16)) &&
           send_byte((uint8_t)(addr >> 8)) &&
           send_byte((uint8_t)addr);
}

/** @brief 在已持锁期间读取 SR1，本事务结束后始终解除片选。 */
static uint8_t read_status(uint8_t *status)
{
    uint8_t received;
    uint8_t result;

    SPI_Select(1U);
    result = send_byte(0x05U) && SPI_TransferByte(0U, &received);
    SPI_Select(0U);
    if (result)
    {
        *status = received;
    }
    return result;
}

/** @brief 在已持锁期间等待 BUSY 清零，最多执行 500 个毫秒延时间隔。 */
static uint8_t wait_busy(void)
{
    uint32_t poll;
    uint8_t status;

    for (poll = 0U; poll <= W25Q128_BUSY_POLL_LIMIT; poll++)
    {
        if (!read_status(&status))
        {
            return 0U;
        }
        if ((status & 0x01U) == 0U)
        {
            return 1U;
        }
        if (poll == W25Q128_BUSY_POLL_LIMIT)
        {
            return 0U;
        }
        SPI_DelayMs(1U);
    }
    return 0U;
}

/** @brief 在已持锁期间等待空闲、发送 WREN 并确认 WEL。 */
static uint8_t write_enable(void)
{
    uint8_t status;
    uint8_t result;

    if (!wait_busy())
    {
        return 0U;
    }
    SPI_Select(1U);
    result = send_byte(0x06U);
    SPI_Select(0U);
    if (!result || !read_status(&status))
    {
        return 0U;
    }
    return (status & 0x02U) != 0U;
}

/** @brief 已校验页范围且已持锁时执行一次页编程。 */
static uint8_t page_program(const uint8_t *buffer, uint32_t addr, uint16_t length)
{
    uint16_t index;
    uint8_t result;

    if (!write_enable())
    {
        return 0U;
    }
    SPI_Select(1U);
    result = send_byte(0x02U) && send_address(addr);
    for (index = 0U; result && index < length; index++)
    {
        result = send_byte(buffer[index]);
    }
    SPI_Select(0U);
    return result && wait_busy();
}

uint8_t W25Q128_Init(void)
{
    flash_initialized = MySPI_Init();
    return flash_initialized;
}

uint8_t W25Q128_ReadJEDECID(uint32_t *id)
{
    uint8_t bytes[3];
    uint8_t result;

    if (id == NULL || !acquire_flash())
    {
        return 0U;
    }
    result = wait_busy();
    if (result)
    {
        SPI_Select(1U);
        result = send_byte(0x9FU) &&
                 SPI_TransferByte(0U, &bytes[0]) &&
                 SPI_TransferByte(0U, &bytes[1]) &&
                 SPI_TransferByte(0U, &bytes[2]);
    }
    release_flash();
    if (result)
    {
        *id = ((uint32_t)bytes[0] << 16) |
              ((uint32_t)bytes[1] << 8) | bytes[2];
    }
    return result;
}

uint16_t W25Q128_ReadID(void)
{
    uint32_t id;
    return W25Q128_ReadJEDECID(&id) ? (uint16_t)id : 0xFFFFU;
}

uint8_t W25Q128_Read(uint8_t *pBuffer, uint32_t ReadAddr, uint32_t NumByteToRead)
{
    uint32_t index;
    uint8_t result;

    if (!valid_range(pBuffer, ReadAddr, NumByteToRead) || !acquire_flash())
    {
        return 0U;
    }
    result = wait_busy();
    if (result)
    {
        SPI_Select(1U);
        result = send_byte(0x03U) && send_address(ReadAddr);
        for (index = 0U; result && index < NumByteToRead; index++)
        {
            result = SPI_TransferByte(0U, &pBuffer[index]);
        }
    }
    release_flash();
    return result;
}

uint8_t W25Q128_Write_Enable(void)
{
    uint8_t result;
    if (!acquire_flash())
    {
        return 0U;
    }
    result = write_enable();
    release_flash();
    return result;
}

uint8_t W25Q128_PageProgram(const uint8_t *pBuffer, uint32_t WriteAddr,
                          uint16_t NumByteToWrite)
{
    uint8_t result;

    if (!valid_range(pBuffer, WriteAddr, NumByteToWrite) ||
        NumByteToWrite > W25Q128_PAGE_SIZE - (WriteAddr & 0xFFU) ||
        !acquire_flash())
    {
        return 0U;
    }
    result = page_program(pBuffer, WriteAddr, NumByteToWrite);
    release_flash();
    return result;
}

uint8_t W25Q128_Write_NoErase(const uint8_t *pBuffer, uint32_t WriteAddr,
                            uint32_t NumByteToWrite)
{
    uint32_t offset = 0U;
    uint32_t addr = WriteAddr;
    uint32_t remaining = NumByteToWrite;
    uint8_t result = 1U;

    if (!valid_range(pBuffer, WriteAddr, NumByteToWrite) || !acquire_flash())
    {
        return 0U;
    }
    while (remaining != 0U)
    {
        uint16_t page_left = (uint16_t)(W25Q128_PAGE_SIZE - (addr & 0xFFU));
        uint16_t length = remaining < page_left ? (uint16_t)remaining : page_left;

        if (!page_program(&pBuffer[offset], addr, length))
        {
            result = 0U;
            break;
        }
        offset += length;
        addr += length;
        remaining -= length;
    }
    release_flash();
    return result;
}

uint8_t W25Q128_Write(const uint8_t *pBuffer, uint32_t WriteAddr,
                    uint32_t NumByteToWrite)
{
    return W25Q128_Write_NoErase(pBuffer, WriteAddr, NumByteToWrite);
}

uint8_t W25Q128_Erase_Sector(uint32_t Dst_Addr)
{
    uint8_t result;

    if (Dst_Addr >= W25Q128_CAPACITY ||
        (Dst_Addr & (W25Q128_SECTOR_SIZE - 1U)) != 0U || !acquire_flash())
    {
        return 0U;
    }
    result = write_enable();
    if (result)
    {
        SPI_Select(1U);
        result = send_byte(0x20U) && send_address(Dst_Addr);
        SPI_Select(0U);
        if (result)
        {
            result = wait_busy();
        }
    }
    release_flash();
    return result;
}

uint8_t W25Q128_ReadStatus(uint8_t *status)
{
    uint8_t result;

    if (status == NULL || !acquire_flash())
    {
        return 0U;
    }
    result = read_status(status);
    release_flash();
    return result;
}

uint8_t W25Q128_ReadSR(void)
{
    uint8_t status;
    return W25Q128_ReadStatus(&status) ? status : 0xFFU;
}

uint8_t W25Q128_Wait_Busy(void)
{
    uint8_t result;

    if (!acquire_flash())
    {
        return 0U;
    }
    result = wait_busy();
    release_flash();
    return result;
}
