/**
 * @file test_firmware_startup.c
 * @brief 升级服务启动准备的主机测试，不访问真实硬件。
 * @details 使用模拟接口检查：仅空白管理区可以首次初始化；
 *          已有有效记录、损坏记录、读错误都不能被启动流程覆盖。
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

/** @brief 模拟当前 App 映像；测试不会运行或改写该映像。 */
static const uint8_t app_image[1024] = {0x5AU, 0xA5U, 0x12U, 0x34U};

/** @brief 管理区两份记录的原始字节，仅供模拟 W25Q128_Read。 */
static uint8_t raw_metadata[2][FIRMWARE_META_RECORD_SIZE];

/** @brief 模拟 Firmware_ReadMetadata 返回的完整管理对象。 */
static firmware_metadata_t metadata;

static unsigned checks;
static unsigned bind_calls, metadata_calls, baseline_calls, raw_read_calls;
static const uint8_t *bound_app;
static uint32_t bound_capacity, baseline_size;
static uint8_t bind_success, metadata_valid, baseline_success;
static unsigned fail_raw_read_call;
/** @brief getter 返回的只读静态诊断对象，在多次 Prepare 之间地址不变。 */
static const firmware_startup_diagnostic_t *diagnostic_reference;

/** @brief 检查失败或成功原因，并取得本次只读诊断快照。 */
static const firmware_startup_diagnostic_t *expect_status(uint32_t expected)
{
    const firmware_startup_diagnostic_t *diagnostic =
        Firmware_StartupGetDiagnostic();
    CHECK(diagnostic != NULL);
    CHECK(diagnostic == diagnostic_reference);
    CHECK((uint32_t)diagnostic->status == expected);
    return diagnostic;
}

/** @brief 未读取原始区时，诊断不能残留上次的读取成功标记。 */
static void expect_no_raw_reads(const firmware_startup_diagnostic_t *diagnostic)
{
    CHECK(diagnostic->raw_read_ok[0] == 0U);
    CHECK(diagnostic->raw_read_ok[1] == 0U);
}

/** @brief 两份原始记录读取成功时，诊断完整保存实际返回的 60 字节。 */
static void expect_raw_snapshot(const firmware_startup_diagnostic_t *diagnostic)
{
    CHECK(diagnostic->raw_read_ok[0] == 1U);
    CHECK(diagnostic->raw_read_ok[1] == 1U);
    CHECK(memcmp(diagnostic->raw, raw_metadata, sizeof(raw_metadata)) == 0);
}

/** @brief 管理字段必须来自真实解析结果，而不是固定填入默认值。 */
static void expect_metadata_snapshot(
    const firmware_startup_diagnostic_t *diagnostic)
{
    CHECK(diagnostic->boot_phase == metadata.boot_phase);
    CHECK(diagnostic->confirmed_slot == metadata.confirmed_slot);
    CHECK(diagnostic->candidate_slot == metadata.candidate_slot);
}

/**
 * @brief 模拟注册内部 App 区，不读取内存内容。
 * @param[in] app_start 当前 App 映像的起始地址。
 * @param[in] app_capacity 内部 App 分区容量，单位字节。
 * @return 测试预设的绑定结果。
 */
uint8_t Firmware_SetAppRegion(const uint8_t *app_start, uint32_t app_capacity)
{
    ++bind_calls;
    bound_app = app_start;
    bound_capacity = app_capacity;
    return bind_success;
}

/** @brief 模拟读取已有管理记录；无有效记录时保持输出不变。 */
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

/**
 * @brief 模拟首次基线创建，只记录实际传入的映像长度。
 * @note 真正的擦写和 CRC 不属于本启动辅助函数测试的范围。
 */
uint8_t Firmware_CreateBaseline(uint32_t firmware_size)
{
    ++baseline_calls;
    baseline_size = firmware_size;
    if (baseline_success) {
        metadata_valid = 1U;
    }
    return baseline_success;
}

/**
 * @brief 模拟读取两份管理记录原始字节。
 * @note 按读取次数注入错误，验证任何一份不可读都不会创建基线。
 */
uint8_t W25Q128_Read(uint8_t *buffer, uint32_t address, uint32_t length)
{
    unsigned index;
    ++raw_read_calls;
    CHECK(buffer != NULL);
    CHECK(length == FIRMWARE_META_RECORD_SIZE);
    CHECK(address == FIRMWARE_META0_ADDR || address == FIRMWARE_META1_ADDR);
    if (raw_read_calls == fail_raw_read_call) {
        return 0U;
    }
    index = address == FIRMWARE_META0_ADDR ? 0U : 1U;
    memcpy(buffer, raw_metadata[index], length);
    return 1U;
}

/** @brief 恢复诊断的状态读取接口占位；原启动测试不调用此路径。 */
uint8_t W25Q128_ReadStatus(uint8_t *status)
{
    (void)status;
    CHECK(0);
    return 0U;
}

/** @brief 旧启动测试不允许任何显式清理操作；擦除调用立即报错。 */
uint8_t W25Q128_Erase_Sector(uint32_t address)
{
    (void)address;
    CHECK(0);
    return 0U;
}

/** @brief 为每个独立场景恢复模拟状态，默认两份管理记录均为空白。 */
static void fixture(void)
{
    memset(raw_metadata, 0xFF, sizeof(raw_metadata));
    memset(&metadata, 0, sizeof(metadata));
    metadata.magic = FIRMWARE_META_MAGIC;
    metadata.format_version = FIRMWARE_META_VERSION;
    metadata.generation = 17U;
    metadata.slot[FIRMWARE_SLOT_A].firmware_size = sizeof(app_image);
    metadata.slot[FIRMWARE_SLOT_A].firmware_crc32 = 0x12345678UL;
    metadata.slot[FIRMWARE_SLOT_A].valid = FIRMWARE_IMAGE_VALID;
    metadata.slot[FIRMWARE_SLOT_B].valid = FIRMWARE_IMAGE_INVALID;
    metadata.confirmed_slot = FIRMWARE_SLOT_A;
    metadata.candidate_slot = FIRMWARE_SLOT_NONE;
    metadata.boot_phase = FIRMWARE_BOOT_NORMAL;
    metadata.trial_boot_count = 0U;
    metadata.record_crc32 = 0xAABBCCDDUL;
    metadata.commit = FIRMWARE_META_COMMIT;
    bind_calls = metadata_calls = baseline_calls = raw_read_calls = 0U;
    bound_app = NULL;
    bound_capacity = baseline_size = 0U;
    bind_success = baseline_success = 1U;
    metadata_valid = 0U;
    fail_raw_read_call = 0U;
}

/** @brief 尚未运行启动准备时，getter 已可读且原因是 NOT_RUN。 */
static void test_diagnostic_starts_not_run(void)
{
    diagnostic_reference = Firmware_StartupGetDiagnostic();
    CHECK(diagnostic_reference != NULL);
    CHECK(diagnostic_reference->status == FW_STARTUP_NOT_RUN);
    CHECK(Firmware_StartupGetDiagnostic() == diagnostic_reference);
}

/** @brief 空白区允许创建基线，传入实际映像长度而不是整个分区容量。 */
static void test_blank_initialization(void)
{
    fixture();
    CHECK(Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
    CHECK(bind_calls == 1U && bound_app == app_image);
    CHECK(bound_capacity == 4096U);
    CHECK(metadata_calls >= 1U && raw_read_calls == 2U);
    CHECK(baseline_calls == 1U && baseline_size == 700U);
    expect_raw_snapshot(expect_status(FW_STARTUP_READY_CREATED));
}

/** @brief 有效 A 或 B 确认记录不会被启动准备覆盖。 */
static void test_existing_normal_preserved(void)
{
    unsigned confirmed;
    firmware_metadata_t before;
    for (confirmed = 0U; confirmed < FIRMWARE_SLOT_COUNT; ++confirmed) {
        fixture();
        metadata_valid = 1U;
        metadata.confirmed_slot = confirmed;
        metadata.slot[confirmed].valid = FIRMWARE_IMAGE_VALID;
        metadata.slot[confirmed].firmware_size = 900U;
        metadata.slot[confirmed].firmware_crc32 = 0x87654321UL;
        before = metadata;
        CHECK(Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
        CHECK(bind_calls == 1U && metadata_calls >= 1U);
        CHECK(baseline_calls == 0U && raw_read_calls == 0U);
        CHECK(memcmp(&metadata, &before, sizeof(metadata)) == 0);
        expect_no_raw_reads(expect_status(FW_STARTUP_READY_EXISTING));
        expect_metadata_snapshot(Firmware_StartupGetDiagnostic());
    }
}

/** @brief 有效候选及所有安装阶段都拒绝自动基线初始化。 */
static void test_boot_operation_preserved(void)
{
    uint32_t phase;
    firmware_metadata_t before;
    for (phase = FIRMWARE_BOOT_PENDING; phase <= FIRMWARE_BOOT_ROLLBACK; ++phase) {
        fixture();
        metadata_valid = 1U;
        metadata.boot_phase = phase;
        metadata.candidate_slot = phase == FIRMWARE_BOOT_ROLLBACK
                                      ? FIRMWARE_SLOT_NONE : FIRMWARE_SLOT_B;
        metadata.slot[FIRMWARE_SLOT_B].valid = FIRMWARE_IMAGE_VALID;
        metadata.slot[FIRMWARE_SLOT_B].firmware_size = 800U;
        metadata.slot[FIRMWARE_SLOT_B].firmware_crc32 = 0x55667788UL;
        metadata.trial_boot_count = phase == FIRMWARE_BOOT_TRIAL ? 1U : 0U;
        before = metadata;
        CHECK(!Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
        CHECK(baseline_calls == 0U && raw_read_calls == 0U);
        CHECK(memcmp(&metadata, &before, sizeof(metadata)) == 0);
        expect_no_raw_reads(expect_status(FW_STARTUP_BOOT_PENDING));
        expect_metadata_snapshot(Firmware_StartupGetDiagnostic());
    }
}

/** @brief 即使 mock 提供了不一致管理字段，也不能触发覆盖初始化。 */
static void test_invalid_existing_fields(void)
{
    unsigned invalid_kind;
    for (invalid_kind = 0U; invalid_kind < 3U; ++invalid_kind) {
        fixture();
        metadata_valid = 1U;
        if (invalid_kind == 0U) {
            metadata.confirmed_slot = FIRMWARE_SLOT_NONE;
        } else if (invalid_kind == 1U) {
            metadata.candidate_slot = FIRMWARE_SLOT_B;
        } else {
            metadata.slot[FIRMWARE_SLOT_A].valid = FIRMWARE_IMAGE_INVALID;
        }
        CHECK(!Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
        CHECK(baseline_calls == 0U && raw_read_calls == 0U);
        expect_no_raw_reads(expect_status(FW_STARTUP_CONFIRM_INVALID));
        expect_metadata_snapshot(Firmware_StartupGetDiagnostic());
    }
}

/** @brief 无有效解析记录时，任何原始区读取失败都禁止初始化。 */
static void test_raw_read_failure(void)
{
    unsigned failed_copy;
    for (failed_copy = 1U; failed_copy <= 2U; ++failed_copy) {
        const firmware_startup_diagnostic_t *diagnostic;
        unsigned successful_index = failed_copy == 1U ? 1U : 0U;
        fixture();
        fail_raw_read_call = failed_copy;
        /* 另一份虽非空白但可读，优先报告读失败并保留其真实字节。 */
        raw_metadata[successful_index][3U] = 0xA3U;
        CHECK(!Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
        CHECK(raw_read_calls == 2U); /* 诊断路径始终尝试读取两份记录。 */
        CHECK(baseline_calls == 0U);
        diagnostic = expect_status(FW_STARTUP_META_READ_FAILED);
        CHECK(diagnostic->raw_read_ok[failed_copy - 1U] == 0U);
        CHECK(diagnostic->raw_read_ok[successful_index] == 1U);
        CHECK(memcmp(diagnostic->raw[successful_index],
                     raw_metadata[successful_index],
                     FIRMWARE_META_RECORD_SIZE) == 0);
    }
}

/** @brief 损坏记录不是空白区，不能把旧记录擦除并强制重新建立基线。 */
static void test_corrupt_nonblank_preserved(void)
{
    unsigned copy;
    uint8_t before[2][FIRMWARE_META_RECORD_SIZE];
    for (copy = 0U; copy < 2U; ++copy) {
        fixture();
        /* 在记录尾端留下一个编程位，验证检测包含整份 60 字节。 */
        raw_metadata[copy][FIRMWARE_META_RECORD_SIZE - 1U] = 0xFEU;
        memcpy(before, raw_metadata, sizeof(before));
        CHECK(!Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
        CHECK(baseline_calls == 0U);
        CHECK(memcmp(before, raw_metadata, sizeof(before)) == 0);
        CHECK(raw_read_calls == 2U);
        expect_raw_snapshot(expect_status(FW_STARTUP_META_NOT_BLANK));
    }
}

/** @brief 初始化开关关闭时，即使两份区为空白也不能创建基线。 */
static void test_initialization_disabled(void)
{
    fixture();
    CHECK(!Firmware_StartupPrepare(app_image, 4096U, 700U, 0U));
    CHECK(baseline_calls == 0U);
    expect_no_raw_reads(expect_status(FW_STARTUP_INIT_DISABLED));
    fixture();
    metadata_valid = 1U;
    CHECK(Firmware_StartupPrepare(app_image, 4096U, 700U, 0U));
    CHECK(baseline_calls == 0U && raw_read_calls == 0U);
    expect_no_raw_reads(expect_status(FW_STARTUP_READY_EXISTING));
    expect_metadata_snapshot(Firmware_StartupGetDiagnostic());
}

/** @brief 基线创建失败必须上报，不能假装升级服务已准备好。 */
static void test_baseline_failure(void)
{
    fixture();
    baseline_success = 0U;
    CHECK(!Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
    CHECK(baseline_calls == 1U && baseline_size == 700U);
    expect_raw_snapshot(expect_status(FW_STARTUP_BASELINE_FAILED));
}

/** @brief 参数不合法时，在注册和任何 Flash 访问之前拒绝。 */
static void test_parameter_ranges(void)
{
    unsigned invalid_case;
    for (invalid_case = 0U; invalid_case < 5U; ++invalid_case) {
        const uint8_t *start = app_image;
        uint32_t capacity = 4096U;
        uint32_t size = 700U;
        fixture();
        switch (invalid_case) {
            case 0U: start = NULL; break;
            case 1U: capacity = 0U; break;
            case 2U: size = 0U; break;
            case 3U: size = capacity + 1U; break;
            default: capacity = FIRMWARE_SLOT_SIZE + 1U; break;
        }
        CHECK(!Firmware_StartupPrepare(start, capacity, size, 1U));
        CHECK(bind_calls == 0U && metadata_calls == 0U);
        CHECK(baseline_calls == 0U && raw_read_calls == 0U);
        expect_no_raw_reads(expect_status(FW_STARTUP_INVALID_ARGUMENT));
    }
}

/** @brief 注册失败后，不继续读管理信息或尝试创建基线。 */
static void test_bind_failure(void)
{
    fixture();
    bind_success = 0U;
    CHECK(!Firmware_StartupPrepare(app_image, 4096U, 700U, 1U));
    CHECK(bind_calls == 1U && metadata_calls == 0U);
    CHECK(baseline_calls == 0U && raw_read_calls == 0U);
    expect_no_raw_reads(expect_status(FW_STARTUP_BIND_FAILED));
}

int main(void)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"diagnostic_starts_not_run", test_diagnostic_starts_not_run},
        {"blank_initialization_exact_size", test_blank_initialization},
        {"existing_normal_preserved", test_existing_normal_preserved},
        {"boot_operation_preserved", test_boot_operation_preserved},
        {"invalid_existing_fields", test_invalid_existing_fields},
        {"raw_read_failure", test_raw_read_failure},
        {"corrupt_nonblank_preserved", test_corrupt_nonblank_preserved},
        {"initialization_disabled", test_initialization_disabled},
        {"baseline_failure", test_baseline_failure},
        {"parameter_ranges", test_parameter_ranges},
        {"bind_failure", test_bind_failure}
    };
    unsigned i;
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cases[i].run();
        printf("PASS %s\n", cases[i].name);
    }
    printf("PASS firmware_startup: %u checks in %u cases\n", checks,
           (unsigned)(sizeof(cases) / sizeof(cases[0])));
    return 0;
}
