"""为 STM32F407 App 的 BIN 文件添加 OpenBLT 启动校验字。

用法：python add_openblt_checksum.py 固件文件.bin
前提：App 启动文件在向量表偏移 0x188 处已用 DCD 0 预留四字节。
此脚本修改 BIN，并生成 LED_OSP_openblt.hex；不修改 AXF 或 Keil 原始 HEX。
"""

import argparse
import struct
from pathlib import Path

CHECKSUM_OFFSET = 0x188  # 与 Bootloader 的校验字偏移保持一致。
APP_MAX_SIZE = 0xE0000   # App 可使用的片内 Flash：896 KB。
MASK32 = 0xFFFFFFFF     # 将计算结果限制为无符号 32 位整数。


def add_checksum(bin_path: Path) -> int:
    """计算并写入启动校验字，返回本次校验值。

    bin_path：Keil 刚生成的原始 BIN 文件路径。
    成功时只更新偏移 0x188 开始的四字节；文件大小不变。
    文件缺失、长度不合法或校验位置未预留时抛出异常。
    此校验仅用于 OpenBLT 判断启动向量有效性，不能替代整包 CRC32。
    """
    image = bytearray(bin_path.read_bytes())
    if not CHECKSUM_OFFSET + 4 <= len(image) <= APP_MAX_SIZE:
        raise ValueError("BIN 长度不符合 App 分区或校验字位置要求")

    # 按小端读取前七个 32 位向量：初始栈指针和六个异常入口。
    vectors = struct.unpack_from("<7I", image, 0)

    # 取七个向量之和的二进制补码，使加上校验字后的低 32 位为零。
    checksum = (-sum(vectors)) & MASK32
    current = struct.unpack_from("<I", image, CHECKSUM_OFFSET)[0]

    # 允许第一次处理零占位，或再次处理已写入相同校验值的文件。
    # 其他值说明文件可能未预留位置，停止以避免覆盖程序指令。
    if current not in (0, checksum):
        raise ValueError("偏移 0x188 未预留为零，或 BIN 文件已发生变化")

    struct.pack_into("<I", image, CHECKSUM_OFFSET, checksum)
    stored = struct.unpack_from("<I", image, CHECKSUM_OFFSET)[0]
    if (sum(vectors) + stored) & MASK32:
        raise ValueError("OpenBLT 启动校验失败")

    # 先完整写入同目录临时文件，再替换 BIN，减少写入中断造成的损坏。
    temporary = bin_path.with_name(bin_path.name + ".checksum.tmp")
    try:
        temporary.write_bytes(image)
        if temporary.read_bytes() != image:
            raise OSError("校验后的 BIN 文件读回验证失败")
        temporary.replace(bin_path)
    finally:
        if temporary.exists():
            temporary.unlink()
    return checksum


def main() -> None:
    """读取命令行中的 BIN 路径；失败时以非零状态退出，供 Keil 检测。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin_path", type=Path, help="Keil 生成的 App BIN 文件路径")
    args = parser.parse_args()
    try:
        checksum = add_checksum(args.bin_path)
        # 从已补齐校验字的 BIN 生成可供 ST-Link 烧录的 HEX。
        from make_flash_images import write_hex, APP_BASE
        write_hex(args.bin_path.with_name(args.bin_path.stem + "_openblt.hex"),
                  [(APP_BASE, args.bin_path.read_bytes())])
    except (OSError, ValueError) as error:
        parser.exit(1, f"OpenBLT checksum FAILED: {error}\n")
    print(f"OpenBLT checksum OK: 0x{checksum:08X}, file={args.bin_path}")


if __name__ == "__main__":
    main()
