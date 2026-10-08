/**
 * @file firmware_tcp.c
 * @brief 常驻服务器的单发送者门禁、会话隔离及独立 Flash 下载任务。
 * @note 回调只复制数据/通知；解析对象和 Flash 仅由 FW_TCP 任务操作。
 */
#include "firmware_tcp.h"
#include "download_stream.h"
#include "epd_app.h"
#include "w25q128.h"
#include "spi.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "elog.h"
#include <stdio.h>
#include <string.h>

#define FW_TCP_BLOCK_SIZE  256U
#define FW_TCP_QUEUE_ITEMS 8U
#define FW_TCP_TIMEOUT_MS  30000U
typedef struct
{
    lwesp_conn_p conn;                 /**< 库可复用此地址，必须同时检查 epoch。 */
    uint32_t epoch;                    /**< 接受连接时分配的非零递增会话编号。 */
    uint16_t length;                   /**< 数据字节数；OPEN 项为零。 */
    uint8_t opening;                   /**< 1 通知初始化新会话，0 为数据副本。 */
    uint8_t data[FW_TCP_BLOCK_SIZE];   /**< 自有副本，回调返回后仍有效。 */
} firmware_tcp_item_t;
enum { FW_TCP_IDLE = 0, FW_TCP_RESERVED, FW_TCP_ACTIVE, FW_TCP_CLOSING, FW_TCP_BLOCKED };
enum { FW_TCP_FAULT_NONE = 0, FW_TCP_FAULT_QUEUE, FW_TCP_FAULT_COPY, FW_TCP_FAULT_CLOSED };
static QueueHandle_t fw_queue;
static TaskHandle_t fw_task;
static download_stream_t fw_stream;
static uint8_t fw_allowed;
/* 身份/通知字段都在 LwESP 核心锁内读写；库回调本身已经持有该递归锁。 */
static lwesp_conn_p fw_conn, fw_tx_conn;
static uint32_t fw_epoch, fw_counter, fw_state, fw_fault, fw_fault_epoch, fw_tx_epoch;
static uint8_t fw_tx_pending, fw_tx_ok;
static size_t fw_tx_length;
static char fw_reply[48]; /**< SEND 完成前绝不覆盖，也不把栈地址交给异步库。 */

/** @brief 在核心锁内验证身份。@param conn 原连接。@param epoch 原会话号。
 *  @return 1 仍为原会话且在线；0 已关闭、复用或门禁已释放。
 */
static uint8_t Firmware_TCP_Matches(lwesp_conn_p conn, uint32_t epoch)
{
    return (uint8_t)(conn != NULL && fw_conn == conn && fw_epoch == epoch &&
        fw_state != FW_TCP_IDLE && fw_state != FW_TCP_BLOCKED &&
        (uint32_t)(uintptr_t)lwesp_conn_get_arg(conn) == epoch && lwesp_conn_is_active(conn));
}

/**
 * @brief 短锁内核对身份并非阻塞提交；锁外等待 SEND 通知。
 * @param conn 原连接地址，不能仅凭地址判断连接未被库复用。
 * @param epoch 应答所属原会话号，由 worker 保存，不能从新连接推断。
 * @param text 零结尾 CRLF 行，提交前复制至静态缓冲。
 * @return 1 SEND 成功且长度完整；0 失效、提交失败、发送失败或等待超时。
 * @note 超时仍保留缓冲和 pending，结束会话时等待库释放发送后再开放门禁。
 */
static uint8_t Firmware_TCP_Reply(lwesp_conn_p conn, uint32_t epoch, const char *text)
{
    lwespr_t result;
    TickType_t began;
    uint8_t pending, ok;
    size_t length = strlen(text);
    if (length >= sizeof(fw_reply)) return 0U;
    lwesp_core_lock();
    if (fw_tx_pending || !Firmware_TCP_Matches(conn, epoch))
    { lwesp_core_unlock(); return 0U; }
    memcpy(fw_reply, text, length + 1U);
    fw_tx_conn = conn; fw_tx_epoch = epoch; fw_tx_length = length;
    fw_tx_pending = 1U; fw_tx_ok = 0U;
    /* NULL 不保留栈上的发送长度地址；0 表示非阻塞，核心锁内绝不等 AT/SEND。 */
    result = lwesp_conn_send(conn, fw_reply, length, NULL, 0U);
    if (result != lwespOK) fw_tx_pending = 0U;
    lwesp_core_unlock();
    if (result != lwespOK) return 0U;
    began = xTaskGetTickCount();
    for (;;)
    {
        lwesp_core_lock(); pending = fw_tx_pending; ok = fw_tx_ok; lwesp_core_unlock();
        if (!pending) return ok;
        if ((TickType_t)(xTaskGetTickCount() - began) >= pdMS_TO_TICKS(FW_TCP_TIMEOUT_MS)) return 0U;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
    }
}

/** @param conn 原连接。@param epoch 原会话号。@note 短锁内核对并异步关闭，库保存校验号。 */
static void Firmware_TCP_Close(lwesp_conn_p conn, uint32_t epoch)
{
    lwesp_core_lock();
    if (Firmware_TCP_Matches(conn, epoch)) (void)lwesp_conn_close(conn, 0U);
    lwesp_core_unlock();
}

/**
 * @brief worker 清理存储和旧队列，发送缓冲释放后才开放新连接。
 * @param conn 结束的原始连接。
 * @param epoch 结束的原始会话号。
 * @return 1 清理成功可重新待命；0 固件 Abort 失败，保留对象并安全阻止新下载。
 * @note 已提交数据不撤销；任何回调都不能初始化或清空 fw_stream。
 */
static uint8_t Firmware_TCP_End(lwesp_conn_p conn, uint32_t epoch)
{
    firmware_tcp_item_t discarded;
    uint8_t stopped, pending;
    lwesp_core_lock();
    if (fw_epoch == epoch) fw_state = FW_TCP_CLOSING;
    lwesp_core_unlock();
    stopped = Download_Stream_Stop(&fw_stream);
    Firmware_TCP_Close(conn, epoch);
    if (!stopped) elog_e("fw_tcp", "storage cleanup refused; protected, restart required");
    for (;;)
    {
        lwesp_core_lock(); pending = fw_tx_pending; lwesp_core_unlock();
        if (!pending) break;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
    }
    /* 门禁尚未开放，唯一生产者不能投递新会话；旧项不会占用新队列容量。 */
    while (xQueueReceive(fw_queue, &discarded, 0U) == pdPASS) {}
    lwesp_core_lock();
    if (fw_epoch == epoch)
    {
        if (conn != NULL && (uint32_t)(uintptr_t)lwesp_conn_get_arg(conn) == epoch)
            (void)lwesp_conn_set_arg(conn, NULL);
        fw_conn = NULL; fw_fault = FW_TCP_FAULT_NONE; fw_fault_epoch = 0U;
        fw_state = stopped ? FW_TCP_IDLE : FW_TCP_BLOCKED;
    }
    lwesp_core_unlock();
    return stopped;
}

/** @brief 失败时先复制 SPI 原始快照，后续日志/SR1 读取不会覆盖原故障。 */
static void Firmware_TCP_Diagnostic(void)
{
    const char *reason = fw_stream.error != NULL ? fw_stream.error : "UPDATE";
    const char *storage = fw_stream.picture.error != NULL ? fw_stream.picture.error : "";
    spi_diagnostic_t bus_fault = *SPI_GetLastDiagnostic();
    elog_e("fw_tcp", "download failed: %s %s, bytes=%lu, addr=%06lX",
           reason, storage, (unsigned long)fw_stream.received_size,
           (unsigned long)fw_stream.picture.error_address);
    if (fw_stream.received_size == 0U)
        elog_hexdump("download header", 16U, fw_stream.header, fw_stream.header_used);
    if (fw_stream.picture.error != NULL)
    {
        uint8_t status;
        elog_e("picture", "last SPI fault=%u phase=%lu SR=%04lX CR1=%04lX cycles=%lu DWT=%lu",
               (unsigned int)bus_fault.reason, (unsigned long)bus_fault.phase,
               (unsigned long)bus_fault.sr, (unsigned long)bus_fault.cr1,
               (unsigned long)bus_fault.elapsed_cycles, (unsigned long)bus_fault.counter_enabled);
        if (W25Q128_ReadStatus(&status)) elog_e("picture", "Flash SR1=%02X", (unsigned int)status);
    }
}

/** @brief 常驻 worker，按会话号消费事件，独占 Flash。@param argument 未使用，传 NULL。 */
static void Firmware_TCP_Task(void *argument)
{
    firmware_tcp_item_t item;
    lwesp_conn_p conn = NULL;
    uint32_t epoch = 0U, fault;
    TickType_t last_data = xTaskGetTickCount();
    uint8_t started = 0U, end_session;
    char reply[48];
    (void)argument;
    Download_Stream_Init(&fw_stream, fw_allowed);
    for (;;)
    {
        BaseType_t got = xQueueReceive(fw_queue, &item, pdMS_TO_TICKS(100U));
        if (got == pdPASS && item.opening)
        {
            lwesp_core_lock();
            end_session = (uint8_t)(fw_state == FW_TCP_RESERVED && fw_epoch == item.epoch);
            lwesp_core_unlock();
            if (epoch != 0U || !end_session) continue;
            /* 上一个会话成功 Stop 后才允许此 OPEN；仅由 worker 初始化。 */
            Download_Stream_Init(&fw_stream, fw_allowed);
            conn = item.conn; epoch = item.epoch; started = 0U; last_data = xTaskGetTickCount();
            lwesp_core_lock();
            if (fw_epoch == epoch && fw_state == FW_TCP_RESERVED) fw_state = FW_TCP_ACTIVE;
            lwesp_core_unlock();
        }
        if (epoch == 0U) continue;
        lwesp_core_lock();
        fault = fw_fault_epoch == epoch ? fw_fault : FW_TCP_FAULT_NONE;
        lwesp_core_unlock();
        if (fault != FW_TCP_FAULT_NONE)
        {
            if (fault != FW_TCP_FAULT_CLOSED)
                (void)Firmware_TCP_Reply(conn, epoch, "ERR TRANSPORT\r\n");
            (void)Firmware_TCP_End(conn, epoch); epoch = 0U; started = 0U;
            continue;
        }
        if (got == pdPASS && !item.opening && item.epoch == epoch && item.conn == conn)
        {
            uint8_t events;
            started = 1U; end_session = 0U;
            events = Download_Stream_Feed(&fw_stream, item.data, item.length);
            if (events & DOWNLOAD_ERROR)
            {
                const char *reason = fw_stream.error != NULL ? fw_stream.error : "UPDATE";
                const char *storage = fw_stream.picture.error != NULL ? fw_stream.picture.error : "";
                Firmware_TCP_Diagnostic();
                (void)snprintf(reply, sizeof(reply), "ERR %s %s\r\n", reason, storage);
                (void)Firmware_TCP_Reply(conn, epoch, reply); end_session = 1U;
            }
            else
            {
                if (events & DOWNLOAD_INFO)
                {
                    if (!Firmware_TCP_Reply(conn, epoch, "EPD1 800 480 256\r\n")) end_session = 1U;
                    else { (void)Download_Stream_NextRequest(&fw_stream); started = 0U; }
                }
                if ((events & DOWNLOAD_READY) && !Firmware_TCP_Reply(conn, epoch, "READY\r\n"))
                    end_session = 1U;
                if (!end_session && (events & DOWNLOAD_PROGRESS))
                {
                    (void)snprintf(reply, sizeof(reply), "ACK %lu\r\n", (unsigned long)fw_stream.received_size);
                    if (!Firmware_TCP_Reply(conn, epoch, reply)) end_session = 1U;
                }
                if (!end_session && (events & DOWNLOAD_DONE))
                {
                    if (fw_stream.type == DOWNLOAD_PICTURE)
                    {
                        elog_i("picture", "slot=%u stored, CRC OK", (unsigned int)fw_stream.picture.slot);
                        if (!EPD_ShowPicture((uint16_t)fw_stream.picture.slot))
                            elog_w("picture", "saved; screen task unavailable");
                    }
                    else elog_i("fw_tcp", "candidate committed, bytes=%lu; installation pending",
                                (unsigned long)fw_stream.received_size);
                    if (!Firmware_TCP_Reply(conn, epoch, "DONE\r\n")) end_session = 1U;
                    else if (fw_stream.type == DOWNLOAD_PICTURE)
                    { (void)Download_Stream_NextPicture(&fw_stream); started = 0U; }
                    else end_session = 1U;
                }
            }
            if (end_session) { (void)Firmware_TCP_End(conn, epoch); epoch = 0U; started = 0U; }
            last_data = xTaskGetTickCount();
        }
        else if (got != pdPASS && started &&
                 (TickType_t)(xTaskGetTickCount() - last_data) >= pdMS_TO_TICKS(FW_TCP_TIMEOUT_MS))
        {
            (void)Firmware_TCP_Reply(conn, epoch, "ERR TIMEOUT\r\n");
            (void)Firmware_TCP_End(conn, epoch); epoch = 0U; started = 0U;
        }
    }
}

uint8_t Firmware_TCP_Init(uint8_t firmware_allowed)
{
    if (fw_queue != NULL) return 1U; /* 重复调用不重复分配资源。 */
    fw_allowed = (uint8_t)(firmware_allowed != 0U);
    fw_queue = xQueueCreate(FW_TCP_QUEUE_ITEMS, sizeof(firmware_tcp_item_t));
    if (fw_queue == NULL) { elog_e("fw_tcp", "queue allocation failed"); return 0U; }
    if (xTaskCreate(Firmware_TCP_Task, "FW_TCP", 512U, NULL, 1U, &fw_task) != pdPASS)
    {
        vQueueDelete(fw_queue); fw_queue = NULL;
        elog_e("fw_tcp", "task creation failed"); return 0U;
    }
    return 1U;
}

void Firmware_TCP_OnActive(lwesp_evt_t *evt)
{
    firmware_tcp_item_t item;
    lwesp_conn_p conn = lwesp_evt_conn_active_get_conn(evt);
    if (conn == NULL) return;
    /* 旧 AT 固件可能忽略 CIPSERVERMAXCONN，必须应用层门禁。 */
    if (fw_queue == NULL || fw_state != FW_TCP_IDLE || fw_tx_pending)
    {
        (void)lwesp_conn_set_arg(conn, NULL); (void)lwesp_conn_close(conn, 0U); return;
    }
    if (++fw_counter == 0U) ++fw_counter;
    fw_conn = conn; fw_epoch = fw_counter; fw_state = FW_TCP_RESERVED;
    fw_fault = FW_TCP_FAULT_NONE; fw_fault_epoch = fw_epoch;
    (void)lwesp_conn_set_arg(conn, (void *)(uintptr_t)fw_epoch);
    memset(&item, 0, sizeof(item)); item.conn = conn; item.epoch = fw_epoch; item.opening = 1U;
    if (xQueueSend(fw_queue, &item, 0U) != pdPASS)
    {
        (void)lwesp_conn_set_arg(conn, NULL); (void)lwesp_conn_close(conn, 0U);
        fw_conn = NULL; fw_state = FW_TCP_IDLE;
    }
}

void Firmware_TCP_OnReceive(lwesp_evt_t *evt)
{
    firmware_tcp_item_t item;
    lwesp_pbuf_p pbuf = lwesp_evt_conn_recv_get_buff(evt);
    lwesp_conn_p conn = lwesp_evt_conn_recv_get_conn(evt);
    size_t total, offset = 0U, blocks;
    if (pbuf == NULL) return;
    if (fw_queue == NULL || conn != fw_conn ||
        (uint32_t)(uintptr_t)lwesp_conn_get_arg(conn) != fw_epoch ||
        (fw_state != FW_TCP_RESERVED && fw_state != FW_TCP_ACTIVE) ||
        fw_fault != FW_TCP_FAULT_NONE)
    { (void)lwesp_conn_recved(conn, pbuf); return; }
    total = lwesp_pbuf_length(pbuf, 1U);
    blocks = total / FW_TCP_BLOCK_SIZE + (total % FW_TCP_BLOCK_SIZE != 0U);
    if (blocks > uxQueueSpacesAvailable(fw_queue)) fw_fault = FW_TCP_FAULT_QUEUE;
    else while (offset < total)
    {
        size_t count = total - offset;
        if (count > FW_TCP_BLOCK_SIZE) count = FW_TCP_BLOCK_SIZE;
        item.conn = conn; item.epoch = fw_epoch; item.opening = 0U; item.length = (uint16_t)count;
        if (lwesp_pbuf_copy(pbuf, item.data, count, offset) != count)
        { fw_fault = FW_TCP_FAULT_COPY; break; }
        if (xQueueSend(fw_queue, &item, 0U) != pdPASS)
        { fw_fault = FW_TCP_FAULT_QUEUE; break; }
        offset += count;
    }
    if (fw_fault != FW_TCP_FAULT_NONE) fw_fault_epoch = fw_epoch;
    (void)lwesp_conn_recved(conn, pbuf); /* 必须在回调内归还；库随后释放 pbuf。 */
}

void Firmware_TCP_OnClosed(lwesp_evt_t *evt)
{
    lwesp_conn_p conn = lwesp_evt_conn_close_get_conn(evt);
    if (conn == fw_conn && conn != NULL &&
        (uint32_t)(uintptr_t)lwesp_conn_get_arg(conn) == fw_epoch)
    {
        fw_fault = FW_TCP_FAULT_CLOSED; fw_fault_epoch = fw_epoch;
        if (fw_task != NULL) (void)xTaskNotifyGive(fw_task);
    }
}

void Firmware_TCP_OnSent(lwesp_evt_t *evt)
{
    lwesp_conn_p conn = lwesp_evt_conn_send_get_conn(evt);
    /* 只由 worker 提交发送；旧 SEND 完成前不开放新会话，不从复用 arg 推断归属。 */
    if (fw_tx_pending && conn == fw_tx_conn && fw_tx_epoch == fw_epoch)
    {
        fw_tx_ok = (uint8_t)(lwesp_evt_conn_send_get_result(evt) == lwespOK &&
                            lwesp_evt_conn_send_get_length(evt) == fw_tx_length);
        fw_tx_pending = 0U;
        if (fw_task != NULL) (void)xTaskNotifyGive(fw_task);
    }
}
