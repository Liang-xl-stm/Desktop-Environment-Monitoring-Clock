/** @file download_stream.h @brief 识别 FWUP 固件、PIC1 图片和只读 INFO，不依赖 TCP/RTOS。 */
#ifndef DOWNLOAD_STREAM_H
#define DOWNLOAD_STREAM_H
#include "firmware_stream.h"
#include "picture_store.h"

#define DOWNLOAD_PICTURE_MAGIC 0x31434950UL /* 小端字节 "PIC1"。 */
#define DOWNLOAD_INFO_MAGIC    0x4F464E49UL /* 小端字节 "INFO"，只读设备查询。 */
#define DOWNLOAD_READY    0x01U
#define DOWNLOAD_PROGRESS 0x02U
#define DOWNLOAD_DONE     0x04U
#define DOWNLOAD_INFO     0x08U
#define DOWNLOAD_ERROR    0x80U
typedef enum { DOWNLOAD_UNKNOWN = 0, DOWNLOAD_FIRMWARE, DOWNLOAD_PICTURE, DOWNLOAD_QUERY } download_type_t;
typedef struct
{
    firmware_stream_t firmware; /**< 原固件解析器，仅 FWUP 使用。 */
    picture_write_t picture;    /**< 当前图片写入状态，仅 PIC1 使用。 */
    uint8_t header[32];         /**< 包头自有副本，不持有网络缓冲地址。 */
    uint32_t header_used, header_size;
    uint32_t total_size, received_size; /**< 统一的正文总量和成功写入量。 */
    download_type_t type;
    uint8_t stage;             /**< 0 包头、1 正文、2 完成、3 失败。 */
    uint8_t firmware_allowed;  /**< 来自启动基线检查；图片下载不使用这个条件。 */
    const char *error;         /**< 失败阶段字符串，失败后供 TCP/串口报告。 */
} download_stream_t;

/** @param ctx 新会话状态地址。@param firmware_allowed 1 允许 FWUP，0 允许图片和 INFO。
 *  @note 由唯一下载任务调用，不能在活跃下载中重新初始化。
 */
void Download_Stream_Init(download_stream_t *ctx, uint8_t firmware_allowed);
/** @param ctx 会话状态。@param data 输入字节地址。@param length 本批实际字节数，非零。
 *  @return READY/PROGRESS/DONE 按位或，查询为 INFO；包头未收齐为 0；失败为 ERROR。
 *  @note 支持拆包和包头正文合包；不允许超量正文或未确认完成就紧跟下一包。
 */
uint8_t Download_Stream_Feed(download_stream_t *ctx, const uint8_t *data, uint32_t length);
/** @param ctx 当前会话，停止未完成下载，不撤销已提交的固件或图片。
 *  @return 1 清理成功；0 底层固件 Abort 拒绝，必须保留此对象且不能重新 Init。
 */
uint8_t Download_Stream_Stop(download_stream_t *ctx);
/** @param ctx 仅在图片 DONE 应答完成后准备下一包；固件完成后不允许复用。@return 1 成功。 */
uint8_t Download_Stream_NextPicture(download_stream_t *ctx);
/** @param ctx 图片 DONE 或 INFO 应答已发送完的解析对象。
 *  @return 1 已准备下一请求；0 当前类型或阶段不能复用。
 *  @note INFO 不调用任何图片/固件存储接口；固件提交后必须结束当前连接。
 */
uint8_t Download_Stream_NextRequest(download_stream_t *ctx);
#endif
