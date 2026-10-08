# 4.26 寸墨水屏移植与调用

本次只接入原目录顶层的四个驱动文件，未接入 ESP32/LVGL 示例。
`Hardware/EPD_4in26/EPD_4in26.c` 和 `.h` 与你已测试过的文件完全一致，
控制命令、波形和刷新方式均沿用原实现。

底层 `DEV_Config` 去掉旧工程 `delay.h` 依赖：微秒时序使用 DWT 周期计数，
毫秒延时在 FreeRTOS 运行后使用任务延时，不重新配置 SysTick，也不复位 Flash 共用的 DWT 计数器。
软件 SPI 使用独立 GPIO，不使用 Flash 的 SPI1 总线。

| 屏幕信号 | STM32 引脚 | 用途 |
| --- | --- | --- |
| DIN | PF0 | 软件 SPI 数据输出 |
| CLK | PF1 | 软件 SPI 时钟 |
| CS | PF2 | 低电平选中 |
| DC | PF3 | 0 发送命令，1 发送数据 |
| RST | PF4 | 低电平复位 |
| BUSY | PF5 | 高电平表示正在刷新 |
| PWR | PF7 | 高电平使能，与 ESP8266 的 PF6 分开 |
| GND | GND | 与开发板共地 |

接线配置位于 `USER/epd_config.h`，默认 `EPD_TASK_ENABLE=1` 创建持久任务。
任务启动后读取 `EPD_STARTUP_PICTURE`（默认 0）号图，无有效图片且 `EPD_DEMO_ENABLE=1` 时显示原黑块测试。
刷新后屏幕休眠，任务继续等待 `EPD_ShowPicture(slot)` 请求；显示成功打印 `picture slot=0 shown, sleeping`。
任务等待日志初始化，不等待热点或 TCP 连接；仅此任务拥有 48000 字节整屏缓冲。
BUSY 等待有次数上限，按当前节拍约 32 秒；失败记录日志，下次请求重新初始化屏幕。
主机测试及编译已验证，板上显示、实际时序和任务栈仍需烧录后观察。

## 自己调用

直接操作底层驱动前将 `EPD_TASK_ENABLE` 改为 `0`，避免自己的任务与屏幕任务同时操作屏幕。
在运行中的普通任务里调用，所有屏幕操作由同一个任务顺序执行。
屏幕函数是同步的：函数返回时，图像数据已经发送，刷新也已等待完成或超时。
图像缓冲在函数返回前必须保持有效，不能被其他任务覆盖。

```c
#include "EPD_4in26.h"

/* 以下函数在自己的 FreeRTOS 任务中调用一次，不放在中断中。 */
static int Screen_ShowTest(void)
{
    /* 64 像素每行需要 8 字节，32 行共 256 字节；0 为黑、1 为白。 */
    static const uint8_t black[64U / 8U * 32U] = {0};

    /* 无参数；配置 GPIO、DWT 时序并使能 PWR。0 成功，-1 失败。 */
    if (DEV_Module_Init() != 0) return -1;
    /* 无参数；硬件复位并初始化普通黑白模式，不自动清屏。 */
    if (EPD_4in26_Init() != 0) goto failed;
    /* 无参数；清白屏并建立新旧图像基准。 */
    if (EPD_4in26_Clear() != 0) goto failed;
    /* 参数：局部图像地址、左上角 x、左上角 y、宽度、高度，单位均为像素。 */
    if (EPD_4in26_Display_Part(black, 32U, 32U, 64U, 32U) != 0) goto failed;
    /* 刷新结束后休眠，屏幕保留图像；下次显示前重新初始化。 */
    return EPD_4in26_Sleep();

failed:
    DEV_Module_Exit(); /* 释放 CS、拉低 RST/PWR，结束本次显示操作。 */
    return -1;
}
```

## 图像接口

| 函数 | 参数与用途 |
| --- | --- |
| `EPD_4in26_Display(image)` | `image` 为至少 48000 字节黑白整屏图像；写新图像 RAM 并全刷 |
| `EPD_4in26_Display_Base(image)` | 同样的整屏图像；同步新旧 RAM，建立刷新基准 |
| `EPD_4in26_Display_Fast(image)` | 同样的整屏图像；需配合快速模式初始化及有效基准 |
| `EPD_4in26_Display_Part(image,x,y,width,height)` | 紧凑局部图像；每行占 `(width+7)/8` 字节，建议 x、width 按 8 像素对齐 |
| `EPD_4in26_Init_4GRAY()` | 无参数；载入原驱动四灰阶波形 |
| `EPD_4in26_4GrayDisplay(image)` | 保留原实现的 800×240、每像素 2 bit、48000 字节源缓存布局；不是一般的 800×480 四灰阶图像接口 |

这些函数 `0` 表示成功，`-1` 表示超时或参数错误，具体支持的检查沿用原驱动。
黑白数据按行排列，每行 100 字节，高位对应先发送的像素。
当前屏幕任务已有一个静态 48000 字节整屏缓冲；仅请求显示图片时不需要再申请。
自行创建整屏缓冲前须重新核算 RAM，不能在任务栈或当前 32KiB FreeRTOS 堆中申请。
