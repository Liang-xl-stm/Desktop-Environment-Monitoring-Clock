/**
 * @file EPD_4in26.h
 * @brief Waveshare 4.26 寸黑白/四灰阶电子纸控制器接口。
 * @details 面板分辨率为 800 x 480；黑白整屏缓存大小为 48,000 字节。
 */
#ifndef __EPD_4IN26_H
#define __EPD_4IN26_H

#include "DEV_Config.h"

#define EPD_4in26_WIDTH  800U /* 面板横向像素数。 */
#define EPD_4in26_HEIGHT 480U /* 面板纵向像素数。 */

/** @brief 等待 BUSY 引脚释放。
 *  @return 操作完成返回 0；等待超时返回 -1。
 */
int EPD_4in26_ReadBusy(void);

/** @brief 执行黑白模式的普通初始化，不自动清屏。
 *  @return 初始化成功返回 0；BUSY 等待超时返回 -1。
 */
int EPD_4in26_Init(void);

/** @brief 初始化并切换至快速刷新模式。
 *  @note 快刷前应先普通初始化并全刷清屏建立基准；连续更新后应切回基准/全刷模式。
 *  @return 初始化成功返回 0；BUSY 等待超时返回 -1。
 */
int EPD_4in26_Init_Fast(void);

/** @brief 初始化四灰阶模式并载入相应 LUT 波形。
 *  @return 初始化成功返回 0；BUSY 等待超时返回 -1。
 */
int EPD_4in26_Init_4GRAY(void);

/** @brief 将面板和新旧图像 RAM 清为白色并执行一次全屏刷新。
 *  @return 刷新成功返回 0；BUSY 等待超时返回 -1。
 */
int EPD_4in26_Clear(void);

/** @brief 将黑白图像写入新图像 RAM 并执行普通全屏刷新。
 *  @param image 800 x 480、1 bit/pixel 的图像缓存，大小至少 48,000 字节；1 为白、0 为黑。
 *  @return 刷新成功返回 0；BUSY 等待超时返回 -1。
 */
int EPD_4in26_Display(const UBYTE *image);

/** @brief 将黑白图像同步写入新旧图像 RAM，再执行全屏基准刷新。
 *  @param image 800 x 480、1 bit/pixel 的图像缓存，大小至少 48,000 字节；1 为白、0 为黑。
 *  @return 刷新成功返回 0；BUSY 等待超时返回 -1。
 */
int EPD_4in26_Display_Base(const UBYTE *image);

/** @brief 使用快速波形显示黑白图像。
 *  @param image 800 x 480、1 bit/pixel 的图像缓存，大小至少 48,000 字节；1 为白、0 为黑。
 *  @note 仅写入新图像 RAM；使用前需先建立有效基准，勿将此接口当作无限连续全刷。
 *  @return 刷新成功返回 0；BUSY 等待超时返回 -1。
 */
int EPD_4in26_Display_Fast(const UBYTE *image);

/** @brief 更新指定矩形区域并执行局部刷新。
 *  @param image 局部图像缓存；每行按 ceil(width / 8) 字节紧凑排列。
 *  @param x 局部窗口左上角横坐标，单位为像素。
 *  @param y 局部窗口左上角纵坐标，单位为像素。
 *  @param width 局部窗口宽度，单位为像素；建议 x 和 width 按 8 像素对齐。
 *  @param height 局部窗口高度，单位为像素。
 *  @return 刷新成功返回 0；参数越界或 BUSY 等待超时返回 -1。
 */
int EPD_4in26_Display_Part(const UBYTE *image, UWORD x, UWORD y, UWORD width, UWORD height);

/** @brief 将四灰阶缓存转换为控制器图像数据并刷新。
 *  @param image 四灰阶源缓存，按每像素 2 bit 打包，至少 48,000 字节。
 *  @note 当前实现和原示例使用 800 x 240 的逻辑绘图缓存，显示方向由绘图库设置。
 *  @return 刷新成功返回 0；BUSY 等待超时返回 -1。
 */
int EPD_4in26_4GrayDisplay(const UBYTE *image);

/** @brief 发送控制器休眠命令。
 *  @return 命令发送后返回 0。
 */
int EPD_4in26_Sleep(void);

#endif
