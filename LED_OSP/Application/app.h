/** @file app.h @brief 应用任务初始化，硬件须提前准备好。 */
#ifndef APPLICATION_APP_H
#define APPLICATION_APP_H
#include <stdint.h>
/** 创建 LED、USART1 回显和 ESP8266 网络任务，以及配置启用的图片显示任务。
 * 无参数；成功返回 1，任意任务创建失败返回 0。仅在启动调度器前调用一次。
 */
uint8_t App_Init(void);
#endif
