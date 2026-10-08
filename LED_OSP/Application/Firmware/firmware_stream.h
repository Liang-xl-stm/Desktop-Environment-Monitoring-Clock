/**
 * @file firmware_stream.h
 * @brief 将连续 TCP 字节流交给 A/B 固件升级服务的解析接口。
 * @details 本模块不依赖 TCP 库或 FreeRTOS；升级任务按收到的先后顺序调用 Feed。
 *          数据格式为 16 字节小端包头 + 原始 .bin 正文，每个对象只处理一个包。
 * @note 先完成 App 区域注册和首次回滚基线，本模块不会自动创建基线。
 */
#ifndef FIRMWARE_STREAM_H
#define FIRMWARE_STREAM_H

#include "firmware_update.h"
#include <stdint.h>

/** @brief 包头检查通过、目标槽准备完成，可以通知电脑开始发送正文。 */
#define FIRMWARE_STREAM_EVENT_READY    0x01U
/** @brief 本次至少有一批正文整批写入成功，可以读取 update.received_size。 */
#define FIRMWARE_STREAM_EVENT_PROGRESS 0x02U
/** @brief 正文收齐、CRC 校验通过并提交了 PENDING 候选记录。 */
#define FIRMWARE_STREAM_EVENT_DONE     0x04U
/** @brief 本次失败，或对终态对象继续 Feed；不能自动重开下载。 */
#define FIRMWARE_STREAM_EVENT_ERROR    0x80U

/** @brief TCP 字节流解析阶段，与底层升级状态分开。 */
typedef enum
{
    FIRMWARE_STREAM_HEADER = 0, /**< 累计接收 16 字节包头。 */
    FIRMWARE_STREAM_BODY,       /**< 已准备目标槽，顺序接收正文。 */
    FIRMWARE_STREAM_DONE,       /**< 候选已提交，不再接受数据。 */
    FIRMWARE_STREAM_FAILED      /**< 已失败或连接结束，不再接受数据。 */
} firmware_stream_stage_t;

/**
 * @brief 一个连接、一个升级包使用的解析对象。
 * @note 使用静态对象，确保底层 Firmware_Begin 持有的 update 地址持续有效。
 *       Feed/Stop 由同一个升级任务串行调用，不能在 TCP 回调中直接擦写 Flash。
 */
typedef struct
{
    firmware_update_t update;       /**< 底层 A/B 服务维护的下载状态。 */
    uint8_t header[16];             /**< 包头累计缓冲，只存 16 字节包头。 */
    uint32_t header_used;           /**< 已收集的包头字节数，范围 0～16。 */
    firmware_stream_stage_t stage;  /**< 当前解析阶段。 */
} firmware_stream_t;

/**
 * @brief 对解析对象进行一次性初始化。
 * @param[out] ctx 首次使用的对象，例如 static firmware_stream_t stream = {0};
 * @return 无；ctx 为 NULL 时不操作。
 * @pre 对象没有活跃升级会话，也未发布过本连接的候选包。
 * @note 只在第一次 Feed 之前调用；不能靠重新 Init 清除错误或覆盖活跃会话。
 *       本示例完成/失败后不自动复用对象，停止流程由 Stop 保守处理。
 */
void Firmware_Stream_Init(firmware_stream_t *ctx);

/**
 * @brief 按顺序输入本次从 TCP 收到的实际有效字节。
 * @param[in,out] ctx 当前连接持有的解析对象，成功后更新解析阶段及写入进度。
 * @param[in] data 本批数据起始地址，至少 length 字节有效；返回前不能被覆盖。
 * @param[in] length 本批实际有效字节数，必须大于 0，不是缓冲区容量。
 * @return READY/PROGRESS/DONE 的按位或；0 表示仍在等待包头剩余字节。
 *         任一错误返回 ERROR，不同时返回本次成功事件。
 * @note 允许一个 Feed 同时包含包头及正文，返回 READY|PROGRESS|DONE。
 *       正文会先检查整批是否超过剩余长度，超量时本批一字节也不写。
 *       DONE/FAILED 后继续 Feed 只返回 ERROR，保持终态，不开启第二个包。
 */
uint8_t Firmware_Stream_Feed(firmware_stream_t *ctx,
                              const uint8_t *data,
                              uint32_t length);

/**
 * @brief 接收失败或连接断开后，清理未发布候选的下载会话。
 * @param[in,out] ctx 当前解析对象；HEADER/BODY 结束后变为 FAILED。
 * @retval 1 没有需要清理的活跃下载，或 Firmware_Abort 已清理成功。
 * @retval 0 参数错误，或底层拒绝清理，例如候选已提交但最终读回曾失败。
 * @note 仅底层状态为 RECEIVING/ERROR 时调用 Abort，不取消已提交的候选。
 *       DONE 保持 DONE，FAILED 保持 FAILED；本函数不重开连接或下载。
 *       返回 0 时保留底层状态，调用方应报告错误，不能重新 Init 强行覆盖。
 */
uint8_t Firmware_Stream_Stop(firmware_stream_t *ctx);

#endif /* FIRMWARE_STREAM_H */
