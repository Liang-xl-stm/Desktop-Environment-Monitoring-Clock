/* Host tests for the TCP byte-stream parser; no STM32 or physical Flash I/O. */
#include "firmware_stream.h"
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

static unsigned checks, begin_calls, write_calls, finish_calls, abort_calls;
static uint32_t begin_size, begin_crc, write_lengths[32], stored_size;
static uint8_t stored[4096];
static int begin_success, write_success, finish_success, abort_success;
static int fail_begin_without_session, publish_on_finish_failure, published;
static firmware_update_t *active_update;
static firmware_stream_t stream;

uint8_t Firmware_Begin(firmware_update_t *update,
                       uint32_t size, uint32_t crc)
{
    ++begin_calls;
    begin_size = size;
    begin_crc = crc;
    if (update == NULL || size == 0U || size > sizeof(stored)) {
        return 0U;
    }
    if (!begin_success && fail_begin_without_session) {
        return 0U;
    }
    active_update = update;
    update->target_slot = FIRMWARE_SLOT_B;
    update->storage_addr = FIRMWARE_SLOT_B_ADDR;
    update->total_size = size;
    update->received_size = 0U;
    update->expected_crc32 = crc;
    update->state = begin_success ? FIRMWARE_RECEIVING : FIRMWARE_ERROR;
    return begin_success ? 1U : 0U;
}

uint8_t Firmware_Write(firmware_update_t *update,
                       const uint8_t *data, uint32_t length)
{
    CHECK(write_calls < sizeof(write_lengths) / sizeof(write_lengths[0]));
    write_lengths[write_calls++] = length;
    CHECK(update != NULL && update == active_update);
    CHECK(update->state == FIRMWARE_RECEIVING);
    CHECK(data != NULL && length > 0U);
    CHECK(update->received_size <= update->total_size);
    CHECK(length <= update->total_size - update->received_size);
    CHECK(length <= sizeof(stored) - stored_size);
    if (!write_success) {
        /* Device writes may partially alter Flash before reporting failure. */
        stored[stored_size] = data[0];
        ++stored_size;
        update->state = FIRMWARE_ERROR;
        return 0U;
    }
    memcpy(stored + stored_size, data, length);
    stored_size += length;
    update->received_size += length;
    return 1U;
}

uint8_t Firmware_Finish(firmware_update_t *update)
{
    ++finish_calls;
    CHECK(update != NULL && update == active_update);
    CHECK(update->state == FIRMWARE_RECEIVING);
    CHECK(update->received_size == update->total_size);
    if (!finish_success) {
        update->state = FIRMWARE_ERROR;
        published = publish_on_finish_failure;
        return 0U;
    }
    update->state = FIRMWARE_READY;
    active_update = NULL;
    published = 1;
    return 1U;
}

uint8_t Firmware_Abort(firmware_update_t *update)
{
    ++abort_calls;
    if (update == NULL || update != active_update || !abort_success || published) {
        return 0U;
    }
    CHECK(update->state == FIRMWARE_RECEIVING || update->state == FIRMWARE_ERROR);
    memset(update, 0, sizeof(*update));
    update->target_slot = FIRMWARE_SLOT_NONE;
    update->state = FIRMWARE_IDLE;
    active_update = NULL;
    return 1U;
}

static void put_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void header(uint8_t *bytes, uint32_t size, uint32_t crc)
{
    put_le32(bytes, 0x50555746UL);
    put_le32(bytes + 4U, 1U);
    put_le32(bytes + 8U, size);
    put_le32(bytes + 12U, crc);
}

static void fixture(void)
{
    begin_calls = write_calls = finish_calls = abort_calls = 0U;
    begin_size = begin_crc = stored_size = 0U;
    memset(stored, 0xFF, sizeof(stored));
    memset(write_lengths, 0, sizeof(write_lengths));
    begin_success = write_success = finish_success = abort_success = 1;
    fail_begin_without_session = publish_on_finish_failure = published = 0;
    active_update = NULL;
    Firmware_Stream_Init(&stream);
    CHECK(stream.stage == FIRMWARE_STREAM_HEADER);
    CHECK(stream.header_used == 0U);
}

static void assert_halted(void)
{
    uint8_t byte = 0x19U;
    unsigned old_begin = begin_calls;
    unsigned old_write = write_calls;
    unsigned old_finish = finish_calls;
    CHECK(Firmware_Stream_Feed(&stream, &byte, 1U) == FIRMWARE_STREAM_EVENT_ERROR);
    CHECK(begin_calls == old_begin && write_calls == old_write &&
          finish_calls == old_finish);
}

static void test_header_one_byte_at_a_time(void)
{
    uint8_t bytes[16];
    const uint8_t body[] = {0x00U, 0xFFU, 0x11U, 0x22U, 0x80U, 0x01U};
    unsigned i;
    fixture();
    header(bytes, sizeof(body), 0x1234ABCDUL);
    for (i = 0U; i < 15U; ++i) {
        CHECK(Firmware_Stream_Feed(&stream, bytes + i, 1U) == 0U);
        CHECK(begin_calls == 0U && write_calls == 0U);
        CHECK(stream.header_used == i + 1U);
    }
    CHECK(Firmware_Stream_Feed(&stream, bytes + 15U, 1U) ==
          FIRMWARE_STREAM_EVENT_READY);
    CHECK(begin_calls == 1U && begin_size == sizeof(body));
    CHECK(begin_crc == 0x1234ABCDUL);
    CHECK(stream.stage == FIRMWARE_STREAM_BODY && write_calls == 0U);
    CHECK(Firmware_Stream_Feed(&stream, body, sizeof(body)) ==
          (FIRMWARE_STREAM_EVENT_PROGRESS | FIRMWARE_STREAM_EVENT_DONE));
    CHECK(write_calls == 1U && write_lengths[0] == sizeof(body));
    CHECK(finish_calls == 1U && published);
    CHECK(memcmp(stored, body, sizeof(body)) == 0);
    CHECK(Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 0U); /* Completed candidates must not be cancelled. */
}

static void test_whole_header_and_split_body(void)
{
    uint8_t bytes[16];
    const uint8_t body[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    fixture();
    header(bytes, sizeof(body), 0xDDEEFF01UL);
    CHECK(Firmware_Stream_Feed(&stream, bytes, sizeof(bytes)) ==
          FIRMWARE_STREAM_EVENT_READY);
    CHECK(begin_calls == 1U && write_calls == 0U && finish_calls == 0U);
    CHECK(Firmware_Stream_Feed(&stream, body, 2U) == FIRMWARE_STREAM_EVENT_PROGRESS);
    CHECK(Firmware_Stream_Feed(&stream, body + 2U, 3U) ==
          FIRMWARE_STREAM_EVENT_PROGRESS);
    CHECK(Firmware_Stream_Feed(&stream, body + 5U, 4U) ==
          (FIRMWARE_STREAM_EVENT_PROGRESS | FIRMWARE_STREAM_EVENT_DONE));
    CHECK(write_calls == 3U && write_lengths[0] == 2U &&
          write_lengths[1] == 3U && write_lengths[2] == 4U);
    CHECK(stream.update.received_size == sizeof(body));
    CHECK(finish_calls == 1U && stored_size == sizeof(body));
    CHECK(memcmp(stored, body, sizeof(body)) == 0);
    assert_halted(); /* Extra bytes after DONE cannot start another update. */
    CHECK(published && abort_calls == 0U);
    CHECK(Firmware_Stream_Stop(&stream));
    CHECK(published && abort_calls == 0U);
}

static void test_header_and_body_in_one_call(void)
{
    uint8_t packet[16U + 5U];
    const uint8_t body[] = {0x80U, 0x00U, 0xFFU, 0x63U, 0x33U};
    fixture();
    header(packet, sizeof(body), 0xA0B0C0D0UL);
    memcpy(packet + 16U, body, sizeof(body));
    CHECK(Firmware_Stream_Feed(&stream, packet, sizeof(packet)) ==
          (FIRMWARE_STREAM_EVENT_READY | FIRMWARE_STREAM_EVENT_PROGRESS |
           FIRMWARE_STREAM_EVENT_DONE));
    CHECK(begin_calls == 1U && write_calls == 1U && finish_calls == 1U);
    CHECK(write_lengths[0] == sizeof(body));
    CHECK(memcmp(stored, body, sizeof(body)) == 0);
}

static void test_header_with_partial_body(void)
{
    uint8_t packet[19];
    const uint8_t body[] = {0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U};
    fixture();
    header(packet, sizeof(body), 0xDEADBEEFUL);
    memcpy(packet + 16U, body, 3U);
    CHECK(Firmware_Stream_Feed(&stream, packet, sizeof(packet)) ==
          (FIRMWARE_STREAM_EVENT_READY | FIRMWARE_STREAM_EVENT_PROGRESS));
    CHECK(write_calls == 1U && write_lengths[0] == 3U && finish_calls == 0U);
    CHECK(Firmware_Stream_Feed(&stream, body + 3U, 2U) ==
          (FIRMWARE_STREAM_EVENT_PROGRESS | FIRMWARE_STREAM_EVENT_DONE));
    CHECK(write_calls == 2U && write_lengths[1] == 2U && finish_calls == 1U);
    CHECK(memcmp(stored, body, sizeof(body)) == 0);
}

static void test_excess_body_rejected_before_write(void)
{
    uint8_t packet[16U + 6U];
    unsigned i;
    fixture();
    header(packet, 5U, 0x123U);
    for (i = 16U; i < sizeof(packet); ++i) {
        packet[i] = (uint8_t)i;
    }
    CHECK(Firmware_Stream_Feed(&stream, packet, sizeof(packet)) ==
          FIRMWARE_STREAM_EVENT_ERROR);
    CHECK(begin_calls == 1U && write_calls == 0U && finish_calls == 0U);
    CHECK(stream.update.received_size == 0U && !published);
    assert_halted();
    CHECK(Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 1U && stream.update.state == FIRMWARE_IDLE);

    fixture();
    header(packet, 5U, 0x123U);
    CHECK(Firmware_Stream_Feed(&stream, packet, 16U) == FIRMWARE_STREAM_EVENT_READY);
    CHECK(Firmware_Stream_Feed(&stream, packet + 16U, 2U) ==
          FIRMWARE_STREAM_EVENT_PROGRESS);
    CHECK(Firmware_Stream_Feed(&stream, packet + 16U, 4U) ==
          FIRMWARE_STREAM_EVENT_ERROR); /* Only 3 bytes remain. */
    CHECK(write_calls == 1U && stored_size == 2U && finish_calls == 0U);
    CHECK(Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 1U);
}

static void test_invalid_header(void)
{
    uint8_t bytes[16];
    unsigned kind;
    for (kind = 0U; kind < 3U; ++kind) {
        fixture();
        header(bytes, 10U, 0x000055AAUL);
        if (kind == 0U) {
            put_le32(bytes, 0xDEADBEEFUL);
        } else if (kind == 1U) {
            put_le32(bytes + 4U, 2U);
        } else {
            put_le32(bytes + 8U, 0U);
        }
        CHECK(Firmware_Stream_Feed(&stream, bytes, sizeof(bytes)) ==
              FIRMWARE_STREAM_EVENT_ERROR);
        /* Magic/version belong to the parser; size limits belong to Begin. */
        CHECK(begin_calls == (kind == 2U ? 1U : 0U));
        CHECK(write_calls == 0U && finish_calls == 0U);
        assert_halted();
        CHECK(Firmware_Stream_Stop(&stream));
        CHECK(abort_calls == 0U);
    }
}

static void test_begin_failure(void)
{
    uint8_t bytes[16];
    fixture();
    header(bytes, 10U, 0x12345678UL);
    begin_success = 0;
    CHECK(Firmware_Stream_Feed(&stream, bytes, sizeof(bytes)) ==
          FIRMWARE_STREAM_EVENT_ERROR);
    CHECK(begin_calls == 1U && write_calls == 0U && finish_calls == 0U);
    assert_halted();
    CHECK(Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 1U);

    fixture();
    begin_success = 0;
    fail_begin_without_session = 1;
    CHECK(Firmware_Stream_Feed(&stream, bytes, sizeof(bytes)) ==
          FIRMWARE_STREAM_EVENT_ERROR);
    CHECK(Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 0U); /* Early Begin rejection created no session. */
}

static void test_write_failure(void)
{
    uint8_t bytes[16];
    const uint8_t body[] = {1U, 2U, 3U, 4U};
    fixture();
    header(bytes, sizeof(body), 0x12345678UL);
    CHECK(Firmware_Stream_Feed(&stream, bytes, sizeof(bytes)) ==
          FIRMWARE_STREAM_EVENT_READY);
    write_success = 0;
    CHECK(Firmware_Stream_Feed(&stream, body, sizeof(body)) ==
          FIRMWARE_STREAM_EVENT_ERROR);
    CHECK(write_calls == 1U && finish_calls == 0U);
    CHECK(stream.update.received_size == 0U && stored_size == 1U);
    assert_halted();
    CHECK(Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 1U && !published);
}

static void test_finish_failure(void)
{
    uint8_t packet[20];
    fixture();
    header(packet, 4U, 0xABCDEFUL);
    memset(packet + 16U, 0xA5, 4U);
    finish_success = 0;
    CHECK(Firmware_Stream_Feed(&stream, packet, sizeof(packet)) ==
          FIRMWARE_STREAM_EVENT_ERROR);
    CHECK(begin_calls == 1U && write_calls == 1U && finish_calls == 1U);
    CHECK(stream.update.received_size == 4U && !published);
    assert_halted();
    CHECK(Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 1U && stream.update.state == FIRMWARE_IDLE);
}

static void test_finish_published_but_readback_failed(void)
{
    uint8_t packet[20];
    fixture();
    header(packet, 4U, 0xABCDEFUL);
    memset(packet + 16U, 0xA5, 4U);
    finish_success = 0;
    publish_on_finish_failure = 1;
    CHECK(Firmware_Stream_Feed(&stream, packet, sizeof(packet)) ==
          FIRMWARE_STREAM_EVENT_ERROR);
    CHECK(published && stream.update.state == FIRMWARE_ERROR);
    CHECK(!Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 1U && published);
    CHECK(stream.update.state == FIRMWARE_ERROR);
    assert_halted();
    CHECK(write_calls == 1U && finish_calls == 1U && published);
}

static void test_disconnect(void)
{
    uint8_t packet[20];
    fixture();
    header(packet, 20U, 0xAABBCCDDUL);
    CHECK(Firmware_Stream_Feed(&stream, packet, 7U) == 0U);
    CHECK(Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 0U && begin_calls == 0U);
    assert_halted();

    fixture();
    memset(packet + 16U, 0x81, 4U);
    CHECK(Firmware_Stream_Feed(&stream, packet, sizeof(packet)) ==
          (FIRMWARE_STREAM_EVENT_READY | FIRMWARE_STREAM_EVENT_PROGRESS));
    CHECK(stream.update.received_size == 4U && finish_calls == 0U);
    CHECK(Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 1U && stream.update.state == FIRMWARE_IDLE);
    assert_halted();

    fixture();
    CHECK(Firmware_Stream_Feed(&stream, packet, 16U) == FIRMWARE_STREAM_EVENT_READY);
    abort_success = 0;
    CHECK(!Firmware_Stream_Stop(&stream));
    CHECK(abort_calls == 1U && stream.update.state == FIRMWARE_RECEIVING);
    assert_halted();
}

static void test_invalid_arguments(void)
{
    uint8_t byte = 0U;
    fixture();
    CHECK(Firmware_Stream_Feed(NULL, &byte, 1U) == FIRMWARE_STREAM_EVENT_ERROR);
    CHECK(!Firmware_Stream_Stop(NULL));
    CHECK(Firmware_Stream_Feed(&stream, NULL, 1U) == FIRMWARE_STREAM_EVENT_ERROR);
    CHECK(begin_calls == 0U && write_calls == 0U && finish_calls == 0U);
}

int main(void)
{
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"byte_header", test_header_one_byte_at_a_time},
        {"split_body", test_whole_header_and_split_body},
        {"coalesced_packet", test_header_and_body_in_one_call},
        {"partial_coalesced", test_header_with_partial_body},
        {"excess_body", test_excess_body_rejected_before_write},
        {"invalid_header", test_invalid_header},
        {"begin_failure", test_begin_failure},
        {"write_failure", test_write_failure},
        {"finish_failure", test_finish_failure},
        {"published_readback_failure", test_finish_published_but_readback_failed},
        {"disconnect", test_disconnect},
        {"invalid_arguments", test_invalid_arguments}
    };
    unsigned i;
    for (i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        cases[i].run();
        printf("PASS %s\n", cases[i].name);
    }
    printf("PASS firmware_stream: %u checks in %u cases\n", checks,
           (unsigned)(sizeof(cases) / sizeof(cases[0])));
    return 0;
}
