/**
 * @file firmware_update.c
 * @brief A/B 外部 Flash 固件升级服务的配套实现。
 * @details 使用项目的 W25Q128_Read/Write/Erase_Sector 接口；纯 C99，
 *          不固定 STM32 App 地址、不访问 RTOS 或中断寄存器。
 * @note 与配套新头文件一起使用；由一个任务独占所有升级接口和分区。
 *       CRC 只检验数据完整性，不检验来源真实性。
 *       本文件不提供 Bootloader 内部 Flash 安装、跳转、看门狗回滚代码。
 */

#include "firmware_update.h"
#include "w25q128.h"
#include <stddef.h>
#include <string.h>

/** @brief 小块读回/复制大小，避免分配整个固件大小的 RAM。 */
#define FIRMWARE_IO_CHUNK 256U

/** @brief 反射形式 CRC-32/ISO-HDLC 多项式。 */
#define FIRMWARE_CRC_POLY 0xEDB88320UL

/** @brief 已注册的当前运行 App 映像首地址，其容量范围必须持续可读。 */
static const uint8_t *registered_app = NULL;
/** @brief 内部 App 分区容量，也是本服务允许的固件长度上限之一。 */
static uint32_t registered_capacity = 0U;
/** @brief 尚未结束的下载会话，只允许一个对象活动。 */
static firmware_update_t *active_update = NULL;

/** @brief 一次管理记录扫描的结果，不把读取失败误当成未初始化。 */
typedef struct
{
    firmware_metadata_t record; /**< 找到的较新有效记录。 */
    uint8_t has_valid;          /**< 1 表示找到有效记录，0 表示两份都无效。 */
    uint8_t copy_index;         /**< record 所在副本：0 或 1。 */
} metadata_snapshot_t;

/**
 * @brief 将一个 32 位整数按小端写入 4 字节缓冲。
 * @param[out] dst 至少 4 字节的目标缓冲。
 * @param[in] value 要编码的整数。
 * @return 无。
 */
static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

/**
 * @brief 从 4 字节小端缓冲解析 32 位整数。
 * @param[in] src 至少 4 字节的源缓冲，不要求地址对齐。
 * @return 解析后的整数。
 */
static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

/**
 * @brief 更新 CRC 中间状态，不执行最终异或。
 * @param[in] crc 上一批中间状态，首批传入 0xFFFFFFFF。
 * @param[in] data 至少 length 字节有效数据。
 * @param[in] length 本批字节数，可为 0。
 * @return 下一批可继续使用的 CRC 中间状态。
 */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data,
                             uint32_t length)
{
    uint32_t i;
    uint32_t bit;

    for (i = 0U; i < length; ++i)
    {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1U) ? FIRMWARE_CRC_POLY : 0U);
        }
    }
    return crc;
}

/**
 * @brief 计算一段可读内存的完整 CRC-32/ISO-HDLC。
 * @param[in] data 长度至少为 length 的可读映像。
 * @param[in] length 映像字节数。
 * @return 包含初值和最终异或的 CRC32，零也可能是合法结果。
 */
static uint32_t crc32_memory(const uint8_t *data, uint32_t length)
{
    return crc32_update(0xFFFFFFFFUL, data, length) ^ 0xFFFFFFFFUL;
}

/**
 * @brief 用小缓冲读取外部 Flash 并计算完整 CRC。
 * @param[in] address 外部 Flash 的起始字节地址。
 * @param[in] length 连续数据的非零长度。
 * @param[out] result 全部读取成功后写入 CRC，失败时保持原值。
 * @return 1 表示完整读取成功，0 表示范围或 Flash 读取错误。
 */
static uint8_t crc32_flash(uint32_t address, uint32_t length,
                           uint32_t *result)
{
    uint8_t buffer[FIRMWARE_IO_CHUNK];
    uint32_t done = 0U;
    uint32_t part;
    uint32_t crc = 0xFFFFFFFFUL;

    if (result == NULL || length == 0U || address >= W25Q128_CAPACITY ||
        length > W25Q128_CAPACITY - address)
    {
        return 0U;
    }
    while (done < length)
    {
        part = length - done;
        if (part > FIRMWARE_IO_CHUNK)
        {
            part = FIRMWARE_IO_CHUNK;
        }
        if (!W25Q128_Read(buffer, address + done, part))
        {
            return 0U;
        }
        crc = crc32_update(crc, buffer, part);
        done += part;
    }
    *result = crc ^ 0xFFFFFFFFUL;
    return 1U;
}

/**
 * @brief 将 A/B 槽编号转换成正文起始地址。
 * @param[in] slot 仅允许 A 或 B，不允许 NONE。
 * @param[out] address 成功时写入对应地址。
 * @return 1 转换成功，0 编号或输出参数无效。
 */
static uint8_t slot_address(uint32_t slot, uint32_t *address)
{
    if (address == NULL || slot >= FIRMWARE_SLOT_COUNT)
    {
        return 0U;
    }
    *address = (slot == FIRMWARE_SLOT_A) ? FIRMWARE_SLOT_A_ADDR
                                          : FIRMWARE_SLOT_B_ADDR;
    return 1U;
}

/**
 * @brief 对管理记录进行确定的 60 字节小端序列化。
 * @param[in] metadata 待编码的记录，不能为 NULL。
 * @param[out] raw 至少 60 字节的目标缓冲。
 * @return 无；本函数只编码，不计算或检查 CRC。
 */
static void metadata_encode(const firmware_metadata_t *metadata,
                             uint8_t *raw)
{
    uint32_t i;
    uint32_t offset;

    put_le32(raw + 0U, metadata->magic);
    put_le32(raw + 4U, metadata->format_version);
    put_le32(raw + 8U, metadata->generation);
    for (i = 0U; i < FIRMWARE_SLOT_COUNT; ++i)
    {
        offset = 12U + i * 12U;
        put_le32(raw + offset, metadata->slot[i].firmware_size);
        put_le32(raw + offset + 4U, metadata->slot[i].firmware_crc32);
        put_le32(raw + offset + 8U, metadata->slot[i].valid);
    }
    put_le32(raw + 36U, metadata->confirmed_slot);
    put_le32(raw + 40U, metadata->candidate_slot);
    put_le32(raw + 44U, metadata->boot_phase);
    put_le32(raw + 48U, metadata->trial_boot_count);
    put_le32(raw + 52U, metadata->record_crc32);
    put_le32(raw + 56U, metadata->commit);
}

/**
 * @brief 将 60 字节小端记录逐字段解码到 RAM 对象。
 * @param[in] raw 至少 60 字节源数据。
 * @param[out] metadata 解码结果，不依赖结构体内存布局。
 * @return 无；有效性由后续检查函数判断。
 */
static void metadata_decode(const uint8_t *raw,
                             firmware_metadata_t *metadata)
{
    uint32_t i;
    uint32_t offset;

    metadata->magic = get_le32(raw + 0U);
    metadata->format_version = get_le32(raw + 4U);
    metadata->generation = get_le32(raw + 8U);
    for (i = 0U; i < FIRMWARE_SLOT_COUNT; ++i)
    {
        offset = 12U + i * 12U;
        metadata->slot[i].firmware_size = get_le32(raw + offset);
        metadata->slot[i].firmware_crc32 = get_le32(raw + offset + 4U);
        metadata->slot[i].valid = get_le32(raw + offset + 8U);
    }
    metadata->confirmed_slot = get_le32(raw + 36U);
    metadata->candidate_slot = get_le32(raw + 40U);
    metadata->boot_phase = get_le32(raw + 44U);
    metadata->trial_boot_count = get_le32(raw + 48U);
    metadata->record_crc32 = get_le32(raw + 52U);
    metadata->commit = get_le32(raw + 56U);
}

/**
 * @brief 检查管理记录字段之间的关系，防止使用 CRC 正确的非法记录。
 * @param[in] metadata 已解码的记录，不检查其 record_crc32。
 * @return 1 字段合法，0 标识、长度、编号或启动阶段不合法。
 */
static uint8_t metadata_fields_valid(const firmware_metadata_t *metadata)
{
    uint32_t i;
    uint32_t candidate;

    if (metadata->magic != FIRMWARE_META_MAGIC ||
        metadata->format_version != FIRMWARE_META_VERSION ||
        metadata->commit != FIRMWARE_META_COMMIT ||
        metadata->confirmed_slot >= FIRMWARE_SLOT_COUNT ||
        metadata->boot_phase > FIRMWARE_BOOT_ROLLBACK)
    {
        return 0U;
    }
    for (i = 0U; i < FIRMWARE_SLOT_COUNT; ++i)
    {
        if (metadata->slot[i].valid == FIRMWARE_IMAGE_VALID)
        {
            if (metadata->slot[i].firmware_size == 0U ||
                metadata->slot[i].firmware_size > FIRMWARE_SLOT_SIZE)
            {
                return 0U;
            }
        }
        else if (metadata->slot[i].valid != FIRMWARE_IMAGE_INVALID ||
                 metadata->slot[i].firmware_size != 0U ||
                 metadata->slot[i].firmware_crc32 != 0U)
        {
            return 0U;
        }
    }
    if (metadata->slot[metadata->confirmed_slot].valid != FIRMWARE_IMAGE_VALID)
    {
        return 0U;
    }
    candidate = metadata->candidate_slot;
    if (metadata->boot_phase == FIRMWARE_BOOT_NORMAL ||
        metadata->boot_phase == FIRMWARE_BOOT_ROLLBACK)
    {
        return (uint8_t)(candidate == FIRMWARE_SLOT_NONE);
    }
    /* PENDING、INSTALLING、TRIAL 均需要一个与 confirmed 不同的有效候选。 */
    return (uint8_t)(candidate < FIRMWARE_SLOT_COUNT &&
                     candidate != metadata->confirmed_slot &&
                     metadata->slot[candidate].valid == FIRMWARE_IMAGE_VALID);
}

/**
 * @brief 检查原始管理记录的提交标记、CRC 和字段合法性。
 * @param[in] raw 完整 60 字节记录。
 * @param[out] metadata 检查通过后写入解码结果；失败时保持原值。
 * @return 1 记录有效，0 未提交、CRC 错误或格式非法。
 */
static uint8_t metadata_record_valid(const uint8_t *raw,
                                      firmware_metadata_t *metadata)
{
    firmware_metadata_t decoded;

    if (get_le32(raw + 56U) != FIRMWARE_META_COMMIT ||
        get_le32(raw + 52U) != crc32_memory(raw, FIRMWARE_META_CRC_SIZE))
    {
        return 0U;
    }
    metadata_decode(raw, &decoded);
    if (!metadata_fields_valid(&decoded))
    {
        return 0U;
    }
    *metadata = decoded;
    return 1U;
}

/**
 * @brief 读取两份管理记录并选择较新有效副本。
 * @param[out] snapshot 成功扫描后写入结果，has_valid 可为 0。
 * @return 1 两次读取成功且序号没有歧义；0 读取失败或序号关系有歧义。
 * @note has_valid=0 才表示未建立记录；返回 0 不能被当成空白 Flash。
 *       模 2^32 比较可处理正常递增回绕；相差 2^31 或同序号不同记录则拒绝。
 */
static uint8_t metadata_load(metadata_snapshot_t *snapshot)
{
    uint8_t raw0[FIRMWARE_META_RECORD_SIZE];
    uint8_t raw1[FIRMWARE_META_RECORD_SIZE];
    firmware_metadata_t record0;
    firmware_metadata_t record1;
    uint8_t valid0;
    uint8_t valid1;
    uint32_t distance;
    metadata_snapshot_t result;

    if (!W25Q128_Read(raw0, FIRMWARE_META0_ADDR, FIRMWARE_META_RECORD_SIZE) ||
        !W25Q128_Read(raw1, FIRMWARE_META1_ADDR, FIRMWARE_META_RECORD_SIZE))
    {
        return 0U;
    }
    memset(&result, 0, sizeof(result));
    valid0 = metadata_record_valid(raw0, &record0);
    valid1 = metadata_record_valid(raw1, &record1);
    if (!valid0 && !valid1)
    {
        *snapshot = result;
        return 1U;
    }
    result.has_valid = 1U;
    if (valid0 && valid1)
    {
        distance = record0.generation - record1.generation;
        if (distance == 0U)
        {
            if (memcmp(raw0, raw1, FIRMWARE_META_RECORD_SIZE) != 0)
            {
                return 0U;
            }
            result.copy_index = 0U;
        }
        else if (distance == 0x80000000UL)
        {
            return 0U;
        }
        else
        {
            result.copy_index = (uint8_t)(distance < 0x80000000UL ? 0U : 1U);
        }
    }
    else
    {
        result.copy_index = valid0 ? 0U : 1U;
    }
    result.record = result.copy_index == 0U ? record0 : record1;
    *snapshot = result;
    return 1U;
}

/**
 * @brief 将新管理状态事务提交到另一副本，保留原有效副本。
 * @param[in,out] metadata 要提交的状态；成功后返回新序号、CRC、提交标记。
 * @param[in] require_existing 1 要求已有有效记录；0 仅用于首次无记录的基线。
 * @param[in] previous_generation require_existing=1 时必须匹配的旧序号。
 * @return 1 擦除、分步写入和最终读回验证均成功；0 任一步失败。
 * @note 任一副本读取失败都拒绝写入。提交标记最后单独写，不自动重试失败写入。
 *       返回 0 时新记录仍有可能已经提交，后续必须重新读取判断持久化状态。
 */
static uint8_t metadata_store(firmware_metadata_t *metadata,
                               uint8_t require_existing,
                               uint32_t previous_generation)
{
    metadata_snapshot_t snapshot;
    firmware_metadata_t next = *metadata;
    firmware_metadata_t checked;
    uint8_t raw[FIRMWARE_META_RECORD_SIZE];
    uint8_t verify[FIRMWARE_META_RECORD_SIZE];
    uint32_t address;
    uint32_t i;

    if (!metadata_load(&snapshot) ||
        snapshot.has_valid != require_existing ||
        (require_existing && snapshot.record.generation != previous_generation))
    {
        return 0U;
    }
    next.magic = FIRMWARE_META_MAGIC;
    next.format_version = FIRMWARE_META_VERSION;
    next.generation = require_existing ? snapshot.record.generation + 1U : 1U;
    next.commit = FIRMWARE_META_COMMIT;
    if (!metadata_fields_valid(&next))
    {
        return 0U;
    }
    metadata_encode(&next, raw);
    next.record_crc32 = crc32_memory(raw, FIRMWARE_META_CRC_SIZE);
    metadata_encode(&next, raw);
    address = (!require_existing || snapshot.copy_index == 1U)
                  ? FIRMWARE_META0_ADDR : FIRMWARE_META1_ADDR;

    /* 始终擦除非最新副本，并确认记录范围确实被擦成 FF。 */
    if (!W25Q128_Erase_Sector(address) ||
        !W25Q128_Read(verify, address, FIRMWARE_META_RECORD_SIZE))
    {
        return 0U;
    }
    for (i = 0U; i < FIRMWARE_META_RECORD_SIZE; ++i)
    {
        if (verify[i] != 0xFFU)
        {
            return 0U;
        }
    }
    /* 前 56 字节包括记录 CRC，但不包括提交标记。 */
    if (!W25Q128_Write(raw, address, 56U) ||
        !W25Q128_Read(verify, address, 56U) ||
        memcmp(raw, verify, 56U) != 0)
    {
        return 0U;
    }
    /* 内容读回一致之后才写最后 4 字节 commit。 */
    if (!W25Q128_Write(raw + 56U, address + 56U, 4U) ||
        !W25Q128_Read(verify, address, FIRMWARE_META_RECORD_SIZE) ||
        memcmp(raw, verify, FIRMWARE_META_RECORD_SIZE) != 0 ||
        !metadata_record_valid(verify, &checked))
    {
        return 0U;
    }
    *metadata = checked;
    return 1U;
}

/**
 * @brief 将正文长度向上取整到扇区后擦除，地址必须为固件槽起点。
 * @param[in] address A 或 B 槽正文起点。
 * @param[in] length 非零正文长度，不超过单槽容量。
 * @return 1 全部扇区擦除完成；0 参数错误或擦除失败。
 */
static uint8_t erase_image(uint32_t address, uint32_t length)
{
    uint32_t rounded;
    uint32_t offset;

    if ((address != FIRMWARE_SLOT_A_ADDR && address != FIRMWARE_SLOT_B_ADDR) ||
        length == 0U || length > FIRMWARE_SLOT_SIZE)
    {
        return 0U;
    }
    /* 长度已限制到 1MiB，向上取整加法不会溢出。 */
    rounded = ((length + W25Q128_SECTOR_SIZE - 1U) /
               W25Q128_SECTOR_SIZE) * W25Q128_SECTOR_SIZE;
    for (offset = 0U; offset < rounded; offset += W25Q128_SECTOR_SIZE)
    {
        if (!W25Q128_Erase_Sector(address + offset))
        {
            return 0U;
        }
    }
    return 1U;
}

/**
 * @brief 检查当前运行 App 与指定的有效槽正文都匹配描述中的 CRC。
 * @param[in] metadata 格式已验证的管理记录。
 * @param[in] slot 要检查的 A/B 编号。
 * @return 1 长度、外部正文和当前内部 App 均一致；0 任一步不匹配或读取失败。
 * @note CRC 相同证明内容完整性，不验证固件来源或运行功能。
 */
static uint8_t running_image_matches(const firmware_metadata_t *metadata,
                                      uint32_t slot)
{
    uint32_t address;
    uint32_t external_crc;
    uint32_t length;

    if (registered_app == NULL || slot >= FIRMWARE_SLOT_COUNT ||
        metadata->slot[slot].valid != FIRMWARE_IMAGE_VALID ||
        !slot_address(slot, &address))
    {
        return 0U;
    }
    length = metadata->slot[slot].firmware_size;
    if (length == 0U || length > registered_capacity ||
        !crc32_flash(address, length, &external_crc) ||
        external_crc != metadata->slot[slot].firmware_crc32)
    {
        return 0U;
    }
    return (uint8_t)(crc32_memory(registered_app, length) == external_crc);
}

/**
 * @brief 验证会话仍属于当前 NORMAL 管理记录，拒绝过期或被篡改的 RAM 字段。
 * @param[in] update 活跃会话对象。
 * @param[out] snapshot 成功时写入最新管理记录，供 Finish 使用。
 * @return 1 地址、长度、序号、目标无效状态均一致；0 参数或读取检查失败。
 */
static uint8_t session_matches_metadata(const firmware_update_t *update,
                                         metadata_snapshot_t *snapshot)
{
    uint32_t address;

    if (update != active_update || registered_app == NULL ||
        !slot_address((uint32_t)update->target_slot, &address) ||
        address != update->storage_addr || update->total_size == 0U ||
        update->total_size > registered_capacity ||
        update->total_size > FIRMWARE_SLOT_SIZE ||
        update->received_size > update->total_size ||
        !metadata_load(snapshot) || !snapshot->has_valid)
    {
        return 0U;
    }
    return (uint8_t)(snapshot->record.generation == update->metadata_generation &&
                     snapshot->record.boot_phase == FIRMWARE_BOOT_NORMAL &&
                     snapshot->record.candidate_slot == FIRMWARE_SLOT_NONE &&
                     snapshot->record.confirmed_slot != (uint32_t)update->target_slot &&
                     snapshot->record.slot[update->target_slot].valid ==
                         FIRMWARE_IMAGE_INVALID);
}

/**
 * @brief 注册当前实际运行 App 的可读映像首地址和内部 App 分区容量。
 * @param[in] app_start 真正的内部 App .bin 起点，整个容量范围都必须可读。
 * @param[in] app_capacity 内部 App 分区字节容量，非零且不超过单槽。
 * @return 1 注册成功；0 参数非法或存在活跃下载，会保持原绑定。
 */
uint8_t Firmware_SetAppRegion(const uint8_t *app_start,
                              uint32_t app_capacity)
{
    if (app_start == NULL || app_capacity == 0U ||
        app_capacity > FIRMWARE_SLOT_SIZE || active_update != NULL)
    {
        return 0U;
    }
    registered_app = app_start;
    registered_capacity = app_capacity;
    return 1U;
}

/**
 * @brief 只读获取较新的完整管理记录。
 * @param[out] metadata 成功后写入结果，失败保持原值。
 * @return 1 找到有效记录；0 读取失败、没有记录或参数非法。
 */
uint8_t Firmware_ReadMetadata(firmware_metadata_t *metadata)
{
    metadata_snapshot_t snapshot;

    if (metadata == NULL || !metadata_load(&snapshot) || !snapshot.has_valid)
    {
        return 0U;
    }
    *metadata = snapshot.record;
    return 1U;
}

/**
 * @brief 首次明确初始化，将当前 App 保存到 A 作为回滚基线。
 * @param[in] firmware_size 当前运行 App 的准确 .bin 长度，不能传容量或指针大小。
 * @return 1 复制、校验及 NORMAL 记录提交成功；0 已有记录或任一步失败。
 * @note 只有两份管理信息均读取成功且无有效记录才允许擦除 A。
 */
uint8_t Firmware_CreateBaseline(uint32_t firmware_size)
{
    metadata_snapshot_t snapshot;
    firmware_metadata_t metadata;
    uint8_t buffer[FIRMWARE_IO_CHUNK];
    uint32_t source_crc;
    uint32_t copied_crc;
    uint32_t done = 0U;
    uint32_t part;

    if (registered_app == NULL || active_update != NULL ||
        firmware_size == 0U || firmware_size > registered_capacity ||
        firmware_size > FIRMWARE_SLOT_SIZE ||
        !metadata_load(&snapshot) || snapshot.has_valid)
    {
        return 0U;
    }
    source_crc = crc32_memory(registered_app, firmware_size);
    if (!erase_image(FIRMWARE_SLOT_A_ADDR, firmware_size))
    {
        return 0U;
    }
    while (done < firmware_size)
    {
        part = firmware_size - done;
        if (part > FIRMWARE_IO_CHUNK)
        {
            part = FIRMWARE_IO_CHUNK;
        }
        memcpy(buffer, registered_app + done, part);
        if (!W25Q128_Write(buffer, FIRMWARE_SLOT_A_ADDR + done, part))
        {
            return 0U;
        }
        done += part;
    }
    if (!crc32_flash(FIRMWARE_SLOT_A_ADDR, firmware_size, &copied_crc) ||
        copied_crc != source_crc)
    {
        return 0U;
    }
    /* 基线写入和校验后，最后建立第一份完整的管理记录。 */
    memset(&metadata, 0, sizeof(metadata));
    metadata.slot[FIRMWARE_SLOT_A].firmware_size = firmware_size;
    metadata.slot[FIRMWARE_SLOT_A].firmware_crc32 = source_crc;
    metadata.slot[FIRMWARE_SLOT_A].valid = FIRMWARE_IMAGE_VALID;
    metadata.confirmed_slot = FIRMWARE_SLOT_A;
    metadata.candidate_slot = FIRMWARE_SLOT_NONE;
    metadata.boot_phase = FIRMWARE_BOOT_NORMAL;
    return metadata_store(&metadata, 0U, 0U);
}

/**
 * @brief 自动选择未确认槽，提交其无效状态，再准备新固件正文区域。
 * @param[in,out] update 初始化过且持续有效的下载对象。
 * @param[in] firmware_size 新固件正文长度，不含包头且不能超过内部 App 容量。
 * @param[in] firmware_crc32 新固件正文的预期 CRC32，零也是允许的值。
 * @return 1 准备完成；0 参数/阶段错误或 Flash 失败，处理失败时 state=ERROR。
 */
uint8_t Firmware_Begin(firmware_update_t *update,
                       uint32_t firmware_size,
                       uint32_t firmware_crc32)
{
    metadata_snapshot_t snapshot;
    firmware_metadata_t metadata;
    uint32_t target;
    uint32_t address;

    if (update == NULL || registered_app == NULL || firmware_size == 0U ||
        firmware_size > registered_capacity || firmware_size > FIRMWARE_SLOT_SIZE ||
        (active_update != NULL && active_update != update) ||
        update->state == FIRMWARE_PREPARING ||
        update->state == FIRMWARE_RECEIVING || update->state == FIRMWARE_VERIFYING)
    {
        return 0U;
    }
    if (!metadata_load(&snapshot) || !snapshot.has_valid ||
        snapshot.record.boot_phase != FIRMWARE_BOOT_NORMAL ||
        snapshot.record.candidate_slot != FIRMWARE_SLOT_NONE ||
        !running_image_matches(&snapshot.record, snapshot.record.confirmed_slot))
    {
        return 0U;
    }
    target = snapshot.record.confirmed_slot == FIRMWARE_SLOT_A
                 ? FIRMWARE_SLOT_B : FIRMWARE_SLOT_A;
    if (!slot_address(target, &address))
    {
        return 0U;
    }

    active_update = update;
    update->state = FIRMWARE_PREPARING;
    update->target_slot = (firmware_slot_t)target;
    update->storage_addr = address;
    update->total_size = firmware_size;
    update->received_size = 0U;
    update->expected_crc32 = firmware_crc32;
    update->metadata_generation = snapshot.record.generation;

    metadata = snapshot.record;
    /* 先持久化使目标无效，断电后不能把正在改写的槽当成有效候选。 */
    memset(&metadata.slot[target], 0, sizeof(metadata.slot[target]));
    metadata.candidate_slot = FIRMWARE_SLOT_NONE;
    metadata.trial_boot_count = 0U;
    if (!metadata_store(&metadata, 1U, snapshot.record.generation))
    {
        update->state = FIRMWARE_ERROR;
        return 0U;
    }
    update->metadata_generation = metadata.generation;
    if (!erase_image(address, firmware_size))
    {
        update->state = FIRMWARE_ERROR;
        return 0U;
    }
    update->state = FIRMWARE_RECEIVING;
    return 1U;
}

/**
 * @brief 向目标槽顺序追加本批固件正文。
 * @param[in,out] update 当前活跃会话，成功后增加已写入长度。
 * @param[in] data 本批正文，返回前至少 length 字节保持有效。
 * @param[in] length 实际有效字节数，非零且不能超过剩余长度。
 * @return 1 本批完成；0 错误。整批成功前不更新进度，写失败可能已部分改变 Flash。
 */
uint8_t Firmware_Write(firmware_update_t *update,
                       const uint8_t *data, uint32_t length)
{
    metadata_snapshot_t snapshot;

    if (update == NULL || data == NULL || length == 0U ||
        update != active_update || update->state != FIRMWARE_RECEIVING)
    {
        return 0U;
    }
    /* 先检查 total/received，再做减法，避免下溢或相加溢出。 */
    if (!session_matches_metadata(update, &snapshot) ||
        length > update->total_size - update->received_size)
    {
        update->state = FIRMWARE_ERROR;
        return 0U;
    }
    if (!W25Q128_Write(data, update->storage_addr + update->received_size, length))
    {
        update->state = FIRMWARE_ERROR;
        return 0U;
    }
    update->received_size += length;
    return 1U;
}

/**
 * @brief 正文收齐后读回校验并提交 PENDING 候选，保留已确认槽。
 * @param[in,out] update 当前活跃会话，成功后为 READY。
 * @return 1 候选已经完整提交；0 长度、CRC、状态或 Flash 操作失败。
 * @note 未收齐时返回 0，保持 RECEIVING，允许继续接收；其余处理失败进入 ERROR。
 */
uint8_t Firmware_Finish(firmware_update_t *update)
{
    metadata_snapshot_t snapshot;
    firmware_metadata_t metadata;
    uint32_t actual_crc;
    uint32_t target;

    if (update == NULL || update != active_update ||
        update->state != FIRMWARE_RECEIVING)
    {
        return 0U;
    }
    if (!session_matches_metadata(update, &snapshot))
    {
        update->state = FIRMWARE_ERROR;
        return 0U;
    }
    if (update->received_size != update->total_size)
    {
        return 0U;
    }
    update->state = FIRMWARE_VERIFYING;
    if (!crc32_flash(update->storage_addr, update->total_size, &actual_crc) ||
        actual_crc != update->expected_crc32)
    {
        update->state = FIRMWARE_ERROR;
        return 0U;
    }
    metadata = snapshot.record;
    target = (uint32_t)update->target_slot;
    metadata.slot[target].firmware_size = update->total_size;
    metadata.slot[target].firmware_crc32 = actual_crc;
    metadata.slot[target].valid = FIRMWARE_IMAGE_VALID;
    metadata.candidate_slot = target;
    metadata.boot_phase = FIRMWARE_BOOT_PENDING;
    metadata.trial_boot_count = 0U;
    if (!metadata_store(&metadata, 1U, snapshot.record.generation))
    {
        update->state = FIRMWARE_ERROR;
        return 0U;
    }
    update->metadata_generation = metadata.generation;
    update->state = FIRMWARE_READY;
    active_update = NULL;
    return 1U;
}

/**
 * @brief 清理尚未发布候选的接收或错误会话，不执行任何 Flash 写入。
 * @param[in,out] update 当前活跃会话对象，成功后清零并将目标设为 NONE。
 * @return 1 清理成功；0 管理读取失败、候选已发布或状态不允许。
 */
uint8_t Firmware_Abort(firmware_update_t *update)
{
    metadata_snapshot_t snapshot;
    uint32_t target;

    if (update == NULL || update != active_update ||
        (update->state != FIRMWARE_RECEIVING && update->state != FIRMWARE_ERROR))
    {
        return 0U;
    }
    target = (uint32_t)update->target_slot;
    if (target >= FIRMWARE_SLOT_COUNT || !metadata_load(&snapshot) ||
        !snapshot.has_valid || snapshot.record.boot_phase != FIRMWARE_BOOT_NORMAL ||
        snapshot.record.candidate_slot != FIRMWARE_SLOT_NONE ||
        snapshot.record.confirmed_slot == target ||
        snapshot.record.slot[target].valid != FIRMWARE_IMAGE_INVALID)
    {
        return 0U;
    }
    memset(update, 0, sizeof(*update));
    update->state = FIRMWARE_IDLE;
    update->target_slot = FIRMWARE_SLOT_NONE;
    active_update = NULL;
    return 1U;
}

/**
 * @brief 当前新 App 完成必要自检后确认试运行成功。
 * @return 1 当前映像和候选均校验通过，已提交 NORMAL 确认记录；0 拒绝或失败。
 * @note 只接受 TRIAL，不接受仅下载成功的 PENDING；安装/跳转由 Bootloader 完成。
 */
uint8_t Firmware_Confirm(void)
{
    metadata_snapshot_t snapshot;
    firmware_metadata_t metadata;
    uint32_t candidate;

    if (registered_app == NULL || active_update != NULL ||
        !metadata_load(&snapshot) || !snapshot.has_valid ||
        snapshot.record.boot_phase != FIRMWARE_BOOT_TRIAL)
    {
        return 0U;
    }
    candidate = snapshot.record.candidate_slot;
    if (!running_image_matches(&snapshot.record, candidate))
    {
        return 0U;
    }
    metadata = snapshot.record;
    metadata.confirmed_slot = candidate;
    metadata.candidate_slot = FIRMWARE_SLOT_NONE;
    metadata.boot_phase = FIRMWARE_BOOT_NORMAL;
    metadata.trial_boot_count = 0U;
    /* 只改变确认状态；另一槽的正文和有效描述都保留。 */
    return metadata_store(&metadata, 1U, snapshot.record.generation);
}
