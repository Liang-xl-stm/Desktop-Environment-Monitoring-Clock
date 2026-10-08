/** Actual picture store against a NOR model: faults must never damage firmware. */
#include "picture_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[0x1000000], image[48000], loaded[48000];
static unsigned erases, writes, reads, fail_erase, fail_write, fail_read, fail_read_at, ignore_erase;
static uint32_t crc32(const uint8_t *p, unsigned n)
{
    uint32_t crc = 0xFFFFFFFFU; unsigned i;
    while (n--) { crc ^= *p++; for (i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0); }
    return crc ^ 0xFFFFFFFFU;
}
uint8_t W25Q128_Read(uint8_t *p, uint32_t addr, uint32_t n)
{
    assert(p && addr >= 0x300000 && n <= sizeof(flash) - addr);
    if (++reads == fail_read_at) return 0;
    if (fail_read) { fail_read = 0; return 0; }
    memcpy(p, flash + addr, n); return 1;
}
uint8_t W25Q128_Write(const uint8_t *p, uint32_t addr, uint32_t n)
{
    unsigned i; uint8_t success = 1;
    assert(p && addr >= 0x300000 && n <= sizeof(flash) - addr); ++writes;
    if (fail_write == writes) { n /= 2; success = 0; }
    for (i = 0; i < n; ++i) flash[addr + i] &= p[i];
    return success;
}
uint8_t W25Q128_Erase_Sector(uint32_t addr)
{
    assert(addr >= 0x300000 && addr + 4096 <= sizeof(flash) && addr % 4096 == 0);
    if (++erases == fail_erase) return 0;
    if (ignore_erase) { ignore_erase = 0; return 1; }
    memset(flash + addr, 0xFF, 4096); return 1;
}
static void upload(picture_write_t *ctx, unsigned slot)
{
    unsigned offset;
    memset(ctx, 0, sizeof(*ctx));
    assert(Picture_Begin(ctx, slot, crc32(image, sizeof(image))));
    for (offset = 0; offset < sizeof(image); offset += 256)
    {
        unsigned n = sizeof(image) - offset; if (n > 256) n = 256;
        assert(Picture_Write(ctx, image + offset, n));
    }
    assert(Picture_Finish(ctx));
    assert(Picture_Read(slot, loaded, sizeof(loaded)) && memcmp(loaded, image, sizeof(image)) == 0);
}
int main(void)
{
    picture_write_t ctx = {0}; unsigned i, before;
    memset(flash, 0xFF, sizeof(flash)); memset(flash, 0xA5, 0x300000);
    for (i = 0; i < sizeof(image); ++i) image[i] = (uint8_t)(i % 251);
    assert(PICTURE_SLOT_COUNT == 256 && PICTURE_SLOT_SIZE * 256U == 13U * 1024U * 1024U);
    assert(!Picture_Read(0, loaded, sizeof(loaded)));
    upload(&ctx, 0); upload(&ctx, 255);
    before = erases;
    assert(!Picture_Begin(&ctx, 256, 0) && erases == before);
    assert(!Picture_Read(0, loaded, 47999));
    assert(!Picture_Read(256, loaded, sizeof(loaded)));
    assert(Picture_Begin(&ctx, 1, crc32(image, sizeof(image))));
    before = erases;
    assert(!Picture_Begin(&ctx, 2, 0) && erases == before);
    assert(!Picture_Finish(&ctx));
    assert(!Picture_Read(1, loaded, sizeof(loaded))); /* No commit for interrupted body. */
    Picture_Abort(&ctx);
    assert(Picture_Begin(&ctx, 1, 0));
    before = writes;
    assert(!Picture_Write(&ctx, image, 48001) && writes == before);
    Picture_Abort(&ctx);
    assert(Picture_Begin(&ctx, 1, 0));
    assert(Picture_Write(&ctx, image, sizeof(image)));
    assert(!Picture_Finish(&ctx) && !Picture_Read(1, loaded, sizeof(loaded)));
    /* Ignored erase must leave old picture intact and stop before body erase. */
    memset(&ctx, 0, sizeof(ctx)); ignore_erase = 1; before = erases;
    assert(!Picture_Begin(&ctx, 0, 0) && erases == before + 1);
    assert(ctx.error != NULL && strcmp(ctx.error, "META_NOT_ERASED") == 0);
    assert(ctx.error_address == PICTURE_STORAGE_ADDR);
    assert(Picture_Read(0, loaded, sizeof(loaded)));
    memset(&ctx, 0, sizeof(ctx)); fail_erase = erases + 2;
    assert(!Picture_Begin(&ctx, 2, 0)); fail_erase = 0;
    assert(ctx.error != NULL && strcmp(ctx.error, "BODY_ERASE") == 0);
    assert(ctx.error_address == PICTURE_STORAGE_ADDR + 2 * PICTURE_SLOT_SIZE + 4096);
    memset(&ctx, 0, sizeof(ctx));
    assert(Picture_Begin(&ctx, 2, crc32(image, sizeof(image))));
    fail_write = writes + 1;
    assert(!Picture_Write(&ctx, image, sizeof(image)));
    assert(!Picture_Read(2, loaded, sizeof(loaded))); fail_write = 0;
    memset(&ctx, 0, sizeof(ctx));
    assert(Picture_Begin(&ctx, 2, crc32(image, sizeof(image))));
    assert(Picture_Write(&ctx, image, sizeof(image)));
    fail_write = writes + 2; /* Partial valid-marker programming. */
    assert(!Picture_Finish(&ctx)); fail_write = 0;
    assert(!Picture_Read(2, loaded, sizeof(loaded)));
    upload(&ctx, 2);
    /* Commit can succeed even if the final readback fails: preserve the valid image. */
    memset(&ctx, 0, sizeof(ctx));
    assert(Picture_Begin(&ctx, 3, crc32(image, sizeof(image))));
    assert(Picture_Write(&ctx, image, sizeof(image)));
    fail_read_at = reads + 190; /* 188 body chunks + metadata verify + final record. */
    assert(!Picture_Finish(&ctx)); fail_read_at = 0;
    assert(Picture_Read(3, loaded, sizeof(loaded)) && memcmp(loaded, image, sizeof(image)) == 0);
    flash[PICTURE_STORAGE_ADDR + 2 * PICTURE_SLOT_SIZE + 4096] ^= 1;
    assert(!Picture_Read(2, loaded, sizeof(loaded)));
    fail_read = 1; assert(!Picture_Read(0, loaded, sizeof(loaded)));
    for (i = 0; i < 0x300000; ++i) assert(flash[i] == 0xA5);
    puts("PASS picture store: 256 slots, firmware isolation, CRC, interrupted writes and faults");
    return 0;
}
