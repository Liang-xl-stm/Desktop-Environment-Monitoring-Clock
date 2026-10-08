/**
 * @file test_firmware_startup_recovery.c
 * @brief 主机模拟测试：仅清理明确授权的外来 META0 数据。
 * @details 两份管理区各模拟完整 4KB，验证 META1 记录空白时仅清理尾部旧数据，
 *          不会删除有效或损坏的 FWMD 记录，不访问真实 MCU/Flash。
 */
#include "firmware_startup.h"
#include "firmware_update.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static uint8_t sectors[2][FIRMWARE_META_SECTOR_SIZE];
static const uint8_t app_image[1024] = {0x5AU, 0xA5U, 0x12U, 0x34U};
static firmware_metadata_t metadata;
static unsigned checks, read_calls, erase_calls, metadata_calls;
static unsigned bind_calls, baseline_calls;
static uint32_t read_bytes[2], last_erase_address, baseline_size;
static uint32_t fail_read_address, fail_post_erase_address;
static uint32_t failed_chunk_address, fail_erase_address;
static uint32_t erase_addresses[8], erase_behavior_address;
static uint8_t metadata_valid, erase_success, baseline_success;
static unsigned erase_behavior;
static uint8_t erased_mask, sr1_success, sr1_value;
static unsigned status_calls;

/** @brief 本次日志中的既有数据指纹，仅授权删除这一份旧数据。 */
static const uint8_t authorized_prefix[16] = {
    0x2AU, 0x04U, 0x45U, 0xC5U, 0x6CU, 0x48U, 0x4CU, 0x88U,
    0xD2U, 0xD7U, 0xB4U, 0x10U, 0x7DU, 0x0AU, 0x84U, 0x38U
};

enum {
    ERASE_ALL = 0,
    ERASE_IGNORED,
    ERASE_ONLY_FIRST_60
};

/** @brief 编码独立测试数据；升级管理记录和截图中的外来标识都按小端放置。 */
static void put_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/** @brief 判断两个完整管理扇区的读取范围，越界读取直接判测试失败。 */
static unsigned read_index(uint32_t address, uint32_t length, uint32_t *offset)
{
    unsigned index;
    uint32_t base;
    if (address >= FIRMWARE_META0_ADDR &&
        address < FIRMWARE_META0_ADDR + FIRMWARE_META_SECTOR_SIZE) {
        index = 0U;
        base = FIRMWARE_META0_ADDR;
    } else {
        CHECK(address >= FIRMWARE_META1_ADDR &&
              address < FIRMWARE_META1_ADDR + FIRMWARE_META_SECTOR_SIZE);
        index = 1U;
        base = FIRMWARE_META1_ADDR;
    }
    *offset = address - base;
    CHECK(length > 0U && length <= FIRMWARE_META_SECTOR_SIZE - *offset);
    return index;
}

/**
 * @brief 模拟读取 Flash，可在任意包含指定地址的分块中注入错误。
 * @note 失败按地址而非固定分块长度定位，避免镜像生产实现的分块策略。
 */
uint8_t W25Q128_Read(uint8_t *buffer, uint32_t address, uint32_t length)
{
    uint32_t offset;
    unsigned index;
    CHECK(buffer != NULL);
    index = read_index(address, length, &offset);
    ++read_calls;
    if ((fail_read_address >= address && fail_read_address - address < length) ||
        ((erased_mask & (1U << index)) && fail_post_erase_address >= address &&
         fail_post_erase_address - address < length)) {
        failed_chunk_address = address;
        return 0U;
    }
    read_bytes[index] += length;
    memcpy(buffer, sectors[index] + offset, length);
    return 1U;
}

/**
 * @brief 模拟一次完整 4KB 擦除，只允许命中两个管理扇区，记录真实顺序。
 * @note 支持命令失败、保护区忽略、只擦除前 60 字节三种故障。
 */
uint8_t W25Q128_Erase_Sector(uint32_t address)
{
    unsigned index;
    CHECK(address == FIRMWARE_META0_ADDR || address == FIRMWARE_META1_ADDR);
    CHECK(erase_calls < sizeof(erase_addresses) / sizeof(erase_addresses[0]));
    erase_addresses[erase_calls] = address;
    ++erase_calls;
    last_erase_address = address;
    index = address == FIRMWARE_META0_ADDR ? 0U : 1U;
    if (!erase_success || address == fail_erase_address) {
        return 0U;
    }
    erased_mask |= (uint8_t)(1U << index);
    if (erase_behavior == ERASE_ALL || address != erase_behavior_address) {
        memset(sectors[index], 0xFF, sizeof(sectors[index]));
    } else if (erase_behavior == ERASE_ONLY_FIRST_60) {
        memset(sectors[index], 0xFF, FIRMWARE_META_RECORD_SIZE);
    }
    return 1U;
}

/** @brief 末尾只读状态采样；采样失败不决定清理的成功或失败。 */
uint8_t W25Q128_ReadStatus(uint8_t *status)
{
    ++status_calls;
    CHECK(status != NULL);
    if (!sr1_success) {
        return 0U;
    }
    *status = sr1_value;
    return 1U;
}

/** @brief 模拟升级服务读取有效管理记录；有效时不允许恢复函数删除它。 */
uint8_t Firmware_ReadMetadata(firmware_metadata_t *out)
{
    ++metadata_calls;
    CHECK(out != NULL);
    if (!metadata_valid) {
        return 0U;
    }
    *out = metadata;
    return 1U;
}

/** @brief 只为端到端 StartupPrepare 场景提供注册接口。 */
uint8_t Firmware_SetAppRegion(const uint8_t *app_start, uint32_t app_capacity)
{
    ++bind_calls;
    CHECK(app_start == app_image && app_capacity == 4096U);
    return 1U;
}

/**
 * @brief 模拟首次基线建立，并生成新的有效记录供后续保护测试使用。
 * @note 恢复函数自身不能调用这个接口；只有重新 Prepare 才能调用。
 */
uint8_t Firmware_CreateBaseline(uint32_t firmware_size)
{
    ++baseline_calls;
    baseline_size = firmware_size;
    if (!baseline_success) {
        return 0U;
    }
    metadata_valid = 1U;
    metadata.slot[FIRMWARE_SLOT_A].firmware_size = firmware_size;
    put_le32(sectors[0], FIRMWARE_META_MAGIC);
    return 1U;
}

/** @brief 每个场景初始化为空白管理区，不重置或绕过被测静态实现。 */
static void fixture(void)
{
    memset(sectors, 0xFF, sizeof(sectors));
    memset(&metadata, 0, sizeof(metadata));
    metadata.magic = FIRMWARE_META_MAGIC;
    metadata.format_version = FIRMWARE_META_VERSION;
    metadata.generation = 23U;
    metadata.slot[FIRMWARE_SLOT_A].firmware_size = 700U;
    metadata.slot[FIRMWARE_SLOT_A].firmware_crc32 = 0x12345678UL;
    metadata.slot[FIRMWARE_SLOT_A].valid = FIRMWARE_IMAGE_VALID;
    metadata.confirmed_slot = FIRMWARE_SLOT_A;
    metadata.candidate_slot = FIRMWARE_SLOT_NONE;
    metadata.boot_phase = FIRMWARE_BOOT_NORMAL;
    metadata.record_crc32 = 0xAABBCCDDUL;
    metadata.commit = FIRMWARE_META_COMMIT;
    read_calls = erase_calls = metadata_calls = bind_calls = baseline_calls = 0U;
    read_bytes[0] = read_bytes[1] = 0U;
    last_erase_address = baseline_size = 0U;
    fail_read_address = fail_post_erase_address = fail_erase_address = UINT32_MAX;
    failed_chunk_address = UINT32_MAX;
    metadata_valid = erased_mask = 0U;
    erase_success = baseline_success = 1U;
    erase_behavior = ERASE_ALL;
    erase_behavior_address = FIRMWARE_META0_ADDR;
    memset(erase_addresses, 0, sizeof(erase_addresses));
    status_calls = 0U;
    sr1_success = 1U;
    sr1_value = 0x1CU;
}

/** @brief 调用新清理接口，检查未更改启动诊断、独立 SR1 采样及 getter 只读。 */
static uint8_t run_cleanup(void)
{
    uint8_t before[sizeof(firmware_startup_diagnostic_t)];
    const firmware_cleanup_diagnostic_t *diagnostic;
    unsigned previous_samples = status_calls;
    uint8_t result;
    memcpy(before, Firmware_StartupGetDiagnostic(), sizeof(before));
    result = Firmware_StartupClearLegacyMetadata();
    CHECK(memcmp(before, Firmware_StartupGetDiagnostic(), sizeof(before)) == 0);
    CHECK(status_calls == previous_samples + 1U);
    diagnostic = Firmware_StartupGetCleanupDiagnostic();
    CHECK(diagnostic != NULL);
    CHECK(diagnostic->sr1_valid == sr1_success);
    if (sr1_success) {
        CHECK(diagnostic->sr1 == sr1_value);
    }
    previous_samples = status_calls;
    CHECK(Firmware_StartupGetCleanupDiagnostic() == diagnostic);
    CHECK(status_calls == previous_samples); /* getter 本身不能访问 Flash。 */
    return result;
}

/** @brief 每条失败路径报告具体原因与擦除进度；不以尝试过命令冒充已验证完成。 */
static const firmware_cleanup_diagnostic_t *expect_cleanup(
    uint32_t status, uint8_t attempted, uint8_t completed)
{
    const firmware_cleanup_diagnostic_t *diagnostic =
        Firmware_StartupGetCleanupDiagnostic();
    CHECK(diagnostic != NULL && (uint32_t)diagnostic->status == status);
    CHECK(diagnostic->erase_attempted_mask == attempted);
    CHECK(diagnostic->erase_completed_mask == completed);
    return diagnostic;
}

/** @brief 检查精确的非 FF 或指纹错误字节，避免仅打印没有意义的扇区起始。 */
static void expect_observed(const firmware_cleanup_diagnostic_t *diagnostic,
                            uint32_t address, uint8_t observed)
{
    CHECK(diagnostic->address == address);
    CHECK(diagnostic->byte_valid == 1U && diagnostic->observed_byte == observed);
}

/** @brief 检查清理函数没有偷偷注册 App 或建立新的基线。 */
static void check_no_baseline(void)
{
    CHECK(bind_calls == 0U && baseline_calls == 0U);
}

/** @brief 构造截图中确切的 16 字节旧数据指纹，不是通用删除任意 foreign。 */
static void make_foreign_metadata0(void)
{
    memcpy(sectors[0], authorized_prefix, sizeof(authorized_prefix));
    sectors[0][FIRMWARE_META_SECTOR_SIZE - 1U] = 0x71U;
}

/** @brief 外来 META0 可完整清除，META1 必须逐字节保持不变。 */
static void test_foreign_data_cleared_only_in_metadata0(void)
{
    uint8_t meta1_before[FIRMWARE_META_SECTOR_SIZE];
    unsigned i;
    fixture();
    make_foreign_metadata0();
    memcpy(meta1_before, sectors[1], sizeof(meta1_before));
    CHECK(run_cleanup());
    CHECK(erase_calls == 1U && last_erase_address == FIRMWARE_META0_ADDR);
    for (i = 0U; i < sizeof(sectors[0]); ++i) {
        CHECK(sectors[0][i] == 0xFFU);
    }
    CHECK(memcmp(meta1_before, sectors[1], sizeof(meta1_before)) == 0);
    CHECK(read_bytes[1] >= FIRMWARE_META_SECTOR_SIZE);
    {
        const firmware_cleanup_diagnostic_t *diagnostic =
            expect_cleanup(FW_CLEANUP_READY, 1U, 1U);
        CHECK(diagnostic->meta0_first_non_ff == FIRMWARE_META0_ADDR);
        CHECK(diagnostic->meta1_first_non_ff == UINT32_MAX);
    }
    check_no_baseline();
}

/** @brief 完全空白的 META0 返回成功，不重复擦除已有空白扇区。 */
static void test_blank_metadata0_no_erase(void)
{
    fixture();
    CHECK(run_cleanup());
    CHECK(erase_calls == 0U);
    CHECK(read_bytes[0] >= FIRMWARE_META_SECTOR_SIZE);
    CHECK(read_bytes[1] >= FIRMWARE_META_SECTOR_SIZE);
    {
        const firmware_cleanup_diagnostic_t *diagnostic =
            expect_cleanup(FW_CLEANUP_READY, 0U, 0U);
        CHECK(diagnostic->meta0_first_non_ff == UINT32_MAX);
        CHECK(diagnostic->meta1_first_non_ff == UINT32_MAX);
    }
    check_no_baseline();
}

/** @brief 任意有效记录都禁止清理，包括待安装、试运行和回滚阶段。 */
static void test_valid_metadata_preserved(void)
{
    uint32_t phase;
    for (phase = FIRMWARE_BOOT_NORMAL; phase <= FIRMWARE_BOOT_ROLLBACK; ++phase) {
        uint8_t before[sizeof(sectors)];
        fixture();
        make_foreign_metadata0();
        metadata_valid = 1U;
        metadata.boot_phase = phase;
        metadata.candidate_slot =
            phase == FIRMWARE_BOOT_NORMAL || phase == FIRMWARE_BOOT_ROLLBACK
                ? FIRMWARE_SLOT_NONE : FIRMWARE_SLOT_B;
        metadata.slot[FIRMWARE_SLOT_B].valid = FIRMWARE_IMAGE_VALID;
        metadata.slot[FIRMWARE_SLOT_B].firmware_size = 800U;
        metadata.slot[FIRMWARE_SLOT_B].firmware_crc32 = 0x87654321UL;
        metadata.trial_boot_count = phase == FIRMWARE_BOOT_TRIAL ? 1U : 0U;
        put_le32(sectors[1], FIRMWARE_META_MAGIC);
        memcpy(before, sectors, sizeof(before));
        CHECK(!run_cleanup());
        CHECK(erase_calls == 0U);
        CHECK(memcmp(before, sectors, sizeof(before)) == 0);
        expect_cleanup(FW_CLEANUP_VALID_RECORD, 0U, 0U);
        check_no_baseline();
    }
}

/** @brief FWMD 即使版本或 CRC 损坏，也属于升级记录，不能按外来数据删除。 */
static void test_damaged_fwmd_preserved(void)
{
    uint8_t before[FIRMWARE_META_SECTOR_SIZE];
    fixture();
    put_le32(sectors[0], FIRMWARE_META_MAGIC);
    put_le32(sectors[0] + 4U, 0xAABBCCDDUL); /* 故意破坏格式版本。 */
    sectors[0][FIRMWARE_META_SECTOR_SIZE - 1U] = 0x71U;
    memcpy(before, sectors[0], sizeof(before));
    CHECK(!run_cleanup());
    CHECK(erase_calls == 0U);
    CHECK(memcmp(before, sectors[0], sizeof(before)) == 0);
    expect_cleanup(FW_CLEANUP_META0_PROTECTED, 0U, 0U);
    check_no_baseline();
}

/** @brief META1 记录前 60 字节空白，尾部旧数据必须先擦，再处理 META0。 */
static void test_metadata1_tail_cleared_first(void)
{
    unsigned i;
    uint8_t startup_before[sizeof(firmware_startup_diagnostic_t)];
    const firmware_cleanup_diagnostic_t *diagnostic;
    fixture();
    make_foreign_metadata0();
    sectors[1][FIRMWARE_META_SECTOR_SIZE - 1U] = 0x81U;
    memcpy(startup_before, Firmware_StartupGetDiagnostic(), sizeof(startup_before));
    /* 旧实现会在这里拒绝，因此首场景可以确实观察本次行为修复的 RED。 */
    CHECK(Firmware_StartupClearLegacyMetadata());
    CHECK(erase_calls == 2U);
    CHECK(erase_addresses[0] == FIRMWARE_META1_ADDR);
    CHECK(erase_addresses[1] == FIRMWARE_META0_ADDR);
    for (i = 0U; i < sizeof(sectors[0]); ++i) {
        CHECK(sectors[0][i] == 0xFFU && sectors[1][i] == 0xFFU);
    }
    diagnostic = expect_cleanup(FW_CLEANUP_READY, 3U, 3U);
    CHECK(diagnostic->meta0_first_non_ff == FIRMWARE_META0_ADDR);
    CHECK(diagnostic->meta1_first_non_ff ==
          FIRMWARE_META1_ADDR + FIRMWARE_META_SECTOR_SIZE - 1U);
    CHECK(status_calls == 1U && diagnostic->sr1_valid == 1U);
    CHECK(diagnostic->sr1 == sr1_value);
    CHECK(memcmp(startup_before, Firmware_StartupGetDiagnostic(),
                 sizeof(startup_before)) == 0);
    check_no_baseline();
}

/** @brief META1 前 60 字节任何非 FF 都视为记录，不能靠擦除绕过保护。 */
static void test_metadata1_record_protected(void)
{
    unsigned offset;
    for (offset = 0U; offset < FIRMWARE_META_RECORD_SIZE;
         offset += FIRMWARE_META_RECORD_SIZE - 1U) {
        uint8_t before[sizeof(sectors)];
        fixture();
        make_foreign_metadata0();
        sectors[1][offset] = 0xB1U;
        memcpy(before, sectors, sizeof(before));
        CHECK(!run_cleanup());
        CHECK(erase_calls == 0U && memcmp(before, sectors, sizeof(before)) == 0);
        expect_observed(expect_cleanup(FW_CLEANUP_META1_PROTECTED, 0U, 0U),
                         FIRMWARE_META1_ADDR + offset, 0xB1U);
        check_no_baseline();
    }
}

/** @brief META0 前 60 字节空白但尾部有数据，不匹配授权指纹，必须保留。 */
static void test_metadata0_unknown_tail_preserved(void)
{
    uint8_t before[FIRMWARE_META_SECTOR_SIZE];
    fixture();
    sectors[0][FIRMWARE_META_SECTOR_SIZE - 1U] = 0x91U;
    memcpy(before, sectors[0], sizeof(before));
    CHECK(!run_cleanup());
    CHECK(erase_calls == 0U);
    CHECK(memcmp(before, sectors[0], sizeof(before)) == 0);
    expect_observed(expect_cleanup(FW_CLEANUP_PREFIX_MISMATCH, 0U, 0U),
                     FIRMWARE_META0_ADDR, 0xFFU);
    check_no_baseline();
}

/** @brief 指纹任何一字节不同都禁止删除，避免开关持续启用扩大授权范围。 */
static void test_unknown_foreign_prefix_preserved(void)
{
    unsigned changed_byte;
    for (changed_byte = 0U; changed_byte < sizeof(authorized_prefix); ++changed_byte) {
        uint8_t before[FIRMWARE_META_SECTOR_SIZE];
        fixture();
        make_foreign_metadata0();
        sectors[0][changed_byte] ^= 1U;
        memcpy(before, sectors[0], sizeof(before));
        CHECK(!run_cleanup());
        CHECK(erase_calls == 0U);
        CHECK(memcmp(before, sectors[0], sizeof(before)) == 0);
        expect_observed(expect_cleanup(FW_CLEANUP_PREFIX_MISMATCH, 0U, 0U),
                         FIRMWARE_META0_ADDR + changed_byte,
                         sectors[0][changed_byte]);
        check_no_baseline();
    }
}

/** @brief 任意必要分块不可读都不能进行擦除，不以读失败代替空白。 */
static void test_pre_erase_read_failure(void)
{
    static const uint32_t failed_addresses[] = {
        FIRMWARE_META0_ADDR,
        FIRMWARE_META1_ADDR,
        FIRMWARE_META1_ADDR + 2000U,
        FIRMWARE_META1_ADDR + FIRMWARE_META_SECTOR_SIZE - 1U
    };
    unsigned i;
    for (i = 0U; i < sizeof(failed_addresses) / sizeof(failed_addresses[0]); ++i) {
        uint8_t before[sizeof(sectors)];
        fixture();
        make_foreign_metadata0();
        fail_read_address = failed_addresses[i];
        memcpy(before, sectors, sizeof(before));
        CHECK(!run_cleanup());
        CHECK(erase_calls == 0U);
        CHECK(memcmp(before, sectors, sizeof(before)) == 0);
        {
            const firmware_cleanup_diagnostic_t *diagnostic = expect_cleanup(
                failed_addresses[i] < FIRMWARE_META1_ADDR
                    ? FW_CLEANUP_META0_READ_FAILED : FW_CLEANUP_META1_READ_FAILED,
                0U, 0U);
            CHECK(diagnostic->address == failed_chunk_address);
        }
        check_no_baseline();
    }
}

/** @brief 擦除命令失败不返回成功，也不偷偷建立基线。 */
static void test_erase_failure(void)
{
    uint8_t before[FIRMWARE_META_SECTOR_SIZE];
    fixture();
    make_foreign_metadata0();
    erase_success = 0U;
    memcpy(before, sectors[0], sizeof(before));
    CHECK(!run_cleanup());
    CHECK(erase_calls == 1U && last_erase_address == FIRMWARE_META0_ADDR);
    CHECK(memcmp(before, sectors[0], sizeof(before)) == 0);
    CHECK(expect_cleanup(FW_CLEANUP_META0_ERASE_FAILED, 1U, 0U)->address ==
          FIRMWARE_META0_ADDR);
    check_no_baseline();
}

/** @brief 命令假成功但仍存在非 FF 数据，完整读回检查必须发现它。 */
static void test_erase_not_complete(void)
{
    unsigned behavior;
    for (behavior = ERASE_IGNORED; behavior <= ERASE_ONLY_FIRST_60; ++behavior) {
        fixture();
        make_foreign_metadata0();
        erase_behavior = behavior;
        CHECK(!run_cleanup());
        CHECK(erase_calls == 1U && last_erase_address == FIRMWARE_META0_ADDR);
        CHECK(sectors[0][FIRMWARE_META_SECTOR_SIZE - 1U] != 0xFFU);
        expect_observed(expect_cleanup(FW_CLEANUP_META0_VERIFY_DIRTY, 1U, 0U),
                         behavior == ERASE_IGNORED ? FIRMWARE_META0_ADDR :
                             FIRMWARE_META0_ADDR + FIRMWARE_META_SECTOR_SIZE - 1U,
                         behavior == ERASE_IGNORED ? authorized_prefix[0] : 0x71U);
        check_no_baseline();
    }
}

/** @brief 擦后前部或后部读回失败，都不能宣称清理已经验证成功。 */
static void test_post_erase_read_failure(void)
{
    unsigned failed_offset;
    for (failed_offset = 0U; failed_offset <= 2000U; failed_offset += 2000U) {
        fixture();
        make_foreign_metadata0();
        fail_post_erase_address = FIRMWARE_META0_ADDR + failed_offset;
        CHECK(!run_cleanup());
        CHECK(erase_calls == 1U && last_erase_address == FIRMWARE_META0_ADDR);
        CHECK(expect_cleanup(FW_CLEANUP_META0_VERIFY_READ_FAILED, 1U, 0U)->address ==
              failed_chunk_address);
        check_no_baseline();
    }
}

/** @brief META1 先擦失败、读回失败或仍脏，必须保留 META0 指纹供安全重试。 */
static void test_metadata1_failures_preserve_retry(void)
{
    unsigned failure_kind;
    for (failure_kind = 0U; failure_kind < 3U; ++failure_kind) {
        const firmware_cleanup_diagnostic_t *diagnostic;
        fixture();
        make_foreign_metadata0();
        sectors[1][FIRMWARE_META_SECTOR_SIZE - 1U] = 0x81U;
        if (failure_kind == 0U) {
            fail_erase_address = FIRMWARE_META1_ADDR;
        } else if (failure_kind == 1U) {
            fail_post_erase_address = FIRMWARE_META1_ADDR + 2000U;
        } else {
            erase_behavior_address = FIRMWARE_META1_ADDR;
            erase_behavior = ERASE_ONLY_FIRST_60;
        }
        CHECK(!run_cleanup());
        CHECK(erase_calls == 1U && erase_addresses[0] == FIRMWARE_META1_ADDR);
        CHECK(memcmp(sectors[0], authorized_prefix, sizeof(authorized_prefix)) == 0);
        diagnostic = expect_cleanup(
            failure_kind == 0U ? FW_CLEANUP_META1_ERASE_FAILED :
            failure_kind == 1U ? FW_CLEANUP_META1_VERIFY_READ_FAILED :
                                FW_CLEANUP_META1_VERIFY_DIRTY,
            2U, 0U);
        if (failure_kind == 0U) {
            CHECK(diagnostic->address == FIRMWARE_META1_ADDR);
        } else if (failure_kind == 1U) {
            CHECK(diagnostic->address == failed_chunk_address);
        } else {
            expect_observed(diagnostic,
                FIRMWARE_META1_ADDR + FIRMWARE_META_SECTOR_SIZE - 1U, 0x81U);
        }
        check_no_baseline();

        /* 故障消失后可重新完整扫描，不需要抹掉 META0 指纹强行恢复。 */
        fail_erase_address = fail_post_erase_address = UINT32_MAX;
        erase_behavior = ERASE_ALL;
        CHECK(run_cleanup());
        CHECK(last_erase_address == FIRMWARE_META0_ADDR);
        expect_cleanup(FW_CLEANUP_READY, failure_kind == 1U ? 1U : 3U,
                        failure_kind == 1U ? 1U : 3U);
        check_no_baseline();
    }
}

/** @brief META1 已验证完成而 META0 失败时，诊断掩码必须保留 META1 完成位。 */
static void test_metadata0_failure_after_metadata1(void)
{
    fixture();
    make_foreign_metadata0();
    sectors[1][FIRMWARE_META_SECTOR_SIZE - 1U] = 0x81U;
    fail_erase_address = FIRMWARE_META0_ADDR;
    CHECK(!run_cleanup());
    CHECK(erase_calls == 2U && erase_addresses[0] == FIRMWARE_META1_ADDR);
    CHECK(erase_addresses[1] == FIRMWARE_META0_ADDR);
    CHECK(expect_cleanup(FW_CLEANUP_META0_ERASE_FAILED, 3U, 2U)->address ==
          FIRMWARE_META0_ADDR);
    CHECK(memcmp(sectors[0], authorized_prefix, sizeof(authorized_prefix)) == 0);
    fail_erase_address = UINT32_MAX;
    CHECK(run_cleanup());
    expect_cleanup(FW_CLEANUP_READY, 1U, 1U);
    check_no_baseline();
}

/** @brief 状态寄存器只作为独立诊断，采样失败不推翻实际清理或保护结果。 */
static void test_status_sample_unavailable(void)
{
    fixture();
    make_foreign_metadata0();
    sr1_success = 0U;
    CHECK(run_cleanup());
    CHECK(expect_cleanup(FW_CLEANUP_READY, 1U, 1U)->sr1_valid == 0U);
    CHECK(erase_calls == 1U);
    check_no_baseline();

    fixture();
    make_foreign_metadata0();
    sectors[0][7U] ^= 1U;
    sr1_success = 0U;
    CHECK(!run_cleanup());
    CHECK(expect_cleanup(FW_CLEANUP_PREFIX_MISMATCH, 0U, 0U)->sr1_valid == 0U);
    CHECK(erase_calls == 0U);
    check_no_baseline();
}

/**
 * @brief 用户场景：foreign 标识先导致 NOT_READY，授权清理后再独立建立基线。
 * @note 清理函数本身不建基线；管理记录建立后第二次清理仍然拒绝。
 */
static void test_prepare_recover_prepare_flow(void)
{
    unsigned before_erases;
    fixture();
    make_foreign_metadata0();
    CHECK(!Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
    CHECK(Firmware_StartupGetDiagnostic()->status == FW_STARTUP_META_NOT_BLANK);
    CHECK(bind_calls == 1U && baseline_calls == 0U && erase_calls == 0U);

    CHECK(run_cleanup());
    CHECK(bind_calls == 1U && baseline_calls == 0U && erase_calls == 1U);
    CHECK(Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
    CHECK(Firmware_StartupGetDiagnostic()->status == FW_STARTUP_READY_CREATED);
    CHECK(bind_calls == 2U && baseline_calls == 1U && baseline_size == 700U);

    before_erases = erase_calls;
    CHECK(!run_cleanup());
    CHECK(erase_calls == before_erases && baseline_calls == 1U);
    CHECK(Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
    CHECK(Firmware_StartupGetDiagnostic()->status == FW_STARTUP_READY_EXISTING);
    CHECK(erase_calls == before_erases && baseline_calls == 1U);
}

int main(void)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"metadata1_tail_cleared_first", test_metadata1_tail_cleared_first},
        {"foreign_data_only_metadata0", test_foreign_data_cleared_only_in_metadata0},
        {"blank_no_erase", test_blank_metadata0_no_erase},
        {"valid_metadata_preserved", test_valid_metadata_preserved},
        {"damaged_fwmd_preserved", test_damaged_fwmd_preserved},
        {"metadata1_record_protected", test_metadata1_record_protected},
        {"metadata0_unknown_tail_preserved", test_metadata0_unknown_tail_preserved},
        {"unknown_foreign_prefix_preserved", test_unknown_foreign_prefix_preserved},
        {"pre_erase_read_failure", test_pre_erase_read_failure},
        {"erase_failure", test_erase_failure},
        {"erase_not_complete", test_erase_not_complete},
        {"post_erase_read_failure", test_post_erase_read_failure},
        {"metadata1_failures_preserve_retry", test_metadata1_failures_preserve_retry},
        {"metadata0_failure_after_metadata1", test_metadata0_failure_after_metadata1},
        {"status_sample_unavailable", test_status_sample_unavailable},
        {"prepare_recover_prepare", test_prepare_recover_prepare_flow}
    };
    unsigned i;
    CHECK(Firmware_StartupGetCleanupDiagnostic() != NULL);
    CHECK(Firmware_StartupGetCleanupDiagnostic()->status == FW_CLEANUP_NOT_RUN);
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cases[i].run();
        printf("PASS %s\n", cases[i].name);
    }
    printf("PASS firmware_startup_recovery: %u checks in %u cases\n", checks,
           (unsigned)(sizeof(cases) / sizeof(cases[0])));
    return 0;
}
