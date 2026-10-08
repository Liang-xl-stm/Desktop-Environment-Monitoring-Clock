/**
 * @file EPD_4in26.c
 * @brief 4.26 寸电子纸控制器命令、图像传输及刷新时序实现。
 * @details 控制器经四线 SPI 通信；整屏黑白图像采用 1 bit/pixel，缓存大小为 48,000 字节。
 */
#include "EPD_4in26.h"

#define EPD_BUSY_RETRY_COUNT 1500U /* 每次轮询间隔 20 ms，最大等待约 30 秒。 */

/* 控制器四灰阶 LUT 及驱动电压参数，须与对应面板版本的厂商波形匹配。 */
static const UBYTE lut_data_4gray[112] = {
    0x80, 0x48, 0x4A, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x48, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x88, 0x48, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xA8, 0x48, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x1E, 0x1C, 0x02, 0x00,
    0x05, 0x01, 0x05, 0x01, 0x02,
    0x08, 0x01, 0x01, 0x04, 0x04,
    0x00, 0x02, 0x00, 0x02, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01,
    0x22, 0x22, 0x22, 0x22, 0x22,
    0x17, 0x41, 0xA8, 0x32, 0x30,
    0x00, 0x00
};

/** @brief 对电子纸模块执行硬件复位。
 *  @details RST 拉低 2 ms 后释放，并等待控制器启动稳定。
 */
static void EPD_4in26_Reset(void)
{
    DEV_Digital_Write(EPD_RST_PIN, 1U);
    DEV_Delay_ms(100U);
    DEV_Digital_Write(EPD_RST_PIN, 0U);
    DEV_Delay_ms(2U);
    DEV_Digital_Write(EPD_RST_PIN, 1U);
    DEV_Delay_ms(100U);
}

/** @brief 向控制器发送一条命令。
 *  @param command 控制器命令字节。
 *  @details DC=0 表示命令，CS 在单字节传输期间拉低。
 */
static void EPD_4in26_SendCommand(UBYTE command)
{
    DEV_Digital_Write(EPD_DC_PIN, 0U);
    DEV_Digital_Write(EPD_CS_PIN, 0U);
    DEV_SPI_WriteByte(command);
    DEV_Digital_Write(EPD_CS_PIN, 1U);
}

/** @brief 向控制器发送一个数据字节。
 *  @param data 控制器寄存器参数或图像数据。
 *  @details DC=1 表示数据，CS 在单字节传输期间拉低。
 */
static void EPD_4in26_SendData(UBYTE data)
{
    DEV_Digital_Write(EPD_DC_PIN, 1U);
    DEV_Digital_Write(EPD_CS_PIN, 0U);
    DEV_SPI_WriteByte(data);
    DEV_Digital_Write(EPD_CS_PIN, 1U);
}

/** @brief 向控制器连续发送一段数据缓冲区。
 *  @param data 待发送数据首地址。
 *  @param length 待发送字节数。
 */
static void EPD_4in26_SendDataBuffer(const UBYTE *data, UDOUBLE length)
{
    DEV_Digital_Write(EPD_DC_PIN, 1U);
    DEV_Digital_Write(EPD_CS_PIN, 0U);
    DEV_SPI_Write_nByte((UBYTE *)data, length);
    DEV_Digital_Write(EPD_CS_PIN, 1U);
}

/** @brief 等待面板 BUSY 信号释放。
 *  @details BUSY 为高电平时继续等待；最多轮询 EPD_BUSY_RETRY_COUNT 次，单次间隔 20 ms。
 *  @return 控制器空闲返回 0；等待超时返回 -1。
 */
int EPD_4in26_ReadBusy(void)
{
    UWORD retry;

    for (retry = 0U; retry < EPD_BUSY_RETRY_COUNT; retry++) {
        if (DEV_Digital_Read(EPD_BUSY_PIN) == 0U) {
            DEV_Delay_ms(20U);
            return 0;
        }
        DEV_Delay_ms(20U);
    }

    return -1;
}

/** @brief 启动一次控制器刷新并等待刷新完成。
 *  @param update_control 控制器更新控制寄存器 0x22 的设置值。
 *  @return 刷新完成返回 0；BUSY 等待超时返回 -1。
 */
static int EPD_4in26_TurnOnDisplay(UBYTE update_control)
{
    EPD_4in26_SendCommand(0x22U);
    EPD_4in26_SendData(update_control);
    EPD_4in26_SendCommand(0x20U);
    return EPD_4in26_ReadBusy();
}

/** @brief 将四灰阶 LUT 与对应驱动电压参数写入控制器。
 *  @details LUT 表使用前应确认面板型号/版本与波形参数相匹配。
 */
static void EPD_4in26_Lut(void)
{
    UWORD count;

    EPD_4in26_SendCommand(0x32U);
    for (count = 0U; count < 105U; count++) {
        EPD_4in26_SendData(lut_data_4gray[count]);
    }

    EPD_4in26_SendCommand(0x03U);
    EPD_4in26_SendData(lut_data_4gray[105U]);
    EPD_4in26_SendCommand(0x04U);
    EPD_4in26_SendData(lut_data_4gray[106U]);
    EPD_4in26_SendData(lut_data_4gray[107U]);
    EPD_4in26_SendData(lut_data_4gray[108U]);
    EPD_4in26_SendCommand(0x2CU);
    EPD_4in26_SendData(lut_data_4gray[109U]);
}

/** @brief 设置控制器图像 RAM 的矩形窗口。
 *  @param x_start 窗口起始横坐标。
 *  @param y_start 窗口起始纵坐标。
 *  @param x_end 窗口结束横坐标。
 *  @param y_end 窗口结束纵坐标。
 *  @details 坐标按控制器寄存器要求拆成低字节和高位写入。
 */
static void EPD_4in26_SetWindows(UWORD x_start, UWORD y_start, UWORD x_end, UWORD y_end)
{
    EPD_4in26_SendCommand(0x44U);
    EPD_4in26_SendData((UBYTE)(x_start & 0xFFU));
    EPD_4in26_SendData((UBYTE)((x_start >> 8U) & 0x03U));
    EPD_4in26_SendData((UBYTE)(x_end & 0xFFU));
    EPD_4in26_SendData((UBYTE)((x_end >> 8U) & 0x03U));

    EPD_4in26_SendCommand(0x45U);
    EPD_4in26_SendData((UBYTE)(y_start & 0xFFU));
    EPD_4in26_SendData((UBYTE)((y_start >> 8U) & 0x03U));
    EPD_4in26_SendData((UBYTE)(y_end & 0xFFU));
    EPD_4in26_SendData((UBYTE)((y_end >> 8U) & 0x03U));
}

/** @brief 设置控制器图像 RAM 当前读写游标。
 *  @param x_start 游标横坐标。
 *  @param y_start 游标纵坐标。
 */
static void EPD_4in26_SetCursor(UWORD x_start, UWORD y_start)
{
    EPD_4in26_SendCommand(0x4EU);
    EPD_4in26_SendData((UBYTE)(x_start & 0xFFU));
    EPD_4in26_SendData((UBYTE)((x_start >> 8U) & 0x03U));
    EPD_4in26_SendCommand(0x4FU);
    EPD_4in26_SendData((UBYTE)(y_start & 0xFFU));
    EPD_4in26_SendData((UBYTE)((y_start >> 8U) & 0x03U));
}

/** @brief 执行不同显示模式共用的控制器初始化。
 *  @param four_gray 非零时额外加载四灰阶 LUT；零表示黑白模式。
 *  @return 初始化成功返回 0；任一 BUSY 等待超时返回 -1。
 *  @details 依次复位、软复位、配置内部温度传感器、驱动电压、扫描方向、显示边框和 RAM 窗口。
 */
static int EPD_4in26_Initialize(UBYTE four_gray)
{
    EPD_4in26_Reset();
    DEV_Delay_ms(100U);
    if (EPD_4in26_ReadBusy() != 0) return -1;

    EPD_4in26_SendCommand(0x12U);
    if (EPD_4in26_ReadBusy() != 0) return -1;

    EPD_4in26_SendCommand(0x18U);
    EPD_4in26_SendData(0x80U);
    EPD_4in26_SendCommand(0x0CU);
    EPD_4in26_SendData(0xAEU);
    EPD_4in26_SendData(0xC7U);
    EPD_4in26_SendData(0xC3U);
    EPD_4in26_SendData(0xC0U);
    EPD_4in26_SendData(0x80U);

    EPD_4in26_SendCommand(0x01U);
    EPD_4in26_SendData((UBYTE)((EPD_4in26_HEIGHT - 1U) & 0xFFU));
    EPD_4in26_SendData((UBYTE)((EPD_4in26_HEIGHT - 1U) >> 8U));
    EPD_4in26_SendData(0x02U);
    EPD_4in26_SendCommand(0x3CU);
    EPD_4in26_SendData(0x01U);
    EPD_4in26_SendCommand(0x11U);
    EPD_4in26_SendData(0x01U);

    EPD_4in26_SetWindows(0U, EPD_4in26_HEIGHT - 1U, EPD_4in26_WIDTH - 1U, 0U);
    EPD_4in26_SetCursor(0U, 0U);
    if (EPD_4in26_ReadBusy() != 0) return -1;

    if (four_gray != 0U) EPD_4in26_Lut();
    return 0;
}

/** @brief 初始化控制器的普通黑白刷新模式。
 *  @return 初始化成功返回 0；BUSY 等待超时返回 -1。
 *  @note 本函数只初始化控制器，不负责清除面板旧图像。
 */
int EPD_4in26_Init(void)
{
    return EPD_4in26_Initialize(0U);
}

/** @brief 初始化黑白快速刷新模式。
 *  @return 初始化成功返回 0；BUSY 等待超时返回 -1。
 *  @note 快刷前应先执行普通初始化及全屏清理以建立有效图像基准。
 */
int EPD_4in26_Init_Fast(void)
{
    if (EPD_4in26_Initialize(0U) != 0) return -1;

    EPD_4in26_SendCommand(0x1AU);
    EPD_4in26_SendData(0x5AU);
    EPD_4in26_SendCommand(0x22U);
    EPD_4in26_SendData(0x91U);
    EPD_4in26_SendCommand(0x20U);
    return EPD_4in26_ReadBusy();
}

/** @brief 初始化四灰阶模式并装载专用 LUT。
 *  @return 初始化成功返回 0；BUSY 等待超时返回 -1。
 */
int EPD_4in26_Init_4GRAY(void)
{
    return EPD_4in26_Initialize(1U);
}

/** @brief 将面板清成白色并同步清除新、旧图像 RAM。
 *  @return 全屏刷新完成返回 0；BUSY 等待超时返回 -1。
 *  @details 黑白格式中 0xFF 表示白色；命令 0x24 与 0x26 的 RAM 均写入白色行数据。
 */
int EPD_4in26_Clear(void)
{
    UWORD row;
    UBYTE white_line[EPD_4in26_WIDTH / 8U];

    for (row = 0U; row < (EPD_4in26_WIDTH / 8U); row++) white_line[row] = 0xFFU;

    EPD_4in26_SendCommand(0x24U);
    for (row = 0U; row < EPD_4in26_HEIGHT; row++) {
        EPD_4in26_SendDataBuffer(white_line, sizeof(white_line));
    }
    EPD_4in26_SendCommand(0x26U);
    for (row = 0U; row < EPD_4in26_HEIGHT; row++) {
        EPD_4in26_SendDataBuffer(white_line, sizeof(white_line));
    }
    return EPD_4in26_TurnOnDisplay(0xF7U);
}

/** @brief 将整屏图像写入新图像 RAM 并执行普通全屏刷新。
 *  @param image 800 x 480、每像素 1 bit 的图像缓存，至少 48,000 字节；1 表示白色、0 表示黑色。
 *  @return 刷新完成返回 0；BUSY 等待超时返回 -1。
 *  @note 本函数只写入命令 0x24 对应的 RAM；若需要同步刷新基准，请使用 EPD_4in26_Display_Base。
 */
int EPD_4in26_Display(const UBYTE *image)
{
    UWORD row;
    UWORD width = EPD_4in26_WIDTH / 8U;

    EPD_4in26_SendCommand(0x24U);
    for (row = 0U; row < EPD_4in26_HEIGHT; row++) {
        EPD_4in26_SendDataBuffer(&image[(UDOUBLE)row * width], width);
    }
    return EPD_4in26_TurnOnDisplay(0xF7U);
}

/** @brief 将整屏图像同步写入新、旧图像 RAM 后执行全屏基准刷新。
 *  @param image 800 x 480、每像素 1 bit 的图像缓存，至少 48,000 字节；1 表示白色、0 表示黑色。
 *  @return 刷新完成返回 0；BUSY 等待超时返回 -1。
 *  @details 通过同时更新 0x24 和 0x26 图像 RAM，避免后续刷新继续引用过期基准帧。
 */
int EPD_4in26_Display_Base(const UBYTE *image)
{
    UWORD row;
    UWORD width = EPD_4in26_WIDTH / 8U;

    EPD_4in26_SendCommand(0x24U);
    for (row = 0U; row < EPD_4in26_HEIGHT; row++) {
        EPD_4in26_SendDataBuffer(&image[(UDOUBLE)row * width], width);
    }
    EPD_4in26_SendCommand(0x26U);
    for (row = 0U; row < EPD_4in26_HEIGHT; row++) {
        EPD_4in26_SendDataBuffer(&image[(UDOUBLE)row * width], width);
    }
    return EPD_4in26_TurnOnDisplay(0xF7U);
}

/** @brief 使用快速波形刷新一帧黑白图像。
 *  @param image 800 x 480、每像素 1 bit 的图像缓存，至少 48,000 字节；1 表示白色、0 表示黑色。
 *  @return 刷新完成返回 0；BUSY 等待超时返回 -1。
 *  @note 仅更新 0x24 对应 RAM；须由调用方维护图像基准，不宜未经全刷就无限循环调用。
 */
int EPD_4in26_Display_Fast(const UBYTE *image)
{
    UWORD row;
    UWORD width = EPD_4in26_WIDTH / 8U;

    EPD_4in26_SendCommand(0x24U);
    for (row = 0U; row < EPD_4in26_HEIGHT; row++) {
        EPD_4in26_SendDataBuffer(&image[(UDOUBLE)row * width], width);
    }
    return EPD_4in26_TurnOnDisplay(0xC7U);
}

/** @brief 在指定窗口执行局部刷新。
 *  @param image 局部窗口的紧凑图像缓存，每行占 ceil(width / 8) 字节。
 *  @param x 窗口左上角横坐标，单位为像素。
 *  @param y 窗口左上角纵坐标，单位为像素。
 *  @param width 窗口宽度，单位为像素；建议 x、width 按 8 像素对齐。
 *  @param height 窗口高度，单位为像素。
 *  @return 刷新完成返回 0；窗口越界或 BUSY 等待超时返回 -1。
 */
int EPD_4in26_Display_Part(const UBYTE *image, UWORD x, UWORD y, UWORD width, UWORD height)
{
    UWORD row;
    UWORD bytes_per_line;

    /* 先校验窗口尺寸和边界，避免坐标计算溢出或写出面板范围。 */
    if ((width == 0U) || (height == 0U) || (x >= EPD_4in26_WIDTH) || (y >= EPD_4in26_HEIGHT) ||
        ((UDOUBLE)x + width > EPD_4in26_WIDTH) || ((UDOUBLE)y + height > EPD_4in26_HEIGHT)) return -1;

    /* 局部图像每行按字节对齐，末尾不足 8 像素时仍占用一个完整字节。 */
    bytes_per_line = (UWORD)((width + 7U) / 8U);
    EPD_4in26_Reset();
    EPD_4in26_SendCommand(0x18U);
    EPD_4in26_SendData(0x80U);
    EPD_4in26_SendCommand(0x3CU);
    EPD_4in26_SendData(0x80U);
    EPD_4in26_SetWindows(x, y, (UWORD)(x + width - 1U), (UWORD)(y + height - 1U));
    EPD_4in26_SetCursor(x, y);
    EPD_4in26_SendCommand(0x24U);
    for (row = 0U; row < height; row++) {
        EPD_4in26_SendDataBuffer(&image[(UDOUBLE)row * bytes_per_line], bytes_per_line);
    }
    return EPD_4in26_TurnOnDisplay(0xFFU);
}

/** @brief 将四灰阶缓存编码为控制器所需的新、旧黑白图像 RAM 数据并刷新。
 *  @param image 按每像素 2 bit 打包的四灰阶缓存，至少 48,000 字节。
 *  @return 刷新完成返回 0；BUSY 等待超时返回 -1。
 *  @details 每轮读取两个源字节，按像素高两位提取灰阶；0x24 与 0x26 使用不同阈值生成驱动平面。
 */
int EPD_4in26_4GrayDisplay(const UBYTE *image)
{
    UDOUBLE i;
    UBYTE j;
    UBYTE k;
    UBYTE temp1;
    UBYTE temp2;
    UBYTE temp3;

    /* 新图像平面：每个输出字节对应 8 个 2 bit 源像素。 */
    EPD_4in26_SendCommand(0x24U);
    for (i = 0U; i < 24000U; i++) {
        temp3 = 0U;
        for (j = 0U; j < 2U; j++) {
            temp1 = image[i * 2U + j];
            for (k = 0U; k < 2U; k++) {
                temp2 = temp1 & 0xC0U;
                /* 依照新帧阈值将源灰阶转换为单 bit 驱动值。 */
                temp3 |= (temp2 == 0x00U || temp2 == 0x80U) ? 0x01U : 0x00U;
                temp3 <<= 1U;
                temp1 <<= 2U;
                temp2 = temp1 & 0xC0U;
                temp3 |= (temp2 == 0x00U || temp2 == 0x80U) ? 0x01U : 0x00U;
                if ((j != 1U) || (k != 1U)) temp3 <<= 1U;
                temp1 <<= 2U;
            }
        }
        EPD_4in26_SendData(temp3);
    }
    /* 按原厂数据布局补齐该图像 RAM 后半段。 */
    for (i = 0U; i < 24000U; i++) EPD_4in26_SendData(0x00U);

    /* 旧图像平面：使用与新帧不同的灰阶阈值，供控制器计算灰阶过渡波形。 */
    EPD_4in26_SendCommand(0x26U);
    for (i = 0U; i < 24000U; i++) {
        temp3 = 0U;
        for (j = 0U; j < 2U; j++) {
            temp1 = image[i * 2U + j];
            for (k = 0U; k < 2U; k++) {
                temp2 = temp1 & 0xC0U;
                /* 按旧帧阈值生成参考驱动位。 */
                temp3 |= (temp2 == 0x00U || temp2 == 0x40U) ? 0x01U : 0x00U;
                temp3 <<= 1U;
                temp1 <<= 2U;
                temp2 = temp1 & 0xC0U;
                temp3 |= (temp2 == 0x00U || temp2 == 0x40U) ? 0x01U : 0x00U;
                if ((j != 1U) || (k != 1U)) temp3 <<= 1U;
                temp1 <<= 2U;
            }
        }
        EPD_4in26_SendData(temp3);
    }
    for (i = 0U; i < 24000U; i++) EPD_4in26_SendData(0x00U);

    /* 四灰阶模式使用 0xC7 更新控制设置，并等待 BUSY 释放。 */
    return EPD_4in26_TurnOnDisplay(0xC7U);
}

/** @brief 发送控制器休眠命令。
 *  @return 命令发送后返回 0。
 *  @note 休眠前应确认刷新已结束；重新显示前应重新初始化控制器。
 */
int EPD_4in26_Sleep(void)
{
    EPD_4in26_SendCommand(0x10U);
    EPD_4in26_SendData(0x03U);
    DEV_Delay_ms(100U);
    return 0;
}
