/** @file w25q128_test.c
 * @brief W25Q128 的应用层自检；测试逻辑与设备协议分离。
 */
#include "w25q128_test.h"
#include "w25q128_config.h"
#include "w25q128.h"
#include <stdio.h>
#include <string.h>

void w25q128_test(void)
{
#if W25Q128_TEST_MODE != 0
    static const uint32_t offsets[3] = {0x100U, 0x1F0U, 0x2FE0U};
    uint8_t write_buffer[100];
    uint8_t read_buffer[100];

#if W25Q128_TEST_MODE == 1
    /* 仅模式 1 擦除专用测试区。 */
    for (uint32_t offset = 0; offset < 0x4000U; offset += 4096U)
    {
        if (!W25Q128_Erase_Sector(W25Q128_TEST_BASE + offset))
        {
            printf("Flash test erase failed\r\n");
            return;
        }
    }
#endif

    /* 单页、跨页、跨扇区：数据图案与原工程自检保持一致。 */
    for (uint32_t test_case = 0; test_case < 3U; test_case++)
    {
        uint32_t address = W25Q128_TEST_BASE + offsets[test_case];

        for (uint32_t i = 0; i < sizeof(write_buffer); i++)
        {
            write_buffer[i] = test_case == 0U ? (uint8_t)i :
                              test_case == 1U ? (uint8_t)(0x80U + i) : (uint8_t)(0xF0U ^ i);
        }

#if W25Q128_TEST_MODE == 1
        if (!W25Q128_Write(write_buffer, address, sizeof(write_buffer)))
        {
            printf("Flash case%u write failed\r\n", (unsigned int)(test_case + 1U));
            return;
        }
#endif
        memset(read_buffer, 0, sizeof(read_buffer));
        if (!W25Q128_Read(read_buffer, address, sizeof(read_buffer)))
        {
            printf("Flash case%u read failed\r\n", (unsigned int)(test_case + 1U));
            return;
        }
        printf("Flash case%u verify: %s\r\n", (unsigned int)(test_case + 1U),
               memcmp(write_buffer, read_buffer, sizeof(write_buffer)) == 0 ? "PASS" : "FAIL");
    }

    /* 回读最先写入的数据，检查跨页或跨扇区操作是否误改其它区域。 */
    for (uint32_t i = 0; i < sizeof(write_buffer); i++) write_buffer[i] = (uint8_t)i;
    if (W25Q128_Read(read_buffer, W25Q128_TEST_BASE + offsets[0], sizeof(read_buffer)))
    {
        printf("Flash preserved data: %s\r\n",
               memcmp(write_buffer, read_buffer, sizeof(write_buffer)) == 0 ? "PASS" : "FAIL");
    }
    else
    {
        printf("Flash preserved data read failed\r\n");
    }
#endif
}
