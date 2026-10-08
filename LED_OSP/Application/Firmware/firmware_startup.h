/** @file firmware_startup.h
 * @brief 注册当前加载映像，并在明确允许且管理区空白时首次建立基线。
 */
#ifndef FIRMWARE_STARTUP_H
#define FIRMWARE_STARTUP_H

#include <stdint.h>
#include "firmware_update.h"

/** @brief 启动准备的具体结果，不替代原接口的 0/1 返回值。 */
typedef enum
{
    FW_STARTUP_NOT_RUN = 0,      /**< 尚未执行启动准备。 */
    FW_STARTUP_READY_EXISTING,   /**< 已有 NORMAL 确认记录，未执行擦写。 */
    FW_STARTUP_READY_CREATED,    /**< 空白管理区首次创建基线成功。 */
    FW_STARTUP_INVALID_ARGUMENT, /**< 地址、容量或映像长度无效。 */
    FW_STARTUP_BIND_FAILED,      /**< App 区注册失败，例如存在活跃会话。 */
    FW_STARTUP_BOOT_PENDING,     /**< 已有待安装、安装、试运行或回滚记录。 */
    FW_STARTUP_CONFIRM_INVALID,  /**< 确认槽或管理字段不一致。 */
    FW_STARTUP_INIT_DISABLED,    /**< 没有有效记录且首次初始化开关关闭。 */
    FW_STARTUP_META_READ_FAILED, /**< 两份原始记录中至少一份读取失败。 */
    FW_STARTUP_META_NOT_BLANK,   /**< 无有效记录，但原始记录不是全 FF。 */
    FW_STARTUP_BASELINE_FAILED   /**< 空白区基线复制、校验或提交失败。 */
} firmware_startup_status_t;

/**
 * @brief 最近一次启动准备的只读诊断快照，不保存固件正文。
 * @note raw_read_ok 为 1 才能解读对应 raw；未读或读失败时不能把 raw 当记录。
 */
typedef struct
{
    firmware_startup_status_t status;
    uint32_t boot_phase;
    uint32_t confirmed_slot;
    uint32_t candidate_slot;
    uint8_t raw_read_ok[2];
    uint8_t raw[2][FIRMWARE_META_RECORD_SIZE];
} firmware_startup_diagnostic_t;

/**
 * @brief 获取上次启动准备的诊断快照。
 * @return 持续有效的只读对象地址，不能自行修改。
 * @note 启动准备与诊断读取应串行；getter 本身不访问或擦写 Flash。
 */
const firmware_startup_diagnostic_t *Firmware_StartupGetDiagnostic(void);

/** @brief 管理区恢复的具体结果；与启动准备的诊断分开保存。 */
typedef enum
{
    FW_CLEANUP_NOT_RUN = 0,
    FW_CLEANUP_READY,
    FW_CLEANUP_VALID_RECORD,
    FW_CLEANUP_META0_READ_FAILED,
    FW_CLEANUP_META1_READ_FAILED,
    FW_CLEANUP_META0_PROTECTED,
    FW_CLEANUP_META1_PROTECTED,
    FW_CLEANUP_PREFIX_MISMATCH,
    FW_CLEANUP_META1_ERASE_FAILED,
    FW_CLEANUP_META1_VERIFY_READ_FAILED,
    FW_CLEANUP_META1_VERIFY_DIRTY,
    FW_CLEANUP_META0_ERASE_FAILED,
    FW_CLEANUP_META0_VERIFY_READ_FAILED,
    FW_CLEANUP_META0_VERIFY_DIRTY
} firmware_cleanup_status_t;

/** @brief 没有相关异常地址时使用的标识，不是有效 Flash 地址。 */
#define FIRMWARE_CLEANUP_NO_ADDRESS 0xFFFFFFFFUL

/** @brief 本次恢复结果；地址均为外部 Flash 绝对字节地址。 */
typedef struct
{
    firmware_cleanup_status_t status; /**< 本次成功或具体拒绝/失败环节。 */
    uint32_t address;                 /**< 失败块起点或首个异常字节地址。 */
    uint32_t meta0_first_non_ff;       /**< 扫描 META0 时首个非 FF 的地址。 */
    uint32_t meta1_first_non_ff;       /**< 扫描 META1 时首个非 FF 的地址。 */
    uint8_t observed_byte;            /**< address 对应字节，仅 byte_valid=1 有效。 */
    uint8_t byte_valid;               /**< 地址对应字节确实读到，不以失败值代替。 */
    uint8_t erase_attempted_mask;     /**< 已调用擦除：bit0=META0，bit1=META1。 */
    uint8_t erase_completed_mask;     /**< 擦除且整区读回 FF：bit0=META0，bit1=META1。 */
    uint8_t sr1;                      /**< 完成或失败后单独读取的 Flash SR1。 */
    uint8_t sr1_valid;                /**< SR1 读取成功标识。 */
} firmware_cleanup_diagnostic_t;

/**
 * @brief 获取上次恢复的持续有效只读快照，getter 不读取 Flash。
 * @return 指向诊断对象的地址，不允许调用者修改。
 * @note 操作和读取诊断须串行；未发生的地址使用 FIRMWARE_CLEANUP_NO_ADDRESS。
 */
const firmware_cleanup_diagnostic_t *Firmware_StartupGetCleanupDiagnostic(void);

/**
 * @brief 清理本次已授权的旧管理区，使两份升级管理扇区可用于首次初始化。
 * @retval 1 两份管理扇区均已确认全 FF，可重新调用 Firmware_StartupPrepare。
 * @retval 0 有效/疑似升级记录、未知指纹，或读取、擦除、校验失败。
 * @pre 用户已授权删除旧管理区数据；Flash 已初始化且没有并发升级任务。
 * @note META0 非空白时须匹配本次旧数据指纹；META1 前60字节须全 FF。
 *       META1 尾部旧数据可清理，先擦并验证 META1，再处理 META0。
 *       保留有效管理记录、损坏 FWMD 和 A/B 正文；本函数不建立基线。
 *       具体拒绝原因、异常地址、擦除掩码及 SR1 通过诊断 getter 获取。
 */
uint8_t Firmware_StartupClearLegacyMetadata(void);

/**
 * @brief 在升级任务创建前，准备当前 App 与 A/B 服务的绑定。
 * @param[in] app_start 当前内部 Flash 加载映像起始地址，范围必须可读。
 * @param[in] app_capacity 实际内部 App 分区容量，非零且不超过单槽容量。
 * @param[in] app_size 当前完整加载映像长度；由链接符号取得，包含 RW 加载数据。
 * @param[in] allow_initialization 1 允许仅对完全空白的管理区首次建立 A 槽基线。
 * @retval 1 已绑定，并存在 NORMAL 管理记录，或首次建立基线成功。
 * @retval 0 参数、绑定、读取失败，记录非空白损坏，存在候选任务，或建基线失败。
 * @pre Flash 已初始化，不能与下载/其他升级服务调用并发。
 * @note 不会覆盖已有有效记录。遇到 PENDING/TRIAL 等阶段保留记录并拒绝下载。
 *       已有 NORMAL 记录的实际固件 CRC 在 Firmware_Begin 中进一步校验。
 *       任一读取失败不视为空白；缺少记录但存在任何非 FF 字节时不自动重建。
 */
uint8_t Firmware_StartupPrepare(const uint8_t *app_start,
                                uint32_t app_capacity,
                                uint32_t app_size,
                                uint8_t allow_initialization);

#endif
