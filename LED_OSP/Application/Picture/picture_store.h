/** @file picture_store.h @brief W25Q128 固定编号黑白图片存储，不依赖网络或屏幕。 */
#ifndef PICTURE_STORE_H
#define PICTURE_STORE_H
#include <stdint.h>

#define PICTURE_STORAGE_ADDR 0x00300000UL /* 固件 B 区结束后的地址。 */
#define PICTURE_SLOT_SIZE    0x0000D000UL /* 4KiB 管理 + 48KiB 正文。 */
#define PICTURE_SLOT_COUNT   256U
#define PICTURE_WIDTH        800U
#define PICTURE_HEIGHT       480U
#define PICTURE_DATA_SIZE    48000UL     /* 800 / 8 * 480，1 白、0 黑。 */

/** @brief 一次图片下载的状态；同一个写入任务串行使用，首次清零。 */
typedef struct
{
    uint32_t slot;          /**< 图片编号，0～255。 */
    uint32_t received_size; /**< 已成功写入正文的字节数。 */
    uint32_t expected_crc;  /**< 电脑提供的 CRC-32/ISO-HDLC。 */
    uint8_t active;         /**< 1 正在下载，0 无活跃下载。 */
    const char *error;      /**< 失败阶段，指向只读常量；NULL 表示尚无错误。 */
    uint32_t error_address; /**< 发生错误的 Flash 字节地址，便于串口定位。 */
} picture_write_t;

/**
 * @brief 擦除选定图片槽，为写入做准备。
 * @param ctx 持续有效的状态地址，首次调用前清零；不能已经处于 active 状态。
 * @param slot 图片编号 0～255，不是 Flash 字节地址。
 * @param crc 正文 48000 字节的 CRC-32/ISO-HDLC，与 Python zlib.crc32 一致。
 * @return 1 准备完成，0 参数错误、状态错误或 Flash 操作失败。
 * @note 会覆盖这个编号的旧图片；失败不能保证保留旧图，不影响其他槽和固件 A/B。
 */
uint8_t Picture_Begin(picture_write_t *ctx, uint32_t slot, uint32_t crc);

/**
 * @brief 顺序追加一批原始黑白像素数据，不包含包头。
 * @param ctx Picture_Begin 成功的状态地址。
 * @param data 数据地址，至少 length 字节，在返回前保持有效。
 * @param length 实际数据长度，大于 0，不能超过剩余 48000 字节正文范围。
 * @return 1 整批写入成功并更新 received_size，0 失败。
 * @note 未自动提交有效标记，失败可能已写入部分数据；写入方不能并发调用。
 */
uint8_t Picture_Write(picture_write_t *ctx, const uint8_t *data, uint32_t length);

/** @brief 收齐正文后读回 CRC，通过后最后提交有效标记。
 *  @param ctx 当前下载状态地址。
 *  @return 1 图片已保存且管理记录读回一致；0 长度不足、CRC 错误或 Flash 失败。
 */
uint8_t Picture_Finish(picture_write_t *ctx);

/** @brief 停止未完成的下载，不擦除、不创建有效记录，不撤销已完成的图片。
 *  @param ctx 状态地址；NULL 时不操作。
 */
void Picture_Abort(picture_write_t *ctx);

/**
 * @brief 读取并校验指定图片，可与下载任务共用 Flash 事务锁。
 * @param slot 图片编号 0～255。
 * @param data 输出缓冲地址，成功后填入 48000 字节黑白图像。
 * @param capacity 缓冲容量，至少 48000 字节；不能传指针的 sizeof。
 * @return 1 管理记录和正文 CRC 均通过；0 无有效图片、参数错误或读取失败。
 * @note 失败时缓冲可能有部分数据，不得显示；不要从任务栈或 32KiB 堆申请整屏缓冲。
 */
uint8_t Picture_Read(uint32_t slot, uint8_t *data, uint32_t capacity);
#endif
