/** @file firmware_tcp.h @brief 常驻服务器的单会话下载队列和非阻塞事件入口。 */
#ifndef FIRMWARE_TCP_H
#define FIRMWARE_TCP_H
#include <stdint.h>
#include "lwesp/lwesp.h"
/** @brief 创建唯一队列和 worker；先于启用 TCP 服务器调用。
 *  @param firmware_allowed 1 基线就绪允许 FWUP；0 仍允许 PIC1 和 INFO。
 *  @return 1 服务已就绪（含重复调用）；0 资源分配失败。
 *  @pre 调度器和 Flash 已运行，worker 独占下载解析/写入。
 */
uint8_t Firmware_TCP_Init(uint8_t firmware_allowed);
/** @param evt ACTIVE 回调事件。应用层只接纳一个发送者，其他连接非阻塞关闭。
 *  @note 递增 epoch 与 OPEN 项关联；旧会话清理完成才重新接纳连接。
 */
void Firmware_TCP_OnActive(lwesp_evt_t *evt);
/** @param evt RECV 回调事件。复制最多 256 字节/项并在回调内归还 pbuf。
 *  @note 项保存接收时 epoch，不保留库缓冲；不擦写 Flash、不阻塞。
 */
void Firmware_TCP_OnReceive(lwesp_evt_t *evt);
/** @param evt CLOSE 回调事件。只标记所属会话关闭，不重置对象、不撤销提交。 */
void Firmware_TCP_OnClosed(lwesp_evt_t *evt);
/** @param evt SEND 回调事件。通知发送结果，允许 worker 释放/复用持久应答缓冲。 */
void Firmware_TCP_OnSent(lwesp_evt_t *evt);
#endif
