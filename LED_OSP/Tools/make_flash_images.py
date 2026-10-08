"""生成含 OpenBLT 校验字的 App HEX 及 Bootloader+App 合并 HEX。

工程根目录运行 python Tools/make_flash_images.py。
只生成文件，不连接 ST-Link、不擦写开发板。
"""
from pathlib import Path
import argparse
import struct

APP_BASE = 0x08020000
BOOT_BASE = 0x08000000
ROOT = Path(__file__).resolve().parents[1]


def record(address, kind, data):
    raw = bytes([len(data), address >> 8, address & 255, kind]) + data
    return ":" + (raw + bytes([(-sum(raw)) & 255])).hex().upper()


def write_hex(path, regions):
    lines = []
    upper = None
    expected = {}
    for base, image in regions:
        for index in range(0, len(image), 16):
            address = base + index
            high = address >> 16
            if high != upper:
                lines.append(record(0, 4, high.to_bytes(2, "big")))
                upper = high
            data = image[index:index + 16]
            lines.append(record(address & 0xFFFF, 0, data))
            for offset, byte in enumerate(data):
                if address + offset in expected:
                    raise ValueError("映像地址重叠")
                expected[address + offset] = byte
    lines.append(record(0, 1, b""))
    text = "\n".join(lines) + "\n"
    # 校验 Intel HEX 每行校验和，并重建所有字节，避免地址或数据丢失。
    actual = {}
    high = 0
    for line in text.splitlines():
        raw = bytes.fromhex(line[1:])
        if sum(raw) & 255 or len(raw) != raw[0] + 5:
            raise ValueError("HEX 记录校验失败")
        if raw[3] == 4:
            high = int.from_bytes(raw[4:6], "big") << 16
        elif raw[3] == 0:
            address = high + int.from_bytes(raw[1:3], "big")
            for offset, byte in enumerate(raw[4:-1]):
                actual[address + offset] = byte
    if actual != expected:
        raise ValueError("HEX 数据读回校验失败")
    path.write_text(text, encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=ROOT / "OBJ/LED_OSP.bin")
    parser.add_argument("--boot", type=Path, default=ROOT.parent / "Bootloader/OBJ/Bootloader.bin")
    args = parser.parse_args()
    app = args.app.read_bytes()
    boot = args.boot.read_bytes()
    if not 0x18C <= len(app) <= 0xE0000 or not 8 <= len(boot) <= 0x20000:
        raise ValueError("映像长度超出分区")
    checksum = struct.unpack_from("<I", app, 0x188)[0]
    if (sum(struct.unpack_from("<7I", app)) + checksum) & 0xFFFFFFFF:
        raise ValueError("App 缺少有效的 OpenBLT 校验字，请先完整编译")
    for base, image in [(APP_BASE, app), (BOOT_BASE, boot)]:
        stack, reset = struct.unpack_from("<2I", image)
        if stack & 7 or not (0x20000000 < stack <= 0x20020000 or 0x10000000 < stack <= 0x10010000):
            raise ValueError("初始栈指针不合法")
        if not reset & 1 or not base <= (reset & ~1) < base + len(image):
            raise ValueError("复位入口不在对应映像内")
    output = ROOT / "OBJ"
    write_hex(output / "LED_OSP_openblt.hex", [(APP_BASE, app)])
    write_hex(output / "Bootloader_App.hex", [(BOOT_BASE, boot), (APP_BASE, app)])
    print(f"PASS flash images: Boot={len(boot)} bytes, App={len(app)} bytes, checksum=0x{checksum:08X}")
    print(output / "Bootloader_App.hex")


if __name__ == "__main__":
    main()
