# 官方源码来源

- 项目：FreeRTOS/FreeRTOS-Kernel
- 固定版本：V11.3.1
- 发布页：https://github.com/FreeRTOS/FreeRTOS-Kernel/releases/tag/V11.3.1
- 归档：https://codeload.github.com/FreeRTOS/FreeRTOS-Kernel/zip/refs/tags/V11.3.1
- 归档 SHA256：cc24e16ac8cbb40ece06db2173598a596a6f3234d771f8f24b2c3c318498d288
- 许可：MIT，见 LICENSE.md。

保留整个 include 目录。当前编译 tasks.c、list.c、queue.c、
portable/RVDS/ARM_CM4F/port.c 和 portable/MemMang/heap_4.c。
官方文件保持原样；应用配置在 ../../Config/FreeRTOSConfig.h。
暂未加入 event_groups.c、stream_buffer.c、timers.c；需要这些功能时从同一 tag 添加，
并启用对应配置。不要混用其他版本或其他编译器的端口。
