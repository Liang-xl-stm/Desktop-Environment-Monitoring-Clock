# Desktop Environment Monitoring Clock

基于 STM32F407 的桌面环境监测时钟项目，目前已完成联网、墨水屏图片显示和外部 Flash 固件包接收等基础功能。环境监测与时钟应用仍在开发中。

## 工程结构

| 目录 | 内容 | Keil 工程入口 |
| --- | --- | --- |
| Bootloader | OpenBLT 启动检查、应用校验和跳转 | `Bootloader/USER/Bootloader.uvprojx` |
| LED_OSP | FreeRTOS 应用、ESP8266 联网、墨水屏和 W25Q128 存储 | `LED_OSP/USER/LED_OSP.uvprojx` |

两份工程保持同级。源码、头文件和第三方依赖包含在各自工程目录内；编译输出、打包文件、备份和个人 IDE 设置不进入版本管理。

## 当前功能

- ESP8266 连接 2.4 GHz Wi-Fi，开发板提供 TCP 服务器。
- Windows 图片发送窗口自动搜索设备，转换及预览 800×480 黑白图片，并支持连续发送。
- W25Q128 保存图片，墨水屏任务读取校验后的图片并刷新。
- 接收固件升级包，执行 CRC 校验并存入外部 Flash A/B 候选区。
- OpenBLT 检查应用向量表校验字并跳转。外部 Flash 固件安装和自动回滚仍待实现。

## 构建与运行

1. 安装 Keil MDK 和项目所需编译器，分别打开上表中的两份工程执行 Rebuild。
2. 修改 `LED_OSP/USER/app_config.h` 的 Wi-Fi 示例配置；公开提交时保持示例值。
3. Bootloader 起点为 `0x08000000`，大小 128 KiB；应用起点为 `0x08020000`，大小 896 KiB。
4. 应用的构建后步骤会生成 BIN、补齐 OpenBLT 校验字并生成可烧录的应用 HEX。
5. 两份工程编译后，在 `LED_OSP` 目录执行 `python Tools/make_flash_images.py`，生成合并的 `OBJ/Bootloader_App.hex`。

Keil 构建后命令中的 `fromelf.exe` 路径需与本机 Keil 安装位置一致。应用构建后脚本通过 `python` 启动，Python 应能从命令行访问。

图片工具需要 Python 和 Pillow。安装 Pillow 后，在 `LED_OSP` 目录执行：

```text
python -m pip install Pillow
python Tools/picture_sender_gui.pyw
```

电脑与开发板接入同一 Wi-Fi 或热点后，搜索设备、选择图片并发送。完整步骤见 [图片传输说明](LED_OSP/docs/tcp_pictures.md)，接线见 [墨水屏驱动说明](LED_OSP/docs/epd_4in26.md)。

## 验证

在 `LED_OSP` 目录执行 `python Tests/run_tests.py`，运行主机回归测试。需要 GCC、Python、Pillow 和 Tk。主机测试用于检查存储、协议和任务逻辑，板上 GPIO、供电与实际显示仍需硬件验证。

## 第三方代码

工程包含 OpenBLT、FreeRTOS、LwESP、EasyLogger、CMSIS 和 STM32 外设库等第三方代码。各库原有版权声明及许可证随源码保留，具体条款请查看对应目录的许可证文件和源码文件头。
