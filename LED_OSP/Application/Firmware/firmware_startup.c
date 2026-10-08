/** @file firmware_startup.c
 * @brief 首次空白初始化和已有管理记录保护。
 */
#include "firmware_startup.h"
#include "firmware_update.h"
#include "w25q128.h"
#include <stddef.h>
#include <string.h>

/** @brief 最近一次准备结果；诊断读取不访问 Flash，也不改变管理记录。 */
static firmware_startup_diagnostic_t startup_diagnostic;

static firmware_cleanup_diagnostic_t cleanup_diagnostic =
{
    FW_CLEANUP_NOT_RUN, FIRMWARE_CLEANUP_NO_ADDRESS,
    FIRMWARE_CLEANUP_NO_ADDRESS, FIRMWARE_CLEANUP_NO_ADDRESS,
    0U, 0U, 0U, 0U, 0U, 0U
};

/**
 * @brief 本次已确认旧数据的前 16 字节指纹，只授权清理这一份历史内容。
 * @note 不匹配的未知内容不视为可删除数据，不能泛化为任意非 FWMD 数据清理。
 */
static const uint8_t legacy_prefix[16] =
{
    0x2AU, 0x04U, 0x45U, 0xC5U, 0x6CU, 0x48U, 0x4CU, 0x88U,
    0xD2U, 0xD7U, 0xB4U, 0x10U, 0x7DU, 0x0AU, 0x84U, 0x38U
};

/** @brief 一个完整 4KB 管理扇区的扫描结果，读失败时保留已经观察到的信息。 */
typedef struct
{
    uint32_t first_non_ff;       /**< 首个非 FF 地址，未发现时为 NO_ADDRESS。 */
    uint32_t read_failed_address;/**< 读取失败分块的起点，不代表字节内容。 */
    uint32_t prefix_mismatch;    /**< 与授权指纹首个不同字节的地址。 */
    uint8_t blank;              /**< 完整 4KB 全 FF，不只检查记录前 60 字节。 */
    uint8_t record_blank;       /**< 管理记录前 60 字节全 FF。 */
    uint8_t fwmd_prefix;        /**< 首字为 FWMD，即使 CRC/提交损坏也保留。 */
    uint8_t authorized_prefix;  /**< 前 16 字节精确匹配本次授权的旧数据。 */
    uint8_t first_non_ff_byte;  /**< first_non_ff 对应的实际读取值。 */
    uint8_t prefix_mismatch_byte;/**< prefix_mismatch 对应的实际读取值。 */
    uint8_t first_byte;         /**< 首块读取成功后才有效的扇区起始字节。 */
} firmware_sector_scan_t;

/**
 * @brief 用 128 字节缓冲完整扫描一个管理扇区，不进行擦写。
 * @param[in] address META0 或 META1 起始地址，不允许其他地址。
 * @param[out] result 扫描结果；读失败时也保存失败块起点与已读信息。
 * @retval 1 全部 4KB 读取成功，空白和异常字节位置已确定。
 * @retval 0 参数非法或读取失败，不能把未读内容当空白。
 * @note 发现非 FF 后仍读完整扇区；无结果的地址为 NO_ADDRESS。
 */
static uint8_t firmware_scan_management_sector(uint32_t address,
                                                firmware_sector_scan_t *result)
{
    uint8_t buffer[128];
    firmware_sector_scan_t scan;
    uint32_t offset, part, i, first_word;

    if (result == NULL ||
        (address != FIRMWARE_META0_ADDR && address != FIRMWARE_META1_ADDR))
    {
        return 0U;
    }
    memset(&scan, 0, sizeof(scan));
    scan.blank = 1U;
    scan.record_blank = 1U;
    scan.first_non_ff = FIRMWARE_CLEANUP_NO_ADDRESS;
    scan.read_failed_address = FIRMWARE_CLEANUP_NO_ADDRESS;
    scan.prefix_mismatch = FIRMWARE_CLEANUP_NO_ADDRESS;
    for (offset = 0U; offset < FIRMWARE_META_SECTOR_SIZE; offset += part)
    {
        part = FIRMWARE_META_SECTOR_SIZE - offset;
        if (part > sizeof(buffer))
        {
            part = sizeof(buffer);
        }
        if (!W25Q128_Read(buffer, address + offset, part))
        {
            scan.read_failed_address = address + offset;
            *result = scan;
            return 0U;
        }
        if (offset == 0U)
        {
            /* 按小端解析，不依赖缓冲对齐或结构体强制转换。 */
            first_word = (uint32_t)buffer[0] |
                         ((uint32_t)buffer[1] << 8) |
                         ((uint32_t)buffer[2] << 16) |
                         ((uint32_t)buffer[3] << 24);
            scan.first_byte = buffer[0];
            scan.fwmd_prefix = (uint8_t)(first_word == FIRMWARE_META_MAGIC);
            scan.authorized_prefix = 1U;
            for (i = 0U; i < sizeof(legacy_prefix); ++i)
            {
                if (buffer[i] != legacy_prefix[i])
                {
                    scan.authorized_prefix = 0U;
                    scan.prefix_mismatch = address + i;
                    scan.prefix_mismatch_byte = buffer[i];
                    break;
                }
            }
        }
        for (i = 0U; i < part; ++i)
        {
            if (buffer[i] != 0xFFU)
            {
                scan.blank = 0U;
                if (offset + i < FIRMWARE_META_RECORD_SIZE)
                {
                    scan.record_blank = 0U;
                }
                if (scan.first_non_ff == FIRMWARE_CLEANUP_NO_ADDRESS)
                {
                    scan.first_non_ff = address + offset + i;
                    scan.first_non_ff_byte = buffer[i];
                }
            }
        }
    }
    *result = scan;
    return 1U;
}

/**
 * @brief 获取上次清理的只读诊断对象，不操作 Flash。
 * @return 生命周期覆盖整个程序的只读地址，未执行时状态为 NOT_RUN。
 * @note 与清理操作串行调用，调用者不得修改返回对象。
 */
const firmware_cleanup_diagnostic_t *Firmware_StartupGetCleanupDiagnostic(void)
{
    return &cleanup_diagnostic;
}

/**
 * @brief 擦除通过保护检查的管理扇区，并完整读回验证全 FF。
 * @param[in] address 调用方仅传入 META0 或 META1 起始地址。
 * @retval 1 擦除成功且完整 4KB 读回全 FF，完成位已置位。
 * @retval 0 擦除或验证失败，具体状态、地址、字节写入清理诊断。
 * @note 尝试位在擦除前置位；完成位只能在整区验证成功后置位。
 *       调用方已经完成新鲜扫描和指纹保护，不允许扩大擦除范围。
 */
static uint8_t firmware_cleanup_erase_verified(uint32_t address)
{
    firmware_sector_scan_t verified;
    uint8_t is_meta1 = (uint8_t)(address == FIRMWARE_META1_ADDR);
    uint8_t mask = is_meta1 ? 2U : 1U;

    cleanup_diagnostic.erase_attempted_mask |= mask;
    if (!W25Q128_Erase_Sector(address))
    {
        cleanup_diagnostic.status = is_meta1 ? FW_CLEANUP_META1_ERASE_FAILED :
                                              FW_CLEANUP_META0_ERASE_FAILED;
        cleanup_diagnostic.address = address;
        return 0U;
    }
    if (!firmware_scan_management_sector(address, &verified))
    {
        cleanup_diagnostic.status = is_meta1 ? FW_CLEANUP_META1_VERIFY_READ_FAILED :
                                              FW_CLEANUP_META0_VERIFY_READ_FAILED;
        cleanup_diagnostic.address = verified.read_failed_address;
        return 0U;
    }
    if (!verified.blank)
    {
        cleanup_diagnostic.status = is_meta1 ? FW_CLEANUP_META1_VERIFY_DIRTY :
                                              FW_CLEANUP_META0_VERIFY_DIRTY;
        cleanup_diagnostic.address = verified.first_non_ff;
        cleanup_diagnostic.observed_byte = verified.first_non_ff_byte;
        cleanup_diagnostic.byte_valid = 1U;
        return 0U;
    }
    cleanup_diagnostic.erase_completed_mask |= mask;
    return 1U;
}

/**
 * @brief 在清理成功或失败后统一采样一次 SR1，返回原清理结果。
 * @retval 1 原清理状态为 READY，两份管理区已经确认空白。
 * @retval 0 原清理状态为拒绝或失败，SR1 失败不会替换原状态。
 * @note 无参数；前序 Flash 操作已经释放 SPI 总线，采样独立加锁。
 */
static uint8_t firmware_cleanup_finalize(void)
{
    cleanup_diagnostic.sr1_valid =
        (uint8_t)(W25Q128_ReadStatus(&cleanup_diagnostic.sr1) != 0U);
    return (uint8_t)(cleanup_diagnostic.status == FW_CLEANUP_READY);
}

const firmware_startup_diagnostic_t *Firmware_StartupGetDiagnostic(void)
{
    return &startup_diagnostic;
}

/**
 * @brief 只读检查两份管理记录是否均完全未编程。
 * @retval 1 两次读取都成功，且每份 60 字节全部为 FF。
 * @retval 0 任一读取失败或存在任何非 FF 字节。
 * @note 不把 CRC 错误或提交标记损坏视为空白，不执行擦除。
 */
static uint8_t firmware_management_is_blank(void)
{
    uint32_t copy;
    uint32_t i;
    uint8_t read_failed = 0U;
    uint8_t not_blank = 0U;

    for (copy = 0U; copy < 2U; ++copy)
    {
        uint32_t address = copy == 0U ? FIRMWARE_META0_ADDR : FIRMWARE_META1_ADDR;
        if (!W25Q128_Read(startup_diagnostic.raw[copy], address,
                         FIRMWARE_META_RECORD_SIZE))
        {
            read_failed = 1U;
            continue;
        }
        startup_diagnostic.raw_read_ok[copy] = 1U;
        for (i = 0U; i < FIRMWARE_META_RECORD_SIZE; ++i)
        {
            if (startup_diagnostic.raw[copy][i] != 0xFFU)
            {
                not_blank = 1U;
            }
        }
    }
    if (read_failed)
    {
        startup_diagnostic.status = FW_STARTUP_META_READ_FAILED;
        return 0U;
    }
    if (not_blank)
    {
        startup_diagnostic.status = FW_STARTUP_META_NOT_BLANK;
        return 0U;
    }
    return 1U;
}

uint8_t Firmware_StartupPrepare(const uint8_t *app_start,
                                uint32_t app_capacity,
                                uint32_t app_size,
                                uint8_t allow_initialization)
{
    firmware_metadata_t metadata;

    memset(&startup_diagnostic, 0, sizeof(startup_diagnostic));
    startup_diagnostic.confirmed_slot = FIRMWARE_SLOT_NONE;
    startup_diagnostic.candidate_slot = FIRMWARE_SLOT_NONE;

    if (app_start == NULL || app_capacity == 0U ||
        app_capacity > FIRMWARE_SLOT_SIZE || app_size == 0U ||
        app_size > app_capacity)
    {
        startup_diagnostic.status = FW_STARTUP_INVALID_ARGUMENT;
        return 0U;
    }
    if (!Firmware_SetAppRegion(app_start, app_capacity))
    {
        startup_diagnostic.status = FW_STARTUP_BIND_FAILED;
        return 0U;
    }

    if (Firmware_ReadMetadata(&metadata))
    {
        startup_diagnostic.boot_phase = metadata.boot_phase;
        startup_diagnostic.confirmed_slot = metadata.confirmed_slot;
        startup_diagnostic.candidate_slot = metadata.candidate_slot;
        /* 已有有效记录只读检查，不重新建立、更改确认槽或删除候选。 */
        if (metadata.boot_phase != FIRMWARE_BOOT_NORMAL)
        {
            startup_diagnostic.status = FW_STARTUP_BOOT_PENDING;
            return 0U;
        }
        if (metadata.candidate_slot != FIRMWARE_SLOT_NONE ||
            metadata.confirmed_slot >= FIRMWARE_SLOT_COUNT)
        {
            startup_diagnostic.status = FW_STARTUP_CONFIRM_INVALID;
            return 0U;
        }
        if (metadata.slot[metadata.confirmed_slot].valid != FIRMWARE_IMAGE_VALID ||
            metadata.slot[metadata.confirmed_slot].firmware_size == 0U ||
            metadata.slot[metadata.confirmed_slot].firmware_size > app_capacity)
        {
            startup_diagnostic.status = FW_STARTUP_CONFIRM_INVALID;
            return 0U;
        }
        startup_diagnostic.status = FW_STARTUP_READY_EXISTING;
        return 1U;
    }

    /* ReadMetadata 失败既可能没有记录，也可能 I/O 失败，不能直接开始擦写。 */
    if (!allow_initialization)
    {
        startup_diagnostic.status = FW_STARTUP_INIT_DISABLED;
        return 0U;
    }
    if (!firmware_management_is_blank())
    {
        return 0U;
    }

    /* 调用方显式允许首次初始化，且管理区经完整读取确认空白。
     * CreateBaseline 内部会再次扫描并拒绝已有有效记录或读取失败。
     */
    if (!Firmware_CreateBaseline(app_size))
    {
        startup_diagnostic.status = FW_STARTUP_BASELINE_FAILED;
        return 0U;
    }
    startup_diagnostic.status = FW_STARTUP_READY_CREATED;
    return 1U;
}

/**
 * @brief 清理本次指纹匹配的旧管理区，先 META1 尾部、再 META0。
 * @retval 1 两份管理区完整 4KB 均已确认全 FF，可重新执行启动准备。
 * @retval 0 有效/疑似升级记录、未知指纹，或读取、擦除、整区验证失败。
 * @pre 用户已授权本次旧数据删除，Flash 已初始化，无并发升级操作。
 * @note 无参数；不写管理记录、不注册 App、不建立基线、不擦 A/B 正文。
 *       META1 前 60 字节须全 FF；有授权 META0 指纹才允许清其尾部。
 *       保留 META0 指纹直到 META1 整区验证成功，便于失败后安全重试。
 *       启动准备的诊断不变，各失败分支通过独立清理诊断报告。
 */
uint8_t Firmware_StartupClearLegacyMetadata(void)
{
    firmware_metadata_t metadata;
    firmware_sector_scan_t meta0;
    firmware_sector_scan_t meta1;

    /* 每次重新观察 Flash，不将上一次启动原始快照当成实时状态。 */
    memset(&cleanup_diagnostic, 0, sizeof(cleanup_diagnostic));
    cleanup_diagnostic.address = FIRMWARE_CLEANUP_NO_ADDRESS;
    cleanup_diagnostic.meta0_first_non_ff = FIRMWARE_CLEANUP_NO_ADDRESS;
    cleanup_diagnostic.meta1_first_non_ff = FIRMWARE_CLEANUP_NO_ADDRESS;

    /* 任意有效记录都保留，包括待安装、试运行、回滚等阶段。 */
    if (Firmware_ReadMetadata(&metadata))
    {
        cleanup_diagnostic.status = FW_CLEANUP_VALID_RECORD;
        goto complete;
    }
    if (!firmware_scan_management_sector(FIRMWARE_META0_ADDR, &meta0))
    {
        cleanup_diagnostic.status = FW_CLEANUP_META0_READ_FAILED;
        cleanup_diagnostic.address = meta0.read_failed_address;
        cleanup_diagnostic.meta0_first_non_ff = meta0.first_non_ff;
        goto complete;
    }
    cleanup_diagnostic.meta0_first_non_ff = meta0.first_non_ff;
    if (!firmware_scan_management_sector(FIRMWARE_META1_ADDR, &meta1))
    {
        cleanup_diagnostic.status = FW_CLEANUP_META1_READ_FAILED;
        cleanup_diagnostic.address = meta1.read_failed_address;
        cleanup_diagnostic.meta1_first_non_ff = meta1.first_non_ff;
        goto complete;
    }
    cleanup_diagnostic.meta1_first_non_ff = meta1.first_non_ff;

    /* FWMD 即使 CRC/提交损坏也保留，持续启用开关不能绕过保护。 */
    if (meta0.fwmd_prefix)
    {
        cleanup_diagnostic.status = FW_CLEANUP_META0_PROTECTED;
        cleanup_diagnostic.address = FIRMWARE_META0_ADDR;
        cleanup_diagnostic.observed_byte = meta0.first_byte;
        cleanup_diagnostic.byte_valid = 1U;
        goto complete;
    }
    /* META1 记录区任意非 FF 都拒绝；仅尾部旧数据属于本次授权范围。 */
    if (!meta1.record_blank)
    {
        cleanup_diagnostic.status = FW_CLEANUP_META1_PROTECTED;
        cleanup_diagnostic.address = meta1.first_non_ff;
        cleanup_diagnostic.observed_byte = meta1.first_non_ff_byte;
        cleanup_diagnostic.byte_valid = 1U;
        goto complete;
    }
    if (meta0.blank && meta1.blank)
    {
        cleanup_diagnostic.status = FW_CLEANUP_READY;
        goto complete;
    }
    /* META0 空白而 META1 尾脏也拒绝：没有本次授权指纹，不扩大范围。 */
    if (!meta0.authorized_prefix)
    {
        cleanup_diagnostic.status = FW_CLEANUP_PREFIX_MISMATCH;
        cleanup_diagnostic.address = meta0.prefix_mismatch;
        cleanup_diagnostic.observed_byte = meta0.prefix_mismatch_byte;
        cleanup_diagnostic.byte_valid = 1U;
        goto complete;
    }

    /* 先处理 META1；失败时 META0 指纹仍在，不先删除重试的授权依据。 */
    if (!meta1.blank && !firmware_cleanup_erase_verified(FIRMWARE_META1_ADDR))
    {
        goto complete;
    }
    if (!firmware_cleanup_erase_verified(FIRMWARE_META0_ADDR))
    {
        goto complete;
    }
    cleanup_diagnostic.status = FW_CLEANUP_READY;

complete:
    /* 每条退出路径仅采样一次 SR1；采样失败不掩盖真正的清理结果。 */
    return firmware_cleanup_finalize();
}
