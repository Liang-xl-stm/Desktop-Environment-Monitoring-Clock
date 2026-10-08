/**
 * @file epd_config.h
 * @brief 4.26 寸墨水屏接线和启动测试开关。
 * @note 屏幕 PWR 接 PF7；PF6 继续只作为 ESP8266 RST 使用。
 *       当前所有屏幕信号均在 GPIOF；更换端口时需同步修改底层 GPIO 初始化。
 */
#ifndef EPD_CONFIG_H
#define EPD_CONFIG_H

/* 1：创建持久屏幕任务；0：关闭任务，由应用直接操作驱动。 */
#ifndef EPD_TASK_ENABLE
#define EPD_TASK_ENABLE 1
#endif
/* 1：无启动图片时显示黑块；0：无图时不刷新。 */
#ifndef EPD_DEMO_ENABLE
#define EPD_DEMO_ENABLE 1
#endif

#define EPD_STARTUP_PICTURE 0U /* 重启显示的图片编号，0～255。 */

#define EPD_MOSI_PORT      GPIOF
#define EPD_MOSI_GPIO_PIN  GPIO_Pin_0 /* DIN：软件 SPI 数据输出。 */
#define EPD_SCK_PORT       GPIOF
#define EPD_SCK_GPIO_PIN   GPIO_Pin_1 /* CLK：软件 SPI 时钟。 */
#define EPD_CS_PORT        GPIOF
#define EPD_CS_GPIO_PIN    GPIO_Pin_2 /* CS：低电平选中。 */
#define EPD_DC_PORT        GPIOF
#define EPD_DC_GPIO_PIN    GPIO_Pin_3 /* DC：0 命令、1 数据。 */
#define EPD_RST_PORT       GPIOF
#define EPD_RST_GPIO_PIN   GPIO_Pin_4 /* RST：低电平复位。 */
#define EPD_BUSY_PORT      GPIOF
#define EPD_BUSY_GPIO_PIN  GPIO_Pin_5 /* BUSY：高电平表示忙。 */
#define EPD_PWR_PORT       GPIOF
#define EPD_PWR_GPIO_PIN   GPIO_Pin_7 /* PWR：高电平使能。 */

#endif
