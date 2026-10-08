/** @file download_stream.c @brief 先识别包头，再转交相应存储服务。 */
#include "download_stream.h"
#include <stddef.h>
#include <string.h>
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint8_t fail(download_stream_t *ctx, const char *error)
{ ctx->error = error; ctx->stage = 3U; return DOWNLOAD_ERROR; }

void Download_Stream_Init(download_stream_t *ctx, uint8_t firmware_allowed)
{
    if (ctx == NULL) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->firmware_allowed = (uint8_t)(firmware_allowed != 0U);
    ctx->header_size = 4U;
    Firmware_Stream_Init(&ctx->firmware);
}

uint8_t Download_Stream_Feed(download_stream_t *ctx, const uint8_t *data, uint32_t length)
{
    uint32_t used = 0U, part;
    uint8_t events = 0U;
    if (ctx == NULL) return DOWNLOAD_ERROR;
    if (ctx->stage >= 2U) return fail(ctx, "SESSION_ENDED");
    if (data == NULL || length == 0U) return fail(ctx, "INPUT");

    while (ctx->stage == 0U)
    {
        part = ctx->header_size - ctx->header_used;
        if (part > length - used) part = length - used;
        memcpy(ctx->header + ctx->header_used, data + used, part);
        ctx->header_used += part; used += part;
        if (ctx->header_used < ctx->header_size) return 0U;
        if (ctx->type == DOWNLOAD_UNKNOWN)
        {
            uint32_t magic = get32(ctx->header);
            if (magic == DOWNLOAD_INFO_MAGIC)
            {
                if (used != length) return fail(ctx, "INFO_LENGTH");
                ctx->type = DOWNLOAD_QUERY;
                ctx->stage = 2U;
                return DOWNLOAD_INFO;
            }
            else if (magic == FIRMWARE_MAGIC && ctx->firmware_allowed)
            { ctx->type = DOWNLOAD_FIRMWARE; ctx->header_size = 16U; }
            else if (magic == DOWNLOAD_PICTURE_MAGIC)
            { ctx->type = DOWNLOAD_PICTURE; ctx->header_size = 32U; }
            else return fail(ctx, magic == FIRMWARE_MAGIC ? "FW_DISABLED" : "HEADER_MAGIC");
            continue;
        }
        if (ctx->type == DOWNLOAD_FIRMWARE)
        {
            events = Firmware_Stream_Feed(&ctx->firmware, ctx->header, 16U);
            if (events & DOWNLOAD_ERROR) return fail(ctx, "FW_PREPARE");
            ctx->total_size = ctx->firmware.update.total_size;
        }
        else
        {
            /* PIC1/version/slot/width/height/length/crc/reserved，均按小端解析。 */
            if (get32(ctx->header + 4) != 1U || get32(ctx->header + 8) >= PICTURE_SLOT_COUNT ||
                get32(ctx->header + 12) != PICTURE_WIDTH || get32(ctx->header + 16) != PICTURE_HEIGHT ||
                get32(ctx->header + 20) != PICTURE_DATA_SIZE || get32(ctx->header + 28) != 0U)
                return fail(ctx, "PIC_HEADER");
            if (!Picture_Begin(&ctx->picture, get32(ctx->header + 8), get32(ctx->header + 24)))
                return fail(ctx, "PIC_PREPARE");
            ctx->total_size = PICTURE_DATA_SIZE;
            events = DOWNLOAD_READY;
        }
        ctx->stage = 1U;
    }
    part = length - used;
    if (part != 0U)
    {
        if (ctx->type == DOWNLOAD_FIRMWARE)
        {
            uint8_t result = Firmware_Stream_Feed(&ctx->firmware, data + used, part);
            ctx->received_size = ctx->firmware.update.received_size;
            if (result & DOWNLOAD_ERROR) return fail(ctx, "FW_BODY");
            events |= result;
        }
        else
        {
            if (ctx->received_size > ctx->total_size || part > ctx->total_size - ctx->received_size)
                return fail(ctx, "PIC_LENGTH");
            if (!Picture_Write(&ctx->picture, data + used, part)) return fail(ctx, "PIC_WRITE");
            ctx->received_size = ctx->picture.received_size;
            events |= DOWNLOAD_PROGRESS;
            if (ctx->received_size == ctx->total_size)
            {
                if (!Picture_Finish(&ctx->picture)) return fail(ctx, "PIC_FINISH");
                events |= DOWNLOAD_DONE;
            }
        }
    }
    if (events & DOWNLOAD_DONE) ctx->stage = 2U;
    return events;
}

uint8_t Download_Stream_Stop(download_stream_t *ctx)
{
    if (ctx == NULL) return 0U;
    if (ctx->type == DOWNLOAD_FIRMWARE) return Firmware_Stream_Stop(&ctx->firmware);
    /* 查询/未知包头没有存储会话；已提交的图片也不执行取消接口。 */
    if (ctx->type == DOWNLOAD_PICTURE && ctx->stage != 2U)
        Picture_Abort(&ctx->picture);
    if (ctx->stage != 2U) ctx->stage = 3U;
    return 1U;
}

uint8_t Download_Stream_NextPicture(download_stream_t *ctx)
{
    if (ctx == NULL || ctx->type != DOWNLOAD_PICTURE || ctx->stage != 2U) return 0U;
    return Download_Stream_NextRequest(ctx);
}

uint8_t Download_Stream_NextRequest(download_stream_t *ctx)
{
    uint8_t allowed;
    if (ctx == NULL || ctx->stage != 2U ||
        (ctx->type != DOWNLOAD_PICTURE && ctx->type != DOWNLOAD_QUERY)) return 0U;
    allowed = ctx->firmware_allowed;
    Download_Stream_Init(ctx, allowed);
    return 1U;
}
