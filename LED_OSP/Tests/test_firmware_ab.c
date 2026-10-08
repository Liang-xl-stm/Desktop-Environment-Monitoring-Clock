/* Host-only NOR model. Run each named case in a fresh process (CPU reset). */
#include "firmware_update.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLASH_SIZE 0x01000000UL
#define SECTOR_SIZE 4096UL
#define META_MAGIC 0x444D5746UL
#define META_COMMIT 0xA55AA55AUL
#define SLOT_A 0x00100000UL
#define SLOT_B 0x00200000UL
#define META0 0x000FE000UL
#define META1 0x000FF000UL

static uint8_t flash_memory[FLASH_SIZE];
static uint8_t app_memory[0x00100000UL];
static uint8_t candidate[2048];
static firmware_update_t update;
static unsigned checks;
static unsigned read_calls, write_calls, erase_calls;
static uint32_t erase_addresses[2048];

static unsigned fail_read_call, fail_write_call, fail_erase_call;
static uint32_t partial_write_length;
static uint32_t fail_meta_write_length;
static uint32_t ignore_erase_address = UINT32_MAX;
static uint32_t ignore_write_address = UINT32_MAX;
static int fail_after_commit;
static uint32_t committed_address = UINT32_MAX;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static int range_ok(uint32_t address, uint32_t length)
{
    return length != 0U && address < FLASH_SIZE &&
           length <= FLASH_SIZE - address;
}

static int is_metadata(uint32_t address)
{
    return (address >= META0 && address < META0 + SECTOR_SIZE) ||
           (address >= META1 && address < META1 + SECTOR_SIZE);
}

uint8_t W25Q128_Read(uint8_t *buffer, uint32_t address, uint32_t length)
{
    if (buffer == NULL || !range_ok(address, length)) {
        return 0U;
    }
    ++read_calls;
    if (fail_read_call != 0U && read_calls == fail_read_call) {
        fail_read_call = 0U;
        return 0U;
    }
    if (fail_after_commit && committed_address != UINT32_MAX &&
        address == committed_address && length == 60U) {
        fail_after_commit = 0;
        return 0U;
    }
    memcpy(buffer, flash_memory + address, length);
    return 1U;
}

uint8_t W25Q128_Write(const uint8_t *buffer, uint32_t address, uint32_t length)
{
    uint32_t i;
    uint32_t written = length;
    int fail = 0;
    if (buffer == NULL || !range_ok(address, length)) {
        return 0U;
    }
    ++write_calls;
    if ((fail_write_call != 0U && write_calls == fail_write_call) ||
        (fail_meta_write_length != 0U && is_metadata(address) &&
         length == fail_meta_write_length)) {
        fail = 1;
        written = partial_write_length < length ? partial_write_length : length;
        fail_write_call = 0U;
        fail_meta_write_length = 0U;
    }
    if (address == ignore_write_address) {
        ignore_write_address = UINT32_MAX;
        return 1U; /* Protected NOR can ignore programming despite bus success. */
    }
    for (i = 0U; i < written; ++i) {
        flash_memory[address + i] &= buffer[i]; /* NOR cannot restore 0 to 1. */
    }
    if (!fail && is_metadata(address) && length == 4U &&
        (address == META0 + 56U || address == META1 + 56U)) {
        committed_address = address - 56U;
    }
    return fail ? 0U : 1U;
}

uint8_t W25Q128_Erase_Sector(uint32_t address)
{
    if (!range_ok(address, SECTOR_SIZE) || address % SECTOR_SIZE != 0U) {
        return 0U;
    }
    CHECK(erase_calls < sizeof(erase_addresses) / sizeof(erase_addresses[0]));
    erase_addresses[erase_calls++] = address;
    if (fail_erase_call != 0U && erase_calls == fail_erase_call) {
        fail_erase_call = 0U;
        return 0U;
    }
    if (address == ignore_erase_address) {
        ignore_erase_address = UINT32_MAX;
        return 1U;
    }
    memset(flash_memory + address, 0xFF, SECTOR_SIZE);
    return 1U;
}

/* Independent reflected IEEE CRC with the standard known vector below. */
static uint32_t model_crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = UINT32_MAX;
    uint32_t i;
    unsigned bit;
    for (i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
        }
    }
    return crc ^ UINT32_MAX;
}

static void put_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static uint32_t get_le32(const uint8_t *source)
{
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8) |
           ((uint32_t)source[2] << 16) | ((uint32_t)source[3] << 24);
}

/* Encode a Bootloader-created record without using production static helpers. */
static void model_store_metadata(uint32_t address, const firmware_metadata_t *meta)
{
    uint8_t bytes[60];
    uint32_t i;
    memset(bytes, 0, sizeof(bytes));
    put_le32(bytes + 0U, META_MAGIC);
    put_le32(bytes + 4U, 1U);
    put_le32(bytes + 8U, meta->generation);
    for (i = 0U; i < 2U; ++i) {
        put_le32(bytes + 12U + i * 12U, meta->slot[i].firmware_size);
        put_le32(bytes + 16U + i * 12U, meta->slot[i].firmware_crc32);
        put_le32(bytes + 20U + i * 12U, meta->slot[i].valid);
    }
    put_le32(bytes + 36U, meta->confirmed_slot);
    put_le32(bytes + 40U, meta->candidate_slot);
    put_le32(bytes + 44U, meta->boot_phase);
    put_le32(bytes + 48U, meta->trial_boot_count);
    put_le32(bytes + 52U, model_crc32(bytes, 52U));
    put_le32(bytes + 56U, META_COMMIT);
    CHECK(W25Q128_Erase_Sector(address));
    CHECK(W25Q128_Write(bytes, address, 56U));
    CHECK(W25Q128_Write(bytes + 56U, address + 56U, 4U));
}

static int model_record_matches(uint32_t address, uint32_t generation)
{
    const uint8_t *bytes = flash_memory + address;
    return get_le32(bytes) == META_MAGIC && get_le32(bytes + 4U) == 1U &&
           get_le32(bytes + 8U) == generation &&
           get_le32(bytes + 52U) == model_crc32(bytes, 52U) &&
           get_le32(bytes + 56U) == META_COMMIT;
}

static uint32_t other_metadata_address(const firmware_metadata_t *current)
{
    if (model_record_matches(META0, current->generation)) {
        return META1;
    }
    CHECK(model_record_matches(META1, current->generation));
    return META0;
}

static void initialize_model(void)
{
    uint32_t i;
    memset(flash_memory, 0xFF, sizeof(flash_memory));
    for (i = 0U; i < sizeof(app_memory); ++i) {
        app_memory[i] = (uint8_t)(i * 13U + (i >> 8) + 7U);
    }
    for (i = 0U; i < sizeof(candidate); ++i) {
        candidate[i] = (uint8_t)(i * 29U + (i >> 3) + 0x63U);
    }
    CHECK(model_crc32((const uint8_t *)"123456789", 9U) == 0xCBF43926UL);
}

static void create_baseline(uint32_t app_capacity, uint32_t baseline_size)
{
    firmware_metadata_t meta;
    CHECK(Firmware_SetAppRegion(app_memory, app_capacity));
    CHECK(Firmware_CreateBaseline(baseline_size));
    CHECK(Firmware_ReadMetadata(&meta));
    CHECK(meta.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(meta.candidate_slot == FIRMWARE_SLOT_NONE);
    CHECK(meta.boot_phase == FIRMWARE_BOOT_NORMAL);
    CHECK(meta.slot[0].valid == FIRMWARE_IMAGE_VALID);
    CHECK(meta.slot[0].firmware_size == baseline_size);
    CHECK(meta.slot[0].firmware_crc32 == model_crc32(app_memory, baseline_size));
    CHECK(meta.slot[1].valid == FIRMWARE_IMAGE_INVALID);
    CHECK(memcmp(app_memory, flash_memory + SLOT_A, baseline_size) == 0);
}

static void verify_no_slot_erase(unsigned first, uint32_t slot_address)
{
    unsigned i;
    for (i = first; i < erase_calls; ++i) {
        CHECK(erase_addresses[i] < slot_address ||
              erase_addresses[i] >= slot_address + FIRMWARE_SLOT_SIZE);
    }
}

static void test_uninitialized(void)
{
    firmware_metadata_t meta;
    CHECK(!Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(erase_calls == 0U && write_calls == 0U);
    CHECK(Firmware_SetAppRegion(app_memory, sizeof(app_memory)));
    CHECK(!Firmware_ReadMetadata(&meta));
    CHECK(!Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(erase_calls == 0U && write_calls == 0U);
    CHECK(!Firmware_Confirm());
}

static void test_baseline(void)
{
    unsigned previous_erases, previous_writes;
    CHECK(!Firmware_SetAppRegion(NULL, 4096U));
    CHECK(!Firmware_SetAppRegion(app_memory, 0U));
    CHECK(!Firmware_SetAppRegion(app_memory, FIRMWARE_SLOT_SIZE + 1U));
    create_baseline(sizeof(app_memory), 5000U);
    previous_erases = erase_calls;
    previous_writes = write_calls;
    CHECK(!Firmware_CreateBaseline(5000U));
    CHECK(erase_calls == previous_erases && write_calls == previous_writes);
    CHECK(!Firmware_CreateBaseline(0U));
    CHECK(!Firmware_CreateBaseline(FIRMWARE_SLOT_SIZE + 1U));
}

static void test_cycle(void)
{
    firmware_metadata_t meta;
    unsigned before;
    uint32_t crc = model_crc32(candidate, 600U);
    create_baseline(sizeof(app_memory), 5000U);
    before = erase_calls;
    CHECK(Firmware_Begin(&update, 600U, crc));
    CHECK(update.target_slot == FIRMWARE_SLOT_B);
    CHECK(update.storage_addr == SLOT_B && update.received_size == 0U);
    CHECK(update.total_size == 600U && update.expected_crc32 == crc);
    verify_no_slot_erase(before, SLOT_A);
    CHECK(Firmware_Write(&update, candidate, 17U));
    CHECK(Firmware_Write(&update, candidate + 17U, 255U));
    CHECK(Firmware_Write(&update, candidate + 272U, 328U));
    CHECK(update.received_size == 600U);
    CHECK(update.state == FIRMWARE_RECEIVING);
    CHECK(Firmware_Finish(&update));
    CHECK(update.state == FIRMWARE_READY);
    CHECK(Firmware_ReadMetadata(&meta));
    CHECK(meta.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(meta.candidate_slot == FIRMWARE_SLOT_B);
    CHECK(meta.boot_phase == FIRMWARE_BOOT_PENDING);
    CHECK(meta.slot[1].valid == FIRMWARE_IMAGE_VALID);
    CHECK(meta.slot[1].firmware_crc32 == crc);
    CHECK(memcmp(flash_memory + SLOT_B, candidate, 600U) == 0);
    CHECK(memcmp(flash_memory + SLOT_A, app_memory, 5000U) == 0);
    CHECK(!Firmware_Begin(&update, 900U, model_crc32(candidate, 900U)));
    CHECK(!Firmware_Abort(&update));
    CHECK(!Firmware_Confirm());

    /* Simulate Bootloader installing B and entering TRIAL; no real Boot code. */
    meta.generation++;
    meta.boot_phase = FIRMWARE_BOOT_TRIAL;
    meta.trial_boot_count = 1U;
    model_store_metadata(other_metadata_address(&((firmware_metadata_t){
        .generation = meta.generation - 1U })), &meta);
    CHECK(!Firmware_Confirm()); /* Actual App still contains A. */
    memcpy(app_memory, flash_memory + SLOT_B, 600U);
    CHECK(Firmware_Confirm());
    CHECK(Firmware_ReadMetadata(&meta));
    CHECK(meta.confirmed_slot == FIRMWARE_SLOT_B);
    CHECK(meta.candidate_slot == FIRMWARE_SLOT_NONE);
    CHECK(meta.boot_phase == FIRMWARE_BOOT_NORMAL);
    CHECK(memcmp(flash_memory + SLOT_A, candidate, 600U) != 0);

    before = erase_calls;
    CHECK(Firmware_Begin(&update, 900U, model_crc32(candidate, 900U)));
    CHECK(update.target_slot == FIRMWARE_SLOT_A && update.storage_addr == SLOT_A);
    verify_no_slot_erase(before, SLOT_B);
    CHECK(memcmp(flash_memory + SLOT_B, candidate, 600U) == 0);
    CHECK(Firmware_Abort(&update));
    CHECK(update.state == FIRMWARE_IDLE);
}

static void test_capacity_and_arguments(void)
{
    unsigned before;
    create_baseline(4096U, 512U);
    before = erase_calls;
    CHECK(!Firmware_Begin(NULL, 600U, 0U));
    CHECK(!Firmware_Begin(&update, 0U, 0U));
    CHECK(!Firmware_Begin(&update, 4097U, 0U));
    CHECK(erase_calls == before);
    CHECK(!Firmware_Write(&update, candidate, 10U));
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(!Firmware_Write(NULL, candidate, 10U));
    CHECK(!Firmware_Write(&update, NULL, 10U));
    CHECK(!Firmware_Write(&update, candidate, 0U));
    CHECK(update.received_size == 0U);
    CHECK(!Firmware_Finish(&update)); /* Short body cannot become valid. */
    CHECK(Firmware_Abort(&update));
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(!Firmware_Write(&update, candidate, 601U));
    CHECK(update.received_size == 0U);
    CHECK(Firmware_Abort(&update));
    CHECK(!Firmware_Finish(NULL));
    CHECK(!Firmware_Abort(NULL));
    CHECK(!Firmware_ReadMetadata(NULL));
}

static void test_crc_failure(void)
{
    firmware_metadata_t meta;
    create_baseline(sizeof(app_memory), 5000U);
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U) ^ 1U));
    CHECK(Firmware_Write(&update, candidate, 600U));
    CHECK(!Firmware_Finish(&update));
    CHECK(update.state == FIRMWARE_ERROR);
    CHECK(Firmware_ReadMetadata(&meta));
    CHECK(meta.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(meta.candidate_slot == FIRMWARE_SLOT_NONE);
    CHECK(meta.slot[1].valid == FIRMWARE_IMAGE_INVALID);
    CHECK(memcmp(app_memory, flash_memory + SLOT_A, 5000U) == 0);
    CHECK(Firmware_Abort(&update));
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(Firmware_Write(&update, candidate, 600U));
    CHECK(Firmware_Finish(&update));
}

static void test_partial_body_failure(void)
{
    firmware_metadata_t meta;
    create_baseline(sizeof(app_memory), 5000U);
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    fail_write_call = write_calls + 1U;
    partial_write_length = 257U;
    CHECK(!Firmware_Write(&update, candidate, 600U));
    CHECK(update.state == FIRMWARE_ERROR && update.received_size == 0U);
    CHECK(memcmp(flash_memory + SLOT_B, candidate, 257U) == 0);
    CHECK(flash_memory[SLOT_B + 257U] == 0xFFU);
    CHECK(!Firmware_Finish(&update));
    CHECK(Firmware_ReadMetadata(&meta));
    CHECK(meta.slot[1].valid == FIRMWARE_IMAGE_INVALID);
    CHECK(meta.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(memcmp(app_memory, flash_memory + SLOT_A, 5000U) == 0);
    CHECK(Firmware_Abort(&update));
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(Firmware_Write(&update, candidate, 600U));
    CHECK(Firmware_Finish(&update));
}

static void test_metadata_read_failure(void)
{
    firmware_metadata_t out;
    firmware_metadata_t sentinel;
    unsigned before;
    CHECK(Firmware_SetAppRegion(app_memory, sizeof(app_memory)));
    memset(&out, 0xA5, sizeof(out));
    sentinel = out;
    fail_read_call = read_calls + 1U;
    CHECK(!Firmware_ReadMetadata(&out));
    CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0);
    fail_read_call = read_calls + 1U;
    CHECK(!Firmware_CreateBaseline(5000U));
    CHECK(erase_calls == 0U && write_calls == 0U);
    CHECK(Firmware_CreateBaseline(5000U));
    before = erase_calls;
    fail_read_call = read_calls + 2U; /* Failure in the other metadata copy. */
    CHECK(!Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(erase_calls == before);
    CHECK(Firmware_ReadMetadata(&out));
    CHECK(out.confirmed_slot == FIRMWARE_SLOT_A);
}

static void test_begin_metadata_failure(int failure_stage)
{
    firmware_metadata_t old, after;
    unsigned before;
    create_baseline(sizeof(app_memory), 5000U);
    CHECK(Firmware_ReadMetadata(&old));
    before = erase_calls;
    if (failure_stage == 0) {
        fail_erase_call = erase_calls + 1U;
    } else {
        fail_meta_write_length = failure_stage == 1 ? 56U : 4U;
        partial_write_length = failure_stage == 1 ? 13U : 2U;
    }
    CHECK(!Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(update.state == FIRMWARE_ERROR);
    CHECK(Firmware_ReadMetadata(&after));
    CHECK(after.generation == old.generation);
    CHECK(after.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(after.slot[1].valid == FIRMWARE_IMAGE_INVALID);
    verify_no_slot_erase(before, SLOT_A);
    verify_no_slot_erase(before, SLOT_B);
    CHECK(memcmp(app_memory, flash_memory + SLOT_A, 5000U) == 0);
    CHECK(Firmware_Abort(&update));
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(Firmware_Abort(&update));
}

static void test_finish_commit_failure(void)
{
    firmware_metadata_t before, after;
    create_baseline(sizeof(app_memory), 5000U);
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(Firmware_Write(&update, candidate, 600U));
    CHECK(Firmware_ReadMetadata(&before));
    fail_meta_write_length = 4U;
    partial_write_length = 2U;
    CHECK(!Firmware_Finish(&update));
    CHECK(Firmware_ReadMetadata(&after));
    CHECK(after.generation == before.generation);
    CHECK(after.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(after.candidate_slot == FIRMWARE_SLOT_NONE);
    CHECK(after.boot_phase == FIRMWARE_BOOT_NORMAL);
    CHECK(memcmp(flash_memory + SLOT_B, candidate, 600U) == 0);
    CHECK(memcmp(app_memory, flash_memory + SLOT_A, 5000U) == 0);
    CHECK(Firmware_Abort(&update));
}

static void test_finish_commit_readback_failure(void)
{
    firmware_metadata_t after;
    unsigned before;
    create_baseline(sizeof(app_memory), 5000U);
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(Firmware_Write(&update, candidate, 600U));
    committed_address = UINT32_MAX;
    fail_after_commit = 1;
    CHECK(!Firmware_Finish(&update));
    CHECK(Firmware_ReadMetadata(&after));
    CHECK(after.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(after.candidate_slot == FIRMWARE_SLOT_B);
    CHECK(after.boot_phase == FIRMWARE_BOOT_PENDING);
    before = erase_calls;
    CHECK(!Firmware_Abort(&update));
    CHECK(!Firmware_Begin(&update, 900U, model_crc32(candidate, 900U)));
    CHECK(erase_calls == before); /* Valid candidate cannot be erased by retry. */
    CHECK(!Firmware_Confirm());
}

static void test_generation_wrap(void)
{
    firmware_metadata_t older, newer, out;
    CHECK(Firmware_SetAppRegion(app_memory, sizeof(app_memory)));
    memset(&older, 0, sizeof(older));
    older.generation = UINT32_MAX;
    older.slot[0].firmware_size = 512U;
    older.slot[0].firmware_crc32 = model_crc32(app_memory, 512U);
    older.slot[0].valid = FIRMWARE_IMAGE_VALID;
    older.confirmed_slot = FIRMWARE_SLOT_A;
    older.candidate_slot = FIRMWARE_SLOT_NONE;
    older.boot_phase = FIRMWARE_BOOT_NORMAL;
    memcpy(flash_memory + SLOT_A, app_memory, 512U);
    model_store_metadata(META0, &older);
    newer = older;
    newer.generation = 0U;
    newer.slot[1].firmware_size = 600U;
    newer.slot[1].firmware_crc32 = model_crc32(candidate, 600U);
    newer.slot[1].valid = FIRMWARE_IMAGE_VALID;
    newer.confirmed_slot = FIRMWARE_SLOT_B;
    memcpy(flash_memory + SLOT_B, candidate, 600U);
    model_store_metadata(META1, &newer);
    CHECK(Firmware_ReadMetadata(&out));
    CHECK(out.generation == 0U && out.confirmed_slot == FIRMWARE_SLOT_B);
    flash_memory[META1 + 52U] ^= 1U; /* Newer record torn/corrupted. */
    CHECK(Firmware_ReadMetadata(&out));
    CHECK(out.generation == UINT32_MAX && out.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(!Firmware_CreateBaseline(512U));
}

static void test_baseline_partial_failure(void)
{
    firmware_metadata_t meta;
    CHECK(Firmware_SetAppRegion(app_memory, sizeof(app_memory)));
    fail_write_call = 1U;
    partial_write_length = 17U;
    CHECK(!Firmware_CreateBaseline(5000U));
    CHECK(!Firmware_ReadMetadata(&meta));
    CHECK(!Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(Firmware_CreateBaseline(5000U));
    CHECK(Firmware_ReadMetadata(&meta));
    CHECK(meta.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(memcmp(app_memory, flash_memory + SLOT_A, 5000U) == 0);
}

static void test_ignored_metadata_erase(void)
{
    firmware_metadata_t meta;
    unsigned before;
    create_baseline(sizeof(app_memory), 5000U);
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    CHECK(Firmware_Abort(&update));
    CHECK(Firmware_ReadMetadata(&meta));
    ignore_erase_address = other_metadata_address(&meta);
    before = erase_calls;
    CHECK(!Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    verify_no_slot_erase(before, SLOT_B);
    CHECK(Firmware_ReadMetadata(&meta));
    CHECK(meta.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(Firmware_Abort(&update));
}

static void test_ignored_body_write(void)
{
    firmware_metadata_t meta;
    create_baseline(sizeof(app_memory), 5000U);
    CHECK(Firmware_Begin(&update, 600U, model_crc32(candidate, 600U)));
    ignore_write_address = SLOT_B;
    CHECK(Firmware_Write(&update, candidate, 600U));
    CHECK(!Firmware_Finish(&update));
    CHECK(Firmware_ReadMetadata(&meta));
    CHECK(meta.slot[1].valid == FIRMWARE_IMAGE_INVALID);
    CHECK(meta.confirmed_slot == FIRMWARE_SLOT_A);
    CHECK(Firmware_Abort(&update));
}

typedef void (*test_fn_t)(void);
static void test_begin_erase_failure(void) { test_begin_metadata_failure(0); }
static void test_begin_precommit_failure(void) { test_begin_metadata_failure(1); }
static void test_begin_commit_failure(void) { test_begin_metadata_failure(2); }

static const struct {
    const char *name;
    test_fn_t run;
} cases[] = {
    {"uninitialized", test_uninitialized},
    {"baseline", test_baseline},
    {"cycle", test_cycle},
    {"capacity", test_capacity_and_arguments},
    {"crc", test_crc_failure},
    {"partial_body", test_partial_body_failure},
    {"read_failure", test_metadata_read_failure},
    {"begin_erase", test_begin_erase_failure},
    {"begin_precommit", test_begin_precommit_failure},
    {"begin_commit", test_begin_commit_failure},
    {"finish_commit", test_finish_commit_failure},
    {"finish_readback", test_finish_commit_readback_failure},
    {"generation_wrap", test_generation_wrap},
    {"baseline_partial", test_baseline_partial_failure},
    {"ignored_meta_erase", test_ignored_metadata_erase},
    {"ignored_body_write", test_ignored_body_write}
};

int main(int argc, char **argv)
{
    unsigned i;
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            puts(cases[i].name);
        }
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: test_firmware_ab CASE or --list\n");
        return 2;
    }
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            initialize_model();
            cases[i].run();
            printf("PASS %s (%u checks, NOR reads=%u writes=%u erases=%u)\n",
                   cases[i].name, checks, read_calls, write_calls, erase_calls);
            return 0;
        }
    }
    fprintf(stderr, "Unknown test case: %s\n", argv[1]);
    return 2;
}
