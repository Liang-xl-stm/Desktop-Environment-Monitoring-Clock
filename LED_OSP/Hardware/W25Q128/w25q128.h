/**
 * @file w25q128.h
 * @brief 硬件驱动 W25Q128 串行 Flash 协议接口。
 * @details 通过SPI1 直接接口访问 16 MiB Flash，不依赖 STM32 寄存器或操作系统。
 *          每个完整操作独占 SPI，总线收发失败时立即结束并释放片选和锁。
 * @note 写接口不自动擦除；目标位需要从 0 变为 1 时，必须先擦除所在扇区。
 *       返回成功表示通信及等待完成，不代表已读回校验，也不保证受保护区域被修改。
 */
#ifndef HARDWARE_W25Q128_H
#define HARDWARE_W25Q128_H

#include <stdint.h>

/** @brief Flash 总容量，128 Mbit = 16 MiB。 */
#define W25Q128_CAPACITY 0x01000000UL
/** @brief 一次页编程的页大小，单位为字节。 */
#define W25Q128_PAGE_SIZE 256U
/** @brief 最小擦除扇区的大小，单位为字节。 */
#define W25Q128_SECTOR_SIZE 4096U
/** @brief 忙状态最多等待 500 个 1 ms 间隔；实际时间还包含通信及调度开销。 */
#define W25Q128_BUSY_POLL_LIMIT 500U

/**
 * @brief 初始化固定连接的 SPI1，不擦除或写入 Flash。
 * @retval 1 总线准备完成；@retval 0 硬件计数器或互斥创建失败。
 * @note 无参数，在创建 Flash 使用任务前调用即可。
 */
uint8_t W25Q128_Init(void);

/**
 * @brief 读取完整 24 位 JEDEC ID，包括厂商、器件类型及容量编号。
 * @param[out] id 成功时写入 ID，例如 EF4018；失败时保持原值。
 * @retval 1 成功。
 * @retval 0 参数无效、总线锁失败、收发失败或芯片忙超时。
 */
uint8_t W25Q128_ReadJEDECID(uint32_t *id);

/** @brief 兼容接口：返回 JEDEC ID 后 16 位，失败返回 0xFFFF。 */
uint16_t W25Q128_ReadID(void);

/**
 * @brief 连续读取 Flash 数据，允许跨页及跨扇区。
 * @param[out] pBuffer 容量至少为 NumByteToRead 的接收缓冲。
 * @param[in] ReadAddr 起始字节地址，范围为 0x000000 至 0xFFFFFF。
 * @param[in] NumByteToRead 非零读取长度；整个范围必须位于 Flash 容量内。
 * @retval 1 成功。
 * @retval 0 参数无效、锁失败、收发失败或等待超时。
 * @note 失败前可能已填入部分数据，调用者不能将部分读取视为完整成功。
 */
uint8_t W25Q128_Read(uint8_t *pBuffer, uint32_t ReadAddr, uint32_t NumByteToRead);

/**
 * @brief 写使能并确认状态寄存器 WEL 位置位。
 * @retval 1 通信成功且 WEL 为 1。
 * @retval 0 锁失败、收发失败、忙超时或 WEL 未置位。
 * @note 页编程及擦除内部会重新执行写使能；不需由调用者提前调用。
 */
uint8_t W25Q128_Write_Enable(void);

/**
 * @brief 在一个 256 字节页内编程，不自动擦除。
 * @param[in] pBuffer 长度至少为 NumByteToWrite 的源数据。
 * @param[in] WriteAddr 起始字节地址。
 * @param[in] NumByteToWrite 非零长度，必须小于等于当前页的剩余空间。
 * @retval 1 通信成功并且内部编程等待结束。
 * @retval 0 参数无效、锁失败、收发失败、写使能失败或等待超时。
 */
uint8_t W25Q128_PageProgram(const uint8_t *pBuffer, uint32_t WriteAddr,
                          uint16_t NumByteToWrite);

/**
 * @brief 按页分段连续编程，允许跨页及跨扇区，不自动擦除。
 * @param[in] pBuffer 长度至少为 NumByteToWrite 的源数据。
 * @param[in] WriteAddr 起始字节地址。
 * @param[in] NumByteToWrite 非零写入长度；整个范围必须位于 Flash 容量内。
 * @retval 1 所有分段通信及内部编程等待完成。
 * @retval 0 参数无效、锁失败、收发失败、写使能失败或等待超时。
 * @note 错误发生前可能已编程部分数据；本接口不提供回滚。
 */
uint8_t W25Q128_Write_NoErase(const uint8_t *pBuffer, uint32_t WriteAddr,
                            uint32_t NumByteToWrite);

/** @brief 兼容写接口，行为与 W25Q128_Write_NoErase 完全一致，不自动擦除。 */
uint8_t W25Q128_Write(const uint8_t *pBuffer, uint32_t WriteAddr,
                    uint32_t NumByteToWrite);

/**
 * @brief 擦除一个 4 KiB 扇区。
 * @param[in] Dst_Addr 4 KiB 对齐的有效字节地址，不能传入扇区编号。
 * @retval 1 通信成功并且内部擦除等待结束。
 * @retval 0 地址非法、未对齐、锁失败、收发失败、写使能失败或等待超时。
 * @note 擦除涉及整个扇区，成功后应按需要读回确认其内容为 0xFF。
 */
uint8_t W25Q128_Erase_Sector(uint32_t Dst_Addr);

/**
 * @brief 读取状态寄存器 1，独立报告通信是否成功。
 * @param[out] status 成功时写入 SR1；bit0 为 BUSY，bit1 为 WEL。
 * @retval 1 成功。
 * @retval 0 参数无效、锁失败或收发失败；此时保持输出原值。
 */
uint8_t W25Q128_ReadStatus(uint8_t *status);

/**
 * @brief 兼容状态读取接口，失败返回 0xFF。
 * @note 0xFF 也可能是实际接收值；需要区分通信失败时使用 W25Q128_ReadStatus。
 */
uint8_t W25Q128_ReadSR(void);

/**
 * @brief 有限等待内部写入或擦除完成。
 * @retval 1 已读到 BUSY 为 0。
 * @retval 0 锁失败、状态读取失败或等待超时。
 * @note 超时后芯片可能仍在操作，不能据此认为已经停止擦除或写入。
 */
uint8_t W25Q128_Wait_Busy(void);

#endif /* HARDWARE_W25Q128_H */
