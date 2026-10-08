/** @file picture_store.c @brief 固定槽擦写、读回校验及最后提交有效标记。 */
#include "picture_store.h"
#include "w25q128.h"
#include <stddef.h>
#include <string.h>

#define PICTURE_META_MAGIC   0x53434950UL /* "PICS"，存储管理记录。 */
#define PICTURE_META_COMMIT  0xA55A5AA5UL
#define PICTURE_BODY_OFFSET  4096UL

static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t crc_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    uint8_t bit;
    while (n-- != 0U)
    {
        crc ^= *p++;
        for (bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
    }
    return crc;
}
static uint32_t slot_address(uint32_t slot)
{ return PICTURE_STORAGE_ADDR + slot * PICTURE_SLOT_SIZE; }
/** @brief 保存本次失败阶段和地址；不擦除数据，也不撤销已提交的图片。 */
static uint8_t picture_fail(picture_write_t *ctx, const char *error, uint32_t address)
{ ctx->error = error; ctx->error_address = address; return 0U; }

uint8_t Picture_Begin(picture_write_t *ctx, uint32_t slot, uint32_t crc)
{
    uint8_t check[256];
    uint32_t offset, i, address;
    if (ctx == NULL || ctx->active || slot >= PICTURE_SLOT_COUNT) return 0U;
    memset(ctx, 0, sizeof(*ctx));
    ctx->slot = slot; ctx->expected_crc = crc;
    address = slot_address(slot);
    /* 先使旧图失效，并验证整个管理扇区真的擦除，才允许破坏正文。 */
    if (!W25Q128_Erase_Sector(address)) return picture_fail(ctx, "META_ERASE", address);
    for (offset = 0U; offset < PICTURE_BODY_OFFSET; offset += sizeof(check))
    {
        if (!W25Q128_Read(check, address + offset, sizeof(check)))
            return picture_fail(ctx, "META_READ", address + offset);
        for (i = 0U; i < sizeof(check); ++i) if (check[i] != 0xFFU)
            return picture_fail(ctx, "META_NOT_ERASED", address + offset + i);
    }
    for (offset = PICTURE_BODY_OFFSET; offset < PICTURE_SLOT_SIZE; offset += W25Q128_SECTOR_SIZE)
        if (!W25Q128_Erase_Sector(address + offset))
            return picture_fail(ctx, "BODY_ERASE", address + offset);
    ctx->active = 1U;
    return 1U;
}

uint8_t Picture_Write(picture_write_t *ctx, const uint8_t *data, uint32_t length)
{
    if (ctx == NULL || !ctx->active || data == NULL || length == 0U ||
        ctx->slot >= PICTURE_SLOT_COUNT || ctx->received_size > PICTURE_DATA_SIZE ||
        length > PICTURE_DATA_SIZE - ctx->received_size) return 0U;
    if (!W25Q128_Write(data, slot_address(ctx->slot) + PICTURE_BODY_OFFSET + ctx->received_size, length))
    {
        ctx->active = 0U;
        return picture_fail(ctx, "BODY_WRITE", slot_address(ctx->slot) + PICTURE_BODY_OFFSET + ctx->received_size);
    }
    ctx->received_size += length;
    return 1U;
}

uint8_t Picture_Finish(picture_write_t *ctx)
{
    uint8_t buffer[256], metadata[32], check[32];
    uint32_t offset, count, crc = 0xFFFFFFFFUL, address;
    if (ctx == NULL || !ctx->active || ctx->slot >= PICTURE_SLOT_COUNT ||
        ctx->received_size != PICTURE_DATA_SIZE) return 0U;
    /* 本次会结束 active；任何失败都不能通过继续写入伪造完成。 */
    ctx->active = 0U;
    address = slot_address(ctx->slot);
    for (offset = 0U; offset < PICTURE_DATA_SIZE; offset += count)
    {
        count = PICTURE_DATA_SIZE - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        if (!W25Q128_Read(buffer, address + PICTURE_BODY_OFFSET + offset, count))
            return picture_fail(ctx, "BODY_READ", address + PICTURE_BODY_OFFSET + offset);
        crc = crc_update(crc, buffer, count);
    }
    if ((crc ^ 0xFFFFFFFFUL) != ctx->expected_crc)
        return picture_fail(ctx, "BODY_CRC", address + PICTURE_BODY_OFFSET);
    put32(metadata, PICTURE_META_MAGIC);
    put32(metadata + 4, 1U);
    put32(metadata + 8, ctx->slot);
    put32(metadata + 12, PICTURE_DATA_SIZE);
    put32(metadata + 16, ctx->expected_crc);
    put32(metadata + 20, PICTURE_WIDTH);
    put32(metadata + 24, PICTURE_HEIGHT);
    put32(metadata + 28, PICTURE_META_COMMIT);
    if (!W25Q128_Write(metadata, address, 28U)) return picture_fail(ctx, "META_WRITE", address);
    if (!W25Q128_Read(check, address, 28U)) return picture_fail(ctx, "META_VERIFY_READ", address);
    if (memcmp(metadata, check, 28U) != 0) return picture_fail(ctx, "META_VERIFY", address);
    /* 最后单独写有效标记，掉电或部分写入不能成为有效图片。 */
    if (!W25Q128_Write(metadata + 28, address + 28U, 4U))
        return picture_fail(ctx, "COMMIT_WRITE", address + 28U);
    if (!W25Q128_Read(check, address, sizeof(check)))
        return picture_fail(ctx, "COMMIT_READ", address);
    if (memcmp(metadata, check, sizeof(check)) != 0) return picture_fail(ctx, "COMMIT_VERIFY", address);
    return 1U;
}

void Picture_Abort(picture_write_t *ctx)
{ if (ctx != NULL) ctx->active = 0U; }

uint8_t Picture_Read(uint32_t slot, uint8_t *data, uint32_t capacity)
{
    uint8_t metadata[32];
    uint32_t address, crc;
    if (slot >= PICTURE_SLOT_COUNT || data == NULL || capacity < PICTURE_DATA_SIZE) return 0U;
    address = slot_address(slot);
    if (!W25Q128_Read(metadata, address, sizeof(metadata))) return 0U;
    if (get32(metadata) != PICTURE_META_MAGIC || get32(metadata + 4) != 1U ||
        get32(metadata + 8) != slot || get32(metadata + 12) != PICTURE_DATA_SIZE ||
        get32(metadata + 20) != PICTURE_WIDTH || get32(metadata + 24) != PICTURE_HEIGHT ||
        get32(metadata + 28) != PICTURE_META_COMMIT) return 0U;
    if (!W25Q128_Read(data, address + PICTURE_BODY_OFFSET, PICTURE_DATA_SIZE)) return 0U;
    crc = crc_update(0xFFFFFFFFUL, data, PICTURE_DATA_SIZE) ^ 0xFFFFFFFFUL;
    return (uint8_t)(crc == get32(metadata + 16));
}
