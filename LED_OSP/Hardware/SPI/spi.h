/** @file spi.h @brief SPI1：PB3 SCK、PB4 MISO、PB5 MOSI，PB14 片选。 */
#ifndef HARDWARE_SPI_H
#define HARDWARE_SPI_H
#include <stdint.h>

/** @brief 最近一次总线失败的原因；数值用于串口故障日志。 */
typedef enum
{
    SPI_FAULT_NONE = 0,       /**< 尚未记录失败。 */
    SPI_FAULT_CONTEXT,        /**< 未初始化、错误任务、中断或参数错误。 */
    SPI_FAULT_LOCK,           /**< 未能取得事务锁。 */
    SPI_FAULT_COUNTER,        /**< 周期计数器不可用。 */
    SPI_FAULT_TIMEOUT,        /**< 等待硬件标志超时。 */
    SPI_FAULT_HARDWARE        /**< SR 出现 MODF、OVR 或 CRCERR。 */
} spi_fault_t;

/** @brief 失败时、复位 SPI1 之前保存的状态。 */
typedef struct
{
    spi_fault_t reason;       /**< 上面的失败原因。 */
    uint32_t phase;           /**< 0=调用或锁，1=TXE，2=RXNE，3=BSY。 */
    uint32_t sr;              /**< 失败时的 SPI 状态寄存器；阶段 0 不采样，填 0。 */
    uint32_t cr1;             /**< 复位前的 SPI 控制寄存器。 */
    uint32_t elapsed_cycles;  /**< 本阶段已等待的内核周期数；阶段 0 为 0。 */
    uint32_t counter_enabled; /**< 当时 DWT 周期计数是否开启，1=开启。 */
} spi_diagnostic_t;

/**
 * @brief 查看最近一次总线失败，不进行 SPI 通信。
 * @return 驱动内部快照地址，始终有效；调用者不能修改或释放。
 * @note 成功操作不清除快照，后续失败会覆盖。多任务中仅用于诊断，
 *       应在失败后立即复制；不是某个 Flash 操作独占的错误记录。
 */
const spi_diagnostic_t *SPI_GetLastDiagnostic(void);

/** @return 1 初始化成功，0 周期计数器或互斥资源不可用。重复初始化不复位总线。 */
uint8_t MySPI_Init(void);
/** @return 1 取得事务锁，0 未初始化、超时、ISR 调用或重复取得。
 * @note W25Q128 驱动内部使用，应用直接调用 W25Q128_Read/Write 即可。
 */
uint8_t SPI_Acquire(void);
/** 释放总线锁和片选；必须由取得锁的同一任务调用。 */
void SPI_Release(void);
/** @param active 1 拉低 PB14 选中 Flash，0 释放片选；调用前须取得锁。 */
void SPI_Select(uint8_t active);
/** @param tx 待发字节；@param rx 成功时保存收到的字节。
 * @return 1 成功，0 参数错误或硬件超时；失败中止事务，应用不得继续发送该命令。
 * @note TXE、RXNE、BSY 各自独立计时，等待未完成标志的预算分别为 1 ms。
 */
uint8_t SPI_TransferByte(uint8_t tx, uint8_t *rx);
/** @param ms 毫秒间隔；持锁期间使用，调度器运行时让出 CPU。 */
void SPI_DelayMs(uint32_t ms);
#endif
