/** @file epd_app.h @brief 按编号读取 W25Q128 图片并由唯一任务显示。 */
#ifndef EPD_APP_H
#define EPD_APP_H
#include <stdint.h>
/** @brief 创建显示队列及持久屏幕任务，App_Init 中启动调度器前调用一次。
 *  @return 1 成功，0 已初始化、队列或任务分配失败。
 */
uint8_t EPD_App_Init(void);
/**
 * @brief 请求显示已经保存的图片，本函数不执行刷新。
 * @param slot 图片编号 0～255，不是 Flash 地址。
 * @return 1 请求已投递，0 编号越界、任务未启用或在中断中调用。
 * @note 普通任务调用；屏幕忙时保留最后一次请求，图片仍全部保存在 Flash。
 *       返回 1 不代表已经刷新完成，显示结果查看 epd 日志。
 */
uint8_t EPD_ShowPicture(uint16_t slot);
/** @param argument FreeRTOS 任务参数，未使用；屏幕操作只由这个任务执行。 */
void EPD_Task(void *argument);
#endif
