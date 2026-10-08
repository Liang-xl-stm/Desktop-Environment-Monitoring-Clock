/** @file w25q128_config.h
 * @brief W25Q128 的可选自检配置。
 */
#ifndef W25Q128_CONFIG_H
#define W25Q128_CONFIG_H

/** @brief 0：关闭自检；1：擦写后校验；2：只校验此前模式 1 写入的数据。 */
#ifndef W25Q128_TEST_MODE
#define W25Q128_TEST_MODE 0
#endif

/** @brief 自检专用区域的起点；模式 1 会擦除这里开始的四个 4KB 扇区。 */
#define W25Q128_TEST_BASE 0x00020000UL

#if W25Q128_TEST_MODE < 0 || W25Q128_TEST_MODE > 2
#error W25Q128_TEST_MODE must be 0, 1 or 2
#endif

#endif /* W25Q128_CONFIG_H */
