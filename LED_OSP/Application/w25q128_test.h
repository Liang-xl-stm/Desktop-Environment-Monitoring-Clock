/** @file w25q128_test.h
 * @brief 应用层可选 Flash 擦写和掉电保存校验入口。
 */
#ifndef APP_W25Q128_TEST_H
#define APP_W25Q128_TEST_H

/**
 * @brief 按 w25q128_config.h 的模式进行自检。
 * @pre W25Q128 已绑定初始化后的 SPI 设备，仅在任务上下文调用。
 * @note 默认模式 0 不访问 Flash；模式 1 会改写 0x020000～0x023FFF。
 */
void w25q128_test(void);

#endif /* APP_W25Q128_TEST_H */
