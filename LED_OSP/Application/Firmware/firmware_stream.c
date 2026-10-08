/**
 * @file firmware_stream.c
 * @brief TCP 连续字节流包头累计及固件正文转交实现。
 * @note 不直接使用 TCP/RTOS，不自动创建基线，不负责 Bootloader 安装或回滚。
 */
#include "firmware_stream.h"
#include <stddef.h>
#include <string.h>

/**
 * @brief 从四个小端字节解析一个 32 位整数。
 * @param[in] data 至少四字节有效数据，不要求地址对齐。
 * @return 解析后的整数。
 */
static uint32_t stream_get_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/**
 * @brief 标记当前解析失败，不在这里擦除或盲目撤销持久化候选。
 * @param[in,out] ctx 当前解析对象，调用前必须非 NULL。
 * @return 固定返回 ERROR，停止处理本批后续数据。
 */
static uint8_t stream_fail(firmware_stream_t *ctx)
{
    ctx->stage = FIRMWARE_STREAM_FAILED;
    return FIRMWARE_STREAM_EVENT_ERROR;
}

/**
 * @brief 首次使用前初始化一个持续有效的解析对象。
 * @param[out] ctx 无活跃会话的新对象；NULL 时不操作。
 * @return 无。
 * @note 不能用于重置一个已有活跃下载或已发布候选的对象。
 */
void Firmware_Stream_Init(firmware_stream_t *ctx)
{
    if (ctx != NULL)
    {
        memset(ctx, 0, sizeof(*ctx));
        ctx->stage = FIRMWARE_STREAM_HEADER;
        ctx->update.target_slot = FIRMWARE_SLOT_NONE;
    }
}

/**
 * @brief 累计包头并把本次剩余正文交给 A/B 升级服务。
 * @param[in,out] ctx 当前连接的解析对象，由同一个升级任务调用。
 * @param[in] data 本批实际有效字节，函数返回前不允许被覆盖。
 * @param[in] length 本批有效字节数，必须非零。
 * @return 0 仅表示包头还未收齐；成功事件按位或；任何失败只返回 ERROR。
 */
uint8_t Firmware_Stream_Feed(firmware_stream_t *ctx,
                              const uint8_t *data,
                              uint32_t length)
{
    uint32_t used = 0U;
    uint32_t part;
    uint32_t body_length;
    uint32_t firmware_size;
    uint32_t firmware_crc32;
    uint8_t events = 0U;

    if (ctx == NULL)
    {
        return FIRMWARE_STREAM_EVENT_ERROR;
    }
    /* 终态不变，不能将紧随正文的另一个包悄悄当成新的升级。 */
    if (ctx->stage == FIRMWARE_STREAM_DONE ||
        ctx->stage == FIRMWARE_STREAM_FAILED)
    {
        return FIRMWARE_STREAM_EVENT_ERROR;
    }
    if (data == NULL || length == 0U)
    {
        return stream_fail(ctx);
    }

    if (ctx->stage == FIRMWARE_STREAM_HEADER)
    {
        if (ctx->header_used > FIRMWARE_HEADER_SIZE)
        {
            return stream_fail(ctx);
        }
        part = FIRMWARE_HEADER_SIZE - ctx->header_used;
        if (part > length)
        {
            part = length;
        }
        memcpy(ctx->header + ctx->header_used, data, part);
        ctx->header_used += part;
        used = part;
        if (ctx->header_used < FIRMWARE_HEADER_SIZE)
        {
            return 0U;
        }

        /* 不直接转换结构体指针，避免接收地址不对齐及大小端差异。 */
        if (stream_get_le32(ctx->header + 0U) != FIRMWARE_MAGIC ||
            stream_get_le32(ctx->header + 4U) != FIRMWARE_PROTOCOL_VERSION)
        {
            return stream_fail(ctx);
        }
        firmware_size = stream_get_le32(ctx->header + 8U);
        firmware_crc32 = stream_get_le32(ctx->header + 12U);
        if (!Firmware_Begin(&ctx->update, firmware_size, firmware_crc32))
        {
            return stream_fail(ctx);
        }
        ctx->stage = FIRMWARE_STREAM_BODY;
        events |= FIRMWARE_STREAM_EVENT_READY;
    }
    else if (ctx->stage != FIRMWARE_STREAM_BODY)
    {
        return stream_fail(ctx);
    }

    body_length = length - used;
    if (body_length == 0U)
    {
        /* 本次只送到包头末尾，通知上层目标 Flash 已经准备好。 */
        return events;
    }
    /* 先检查进度合法，再做减法，防止减法下溢与长度相加溢出。 */
    if (ctx->update.received_size > ctx->update.total_size ||
        body_length > ctx->update.total_size - ctx->update.received_size)
    {
        return stream_fail(ctx);
    }
    if (!Firmware_Write(&ctx->update, data + used, body_length))
    {
        return stream_fail(ctx);
    }
    events |= FIRMWARE_STREAM_EVENT_PROGRESS;

    if (ctx->update.received_size == ctx->update.total_size)
    {
        /* 数据收齐仍不能直接成功，必须读回 CRC 并提交候选记录。 */
        if (!Firmware_Finish(&ctx->update))
        {
            return stream_fail(ctx);
        }
        ctx->stage = FIRMWARE_STREAM_DONE;
        events |= FIRMWARE_STREAM_EVENT_DONE;
    }
    return events;
}

/**
 * @brief 断开或失败时尝试清理未发布的下载。
 * @param[in,out] ctx 当前解析对象；终态不自动重置。
 * @return 1 没有待清理会话或 Abort 成功；0 参数非法或 Abort 被底层拒绝。
 */
uint8_t Firmware_Stream_Stop(firmware_stream_t *ctx)
{
    if (ctx == NULL)
    {
        return 0U;
    }
    if (ctx->stage == FIRMWARE_STREAM_DONE)
    {
        /* 已提交候选包，连接关闭不能撤销候选，也不调用 Abort。 */
        return 1U;
    }
    ctx->stage = FIRMWARE_STREAM_FAILED;
    if (ctx->update.state == FIRMWARE_RECEIVING ||
        ctx->update.state == FIRMWARE_ERROR)
    {
        return Firmware_Abort(&ctx->update);
    }
    /* HEADER 阶段或 Begin 预检查失败，没有活跃的底层下载需要清理。 */
    return 1U;
}
