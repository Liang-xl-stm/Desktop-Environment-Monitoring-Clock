# 图片发送软件与连续接收 Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development. Each component has separate file ownership; root integrates and reviews.

**Goal:** 自动搜索同一局域网的墨水屏，在 Windows 软件中预览并发送图片，连续发送无需复位。

**Architecture:** ESP8266 常驻 TCP 服务器，Windows 为客户端。保留 PIC1 和 FWUP，新增只读 INFO 查询；下载任务按会话代号隔离连接。Tkinter 窗口复用 Pillow 转换和现有图片传输协议，搜索、转换和发送在后台执行。

**Tech Stack:** STM32 标准库、FreeRTOS、LwESP、Python 3.14、Tkinter、Pillow。当前无 Git 仓库，修改前以目录备份保存原文件。

## Task 1: 常驻服务器和安全会话

Files: Application/esp_app.c, Application/Firmware/firmware_tcp.c/h,
Application/download_stream.c/h, USER/app_config.h, Tests/test_download_tcp.c,
Tests/test_download_stream.c, Tests/stubs/tcp/, Tests/run_tests.py。

- [ ] 新增能复现旧终止行为的测试：完成后关闭、同连接号重新建立、再次完整图片；部分下载关闭后重连；旧数据和第二客户端隔离。
- [ ] 运行严格 GCC 测试，记录缺少新会话行为的红测试结果。
- [ ] 从启动任务启用服务器：
  ```c
  result = lwesp_set_server(1U, TCP_SERVER_PORT, 1U, 120U,
                            Tcp_Event, NULL, NULL, 1U);
  ```
- [ ] 增加 ACTIVE 和 SEND 完成事件入口。回调非阻塞，队列带会话代号，下载任务独占 Stream/Flash；在短核心锁内检查身份和提交非阻塞应答，保持发送缓冲至完成通知。
- [ ] 增加拆包安全的 INFO 查询，不进行 Flash 操作。停止返回失败时不重置仍被固件层引用的对象；已经提交的数据不撤销。
- [ ] 运行新旧 C 回归，确认同连接多图、反复连接、查询、取消和连接号复用；保留正常图片错误阶段诊断。

## Task 2: 转换、搜索和客户端传输

Files: Tools/send_picture.py, Tools/send_firmware.py, new Tools/picture_client.py,
new Tests/test_picture_client.py, Tests/test_picture_sender.py。

- [ ] 新增裁剪/拉伸以及专用应答识别测试，先观察未实现时失败。
- [ ] 转换接口增加 fit="contain"/"cover"/"stretch"，默认行为不变。
- [ ] 软件核心约定：
  ```python
  Device(host, port=8080, width=800, height=480, slots=256)
  discover_devices(port=8080, timeout=0.6, cancel=None, on_progress=None)
  send_to_device(device, data, slot, on_progress=None, on_status=None, cancel=None)
  ```
  progress 回调为 (已完成数量, 总数量)，发送 status 为连接/准备/正文/校验/完成阶段。
- [ ] Windows 读取活动网卡地址及真实前缀；搜索可取消，有范围上限，通过 INFO/EPD1 精确识别，结果去重。
- [ ] 复用 PIC1 传输并增加回调；取消能够及时结束 socket 应答等待，不把未收到 DONE 表述为已确认保存。
- [ ] CLI 增加 --host 参数以连接板端服务器，无 --host 时保留旧监听入口。
- [ ] 用本机 TCP 服务器验证分片、错误应答、异常 ACK、超时、取消和连续连接；运行原发送工具测试。

## Task 3: 中文窗口与一键启动

Files: new Tools/picture_sender_gui.pyw, new Tools/启动图片发送器.vbs,
new docs/picture_sender_gui.md。

- [ ] 创建原图和实际像素预览两栏，设备选择、搜索、适配方式、阈值、抖动、编号、发送、取消、进度和状态。
- [ ] 使用同一份 48000 字节作为预览和发送数据。网络与转换线程只投递结果，Tk 控件只由主线程更新；忽略已经过期的转换结果。
- [ ] 在用户应用数据目录记住选项。选择图片和设置变更更新预览，发送期间锁定相关输入。
- [ ] 提供双击启动入口，用 pythonw 启动中文窗口；不额外安装 GUI 依赖。
- [ ] 打开实际窗口核对布局与图片预览，通过本机模拟板验证搜索结果选择和发送状态。

## Task 4: 集成、审查和交付

- [ ] 两阶段审查：先逐项核对用户功能，再查连接竞态、取消和资源生命周期；修复有证据的问题。
- [ ] 运行完整回归：
  ```text
  python Tests/run_tests.py
  ```
  期望全部主机套件 PASS。
- [ ] Keil 编译 USER/LED_OSP.uvprojx，确认 0 Error(s)、0 Warning(s)。
- [ ] 生成并验证：
  ```text
  python Tools/make_flash_images.py
  ```
- [ ] 更新中文说明，包括首次烧录新版 App、搜索设备、适配方式、重复发送和保存/显示确认的区别。
- [ ] 交付一键启动入口和 OBJ/LED_OSP_openblt.hex，说明本机验证结果及仍需上板验证的部分。
