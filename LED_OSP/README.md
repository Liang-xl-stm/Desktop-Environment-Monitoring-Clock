# STM32F407 + ESP8266 应用

用 Keil 打开 `USER/LED_OSP.uvprojx`，执行 Rebuild。
App 起始地址 `0x08020000`，容量 `0xE0000`（896 KiB）。Bootloader 使用前 128 KiB。

## 文件放在哪里

| 目录 | 内容 |
| --- | --- |
| USER | main.c、系统时钟、FreeRTOS 钩子、配置、Keil 工程 |
| Hardware/LED | PF9 LED 的初始化和控制 |
| Hardware/SPI | SPI1 收发、PB14 片选、事务锁、硬件超时 |
| Hardware/W25Q128 | 外部 Flash 读写和擦除 |
| Hardware/ESP8266 | USART3/PF6 与 LwESP 的硬件适配 |
| Hardware/EPD_4in26 | 4.26 寸 800×480 墨水屏原版控制驱动及软件 SPI 适配 |
| SYSTEM/usart | USART1、USART3 的中断缓存和直接收发接口 |
| SYSTEM/log | EasyLogger 初始化和 USART1 输出锁 |
| Application | 任务创建、联网、自检 |
| Application/Firmware | TCP 接收、包解析、A/B 管理、CRC 和启动检查 |
| Application/Picture | W25Q128 固定图片槽、CRC 校验及读回 |
| CORE / FWLIB | CMSIS、启动文件 / STM32 标准外设库 |
| FreeRTOS / Middlewares | RTOS / LwESP、EasyLogger |
| OBJ | AXF、BIN、HEX、编译结果 |
| Tools / Tests | 电脑发送工具 / 主机回归测试 |

## 怎么调用

```c
LED_Init();
LED_Set(1);                          /* 1 点亮，0 关闭 */
USART1_Init(115200);
USART3_Init(115200);
USART3_Send(data, length);            /* 数据地址、实际字节数 */
received = USART3_Receive(buf, size); /* 输出缓冲、最大容量；返回实际读取长度 */
ok = W25Q128_Init();                 /* 无参数，自动初始化 SPI1 */
ok = W25Q128_Read(buf, address, len); /* 输出缓冲、Flash 字节地址、读取长度 */
ok = W25Q128_Write(data, address, len); /* 输入数据、Flash 地址、写入长度；不自动擦除 */
ok = W25Q128_Erase_Sector(address);  /* 4KiB 扇区起始地址 */
```

Flash 驱动返回 `1` 表示成功、`0` 表示失败；墨水屏返回 `0` 表示成功、`-1` 表示失败。
接口参数详见各头文件注释。
`USER/main.c` 初始化硬件并调用 `App_Init()`，随后启动 FreeRTOS。
LED 每 500ms 切换一次；控制台回显独立运行；网络初始化失败只结束网络任务。
USART3 接收缓存仅由 ESP_RX 读取，其他任务不可同时读取。

日志须在运行中的任务首次调用 `Log_Init()`，之后用 `elog_i` / `elog_e` 等宏。
USART1 原样输出用 `Log_WriteRaw(data, length)`，与格式日志共用同一把锁。

## 配置和升级测试

Wi-Fi 名称、密码和开发板 TCP 端口在 `USER/app_config.h`，无需配置电脑 IP。
升级配置在 `USER/firmware_tcp_config.h`，Flash 自检默认关闭。
电脑和开发板连接同一热点，先在工程根目录运行：

```text
python Tools/send_firmware.py OBJ/LED_OSP.bin --host 172.20.10.3
```

将示例 IP 换成开发板当前 IP。电脑直接连接开发板服务器，程序将新包保存到外部 Flash 的候选分区。
本工程仍是接收和校验阶段，Bootloader 尚未实现外部 A/B 复制与自动回滚。
外部 Flash 已有记录不会因重编译自动覆盖。

Keil 构建后生成 BIN 并添加 OpenBLT 校验字，同时自动生成 `OBJ/LED_OSP_openblt.hex`。
AXF 和 Keil 原始 HEX 不含这次 BIN 补丁。ST-Link 烧录 App 时使用 `LED_OSP_openblt.hex`。
首次同时烧录两份程序时，在两个工程编译后运行 `python Tools/make_flash_images.py`，
使用 `OBJ/Bootloader_App.hex`。工具只生成文件，不会操作开发板。
测试方法见 `Tests/README.md`。

## 墨水屏

接线和测试开关在 `USER/epd_config.h`：DIN=PF0、CLK=PF1、CS=PF2、DC=PF3、
RST=PF4、BUSY=PF5、PWR=PF7。PF6 仍用于 ESP8266 复位。

默认启用持久 EPD 任务，重启读取 `EPD_STARTUP_PICTURE`（默认 0）号图片，校验成功后显示。
无有效图片时保留原黑块测试。刷新后屏幕休眠，任务继续等待新的显示请求。
`EPD_ShowPicture(3)` 请求读取并显示 3 号图片，返回 1 表示已投递；显示结果查看 epd 日志。
`EPD_DEMO_ENABLE=0` 只关闭无图时的黑块测试；直接操作底层驱动时先关闭 `EPD_TASK_ENABLE`。
完整驱动说明见 `docs/epd_4in26.md`。

## TCP 传图

运行 `Tools/启动图片发送器.vbs` 或 `python Tools/picture_sender_gui.pyw`，自动搜索开发板、选择图片、预览后发送，换图无需复位。
图片工具需要 Python 和 Pillow；打包的 EXE 不进入源码仓库。命令行也可使用 `python Tools/send_picture.py "你的图片.jpg" --slot 0 --host 172.20.10.3`。
电脑会先转换为 800×480 黑白数据，等比例缩放、补白边，并输出 BIN 和预览图到 `OBJ/pictures`。
支持 PNG/JPG/BMP 及 48000 字节原始 BIN；图片转换依赖 Pillow，当前电脑已安装。
多图可用 `python Tools/send_picture.py a.png b.png --slot 0 --host 172.20.10.3`，依次保存到 0、1 号槽。
图片编号 0～255，每槽 52KiB，整个图片区使用 `0x300000～0xFFFFFF` 的 13MiB。
重复使用同一编号会替换旧图；写入或掉线失败可能使该槽旧图失效，其他槽和固件区不受影响。
窗口中的整体预览平滑合并细小抖动点，双击查看原始黑白点阵；发送数据始终使用原始 48000 字节。
电脑预览与实屏对比度可能随环境光产生差别。对照图可用 `Tools/Pictures/contrast_test.png`。

DONE 表示 Flash 保存及 CRC 校验完成，屏幕异步显示；快速连传时屏幕队列保留最近的请求。
固件仍用 `Tools/send_firmware.py`，并受原 A/B 基线保护；图片不会作为固件安装。
详细参数、协议及日志说明见 `docs/tcp_pictures.md`。
