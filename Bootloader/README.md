# 独立 Bootloader 工程

Keil 打开 `USER/Bootloader.uvprojx`，使用 ARMCLANG V6.9 完整编译。

- USER：main.c、hooks.c、blt_conf.h、片内 Flash 布局、系统时钟、散装文件。
- Hardware/LED：原示例 LED；Hardware/SDCard：原示例 SD 卡接口。
- CORE：CMSIS 与 STM32F407 启动文件；FWLIB：HAL/LL。
- Middlewares/OpenBLT：OpenBLT 核心与其第三方依赖，保持原实现。
- Middlewares/printf：原示例格式化输出；OBJ：编译结果。

Bootloader 从 `0x08000000` 启动，最大 `0x20000`（128 KiB）；App 从 `0x08020000` 启动。
App 向量表偏移 `0x188` 留有 OpenBLT 校验字，BIN 构建后由应用工具补齐。
当前保留 OpenBLT 的启动检查和跳转。外部 W25Q128 的 A/B 安装及自动回滚还需要后续实现。
