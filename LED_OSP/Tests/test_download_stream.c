/** Protocol tests use the real firmware parser and mock storage operations. */
#include "download_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned picture_begins, picture_writes, firmware_begins, aborts, fail_finish;
static uint8_t body[48000];
uint8_t Picture_Begin(picture_write_t *ctx, uint32_t slot, uint32_t crc)
{ ++picture_begins; assert(slot == 7); ctx->slot = slot; ctx->expected_crc = crc; ctx->active = 1; ctx->received_size = 0; return 1; }
uint8_t Picture_Write(picture_write_t *ctx, const uint8_t *p, uint32_t n)
{ ++picture_writes; assert(ctx->received_size + n <= sizeof(body)); memcpy(body + ctx->received_size, p, n); ctx->received_size += n; return 1; }
uint8_t Picture_Finish(picture_write_t *ctx)
{ assert(ctx->received_size == sizeof(body)); ctx->active = 0; return (uint8_t)!fail_finish; }
void Picture_Abort(picture_write_t *ctx) { ctx->active = 0; ++aborts; }
uint8_t Firmware_Begin(firmware_update_t *ctx, uint32_t n, uint32_t crc)
{ ++firmware_begins; assert(n == 3 && crc == 123); ctx->state = FIRMWARE_RECEIVING; ctx->total_size = n; ctx->received_size = 0; return 1; }
uint8_t Firmware_Write(firmware_update_t *ctx, const uint8_t *p, uint32_t n)
{ assert(memcmp(p, "abc" + ctx->received_size, n) == 0); ctx->received_size += n; return 1; }
uint8_t Firmware_Finish(firmware_update_t *ctx) { ctx->state = FIRMWARE_READY; return 1; }
uint8_t Firmware_Abort(firmware_update_t *ctx) { ctx->state = FIRMWARE_IDLE; ++aborts; return 1; }
static void put32(uint8_t *p, uint32_t v)
{ unsigned i; for (i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
int main(void)
{
    static uint8_t packet[48032]; download_stream_t ctx; unsigned i, before;
    Download_Stream_Init(&ctx, 0);
    assert(Download_Stream_Feed(&ctx, (const uint8_t *)"I", 1) == 0);
    assert(Download_Stream_Feed(&ctx, (const uint8_t *)"NF", 2) == 0);
    assert(Download_Stream_Feed(&ctx, (const uint8_t *)"O", 1) == DOWNLOAD_INFO);
    assert(ctx.type == DOWNLOAD_QUERY && ctx.stage == 2);
    assert(Download_Stream_Stop(&ctx) && !Download_Stream_NextPicture(&ctx));
    assert(picture_begins == 0 && firmware_begins == 0 && aborts == 0);
    assert(Download_Stream_NextRequest(&ctx));
    assert(Download_Stream_Feed(&ctx, (const uint8_t *)"INFOx", 5) == DOWNLOAD_ERROR);
    assert(!strcmp(ctx.error, "INFO_LENGTH") && Download_Stream_Stop(&ctx));
    assert(picture_begins == 0 && firmware_begins == 0 && aborts == 0);
    put32(packet, DOWNLOAD_PICTURE_MAGIC); put32(packet + 4, 1); put32(packet + 8, 7);
    put32(packet + 12, 800); put32(packet + 16, 480); put32(packet + 20, 48000);
    put32(packet + 24, 0x12345678); put32(packet + 28, 0);
    for (i = 32; i < sizeof(packet); ++i) packet[i] = (uint8_t)i;
    Download_Stream_Init(&ctx, 0); /* Firmware blocked, pictures still allowed. */
    for (i = 0; i < 31; ++i) assert(Download_Stream_Feed(&ctx, packet + i, 1) == 0);
    assert(Download_Stream_Feed(&ctx, packet + 31, 1) == DOWNLOAD_READY);
    assert(ctx.type == DOWNLOAD_PICTURE && ctx.total_size == 48000 && picture_begins == 1);
    assert(Download_Stream_Feed(&ctx, packet + 32, 255) == DOWNLOAD_PROGRESS);
    assert(Download_Stream_Feed(&ctx, packet + 287, 48000 - 255) == (DOWNLOAD_PROGRESS | DOWNLOAD_DONE));
    assert(memcmp(body, packet + 32, 48000) == 0 && ctx.received_size == 48000);
    assert(Download_Stream_Stop(&ctx) && aborts == 0); /* Committed picture not aborted. */
    assert(Download_Stream_NextPicture(&ctx));
    assert(Download_Stream_Feed(&ctx, packet, sizeof(packet)) == (DOWNLOAD_READY | DOWNLOAD_PROGRESS | DOWNLOAD_DONE));
    assert(Download_Stream_NextPicture(&ctx));
    before = picture_begins; put32(packet + 8, 256);
    assert(Download_Stream_Feed(&ctx, packet, 32) == DOWNLOAD_ERROR && picture_begins == before);
    assert(ctx.error != NULL && strcmp(ctx.error, "PIC_HEADER") == 0);
    put32(packet + 8, 7); put32(packet + 12, 480);
    Download_Stream_Init(&ctx, 1);
    assert(Download_Stream_Feed(&ctx, packet, 32) == DOWNLOAD_ERROR && picture_begins == before);
    put32(packet + 12, 800);
    Download_Stream_Init(&ctx, 1); fail_finish = 1;
    assert(Download_Stream_Feed(&ctx, packet, sizeof(packet)) == DOWNLOAD_ERROR);
    assert(!Download_Stream_NextPicture(&ctx)); fail_finish = 0;
    Download_Stream_Init(&ctx, 1);
    assert(Download_Stream_Feed(&ctx, packet, 32) == DOWNLOAD_READY);
    before = picture_writes;
    assert(Download_Stream_Feed(&ctx, packet, 48001) == DOWNLOAD_ERROR && picture_writes == before);
    assert(Download_Stream_Stop(&ctx) && aborts == 1);
    Download_Stream_Init(&ctx, 1); put32(packet, FIRMWARE_MAGIC);
    put32(packet + 4, 1); put32(packet + 8, 3); put32(packet + 12, 123); memcpy(packet + 16, "abc", 3);
    for (i = 0; i < 15; ++i) assert(Download_Stream_Feed(&ctx, packet + i, 1) == 0);
    assert(Download_Stream_Feed(&ctx, packet + 15, 4) == (DOWNLOAD_READY | DOWNLOAD_PROGRESS | DOWNLOAD_DONE));
    assert(ctx.type == DOWNLOAD_FIRMWARE && firmware_begins == 1);
    assert(!Download_Stream_NextPicture(&ctx));
    assert(Download_Stream_Stop(&ctx) && aborts == 1); /* Committed firmware not aborted. */
    Download_Stream_Init(&ctx, 0);
    assert(Download_Stream_Feed(&ctx, packet, 19) == DOWNLOAD_ERROR && firmware_begins == 1);
    put32(packet, 0xFFFFFFFFU); Download_Stream_Init(&ctx, 1);
    assert(Download_Stream_Feed(&ctx, packet, 4) == DOWNLOAD_ERROR);
    assert(ctx.error != NULL && strcmp(ctx.error, "HEADER_MAGIC") == 0);
    puts("PASS download dispatcher: split INFO without Flash, committed preservation, split/coalesced pictures and firmware gating");
    return 0;
}
