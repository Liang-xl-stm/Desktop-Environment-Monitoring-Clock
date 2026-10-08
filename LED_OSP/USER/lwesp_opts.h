/**
 * \file            lwesp_opts_template.h
 * \brief           Template config file
 */

/*
 * Copyright (c) 2026 Tilen MAJERLE
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
 * AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * This file is part of LwESP - Lightweight ESP-AT parser library.
 *
 * Author:          Tilen MAJERLE <tilen@majerle.eu>
 * Version:         v1.1.2-dev
 */
#ifndef LWESP_OPTS_HDR_H
#define LWESP_OPTS_HDR_H

/* Rename this file to "lwesp_opts.h" for your application */

/*
 * Open "include/lwesp/lwesp_opt.h" and
 * copy & replace here settings you want to change values
 */

/* 1. 使用操作系统 (需配合 FreeRTOS 等 RTOS) */
#define LWESP_CFG_OS                        1

/* 2. 开启 ESP8266 支持 */
#define LWESP_CFG_ESP8266                   1

/* 3. 关闭 ESP32 及其他系列支持，节省空间 */
#define LWESP_CFG_ESP32                     0
#define LWESP_CFG_ESP32_C2                  0
#define LWESP_CFG_ESP32_C3                  0
#define LWESP_CFG_ESP32_C6                  0

/* 4. 输入数据处理模式：由接收任务直接将数据交给库处理 */
#define LWESP_CFG_INPUT_USE_PROCESS         1

/* 5. 初始化时不恢复模块已保存的设置，保持当前状态 */
#define LWESP_CFG_RESTORE_ON_INIT           0

#define LWESP_CFG_RESET_ON_INIT 			0

#endif /* LWESP_OPTS_HDR_H */
