# 直接驱动调用架构实施计划

目标：按参考项目的 USER、Hardware、SYSTEM、Application、CORE、FWLIB 布局整理两个工程，保留现有功能并简化调用。

- [x] 完整备份 App 与 Bootloader。
- [x] 用已有 NOR Flash 和日志行为测试检查新的固定驱动接口。
- [x] 合并 Device/BSP 的驱动实现，移除设备对象和操作表转接。
- [x] 独立 LED、控制台和网络任务，网络失败不停止 LED。
- [x] 将固件解析、存储、A/B 状态及启动检查集中到 Application/Firmware。
- [x] 迁移 Bootloader，保留 OpenBLT 及厂商库，更新源文件与头文件路径。
- [x] 执行全部主机回归、两工程完整编译、检查加载地址和 OpenBLT 校验字。
- [x] 更新入口说明和发送命令，交付可检查的源码与编译产物。
