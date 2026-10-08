/** @file firmware_tcp_config.h
 * @brief TCP 升级接入及当前内部 App 分区配置。
 */
#ifndef FIRMWARE_TCP_CONFIG_H
#define FIRMWARE_TCP_CONFIG_H

/**
 * @brief 当前内部 App 分区容量，单位为字节。
 * @note 与 Keil Target 中 IROM1 的 Size 保持一致。Bootloader 使用前 128KiB，
 *       App 从 0x08020000 开始，可用容量 896KiB。
 *       映像地址和实际加载长度从链接器生成的符号取得，不手填 bin 长度。
 *
 * 片内 App 分区容量：896 KB，升级固件不能超过该容量。 
 */
#define FIRMWARE_APP_CAPACITY 0x000E0000UL

/**
 * @brief 1：仅两份管理记录均完全空白时，首次保存当前固件到 A 槽。
 * @note 0：禁止启动时首次建基线，须在调试阶段明确调用 CreateBaseline。
 *       已有有效记录、非空白损坏记录或读取错误都不会自动覆盖。
 *       首次操作会擦写 A 槽所需正文扇区和一个管理扇区，保留 B 正文。
 */
#ifndef FIRMWARE_ALLOW_INITIAL_BASELINE
#define FIRMWARE_ALLOW_INITIAL_BASELINE 1U
#endif

#if FIRMWARE_ALLOW_INITIAL_BASELINE != 0 && FIRMWARE_ALLOW_INITIAL_BASELINE != 1
#error FIRMWARE_ALLOW_INITIAL_BASELINE must be 0 or 1
#endif

/**
 * @brief 1：允许首次启动时清理本次已授权删除的两份管理区旧数据。
 * @note 仅在首次准备报 META_NOT_BLANK 时检查，且必须精确匹配已观测的
 *       16 字节旧数据指纹、META1 前60字节为空白。META1 的尾部旧数据
 *       先擦除并完整验证，再处理 META0。有效/损坏 FWMD、未知记录和
 *       读取错误均不会被自动覆盖。成功建基线后不会重复清理。
 *       0：禁用这条首次恢复入口；基线准备的原有保护规则不变。
 */
#ifndef FIRMWARE_CLEAR_LEGACY_METADATA
#define FIRMWARE_CLEAR_LEGACY_METADATA 1U
#endif

#if FIRMWARE_CLEAR_LEGACY_METADATA != 0 && FIRMWARE_CLEAR_LEGACY_METADATA != 1
#error FIRMWARE_CLEAR_LEGACY_METADATA must be 0 or 1
#endif

#endif
