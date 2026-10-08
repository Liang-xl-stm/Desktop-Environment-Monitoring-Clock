r"""FWUP header -> READY -> 256-byte chunks/ACK -> DONE.

Usage: python send_firmware.py "C:\path\application.bin"
Stop NetAssist's port 8080 listener before starting this server.
The board connects as a TCP client; this script never writes local firmware.
For the board's TCP server: add --host BOARD_IP.
"""
import argparse
from pathlib import Path
import socket
import struct
import time
import zlib


def read_line(connection, timeout=180):
    """Read one bounded status line within a total deadline, even for slow bytes."""
    data = bytearray()
    deadline = time.monotonic() + timeout
    original_timeout = connection.gettimeout() if hasattr(connection, "gettimeout") else timeout
    try:
        while len(data) < 80:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Reply deadline expired")
            connection.settimeout(min(remaining, original_timeout) if original_timeout is not None else remaining)
            byte = connection.recv(1)
            if not byte:
                raise RuntimeError("Board disconnected before confirmation")
            data.extend(byte)
            if byte == b"\n":
                try:
                    line = data.decode("ascii").strip()
                except UnicodeDecodeError as exc:
                    raise RuntimeError("Board sent a non-ASCII reply") from exc
                if line.startswith("ERR"):
                    raise RuntimeError("Board reported: " + line)
                return line
        raise RuntimeError("Reply is too long")
    finally:
        connection.settimeout(original_timeout)


def send_image(connection, image):
    """Send one raw .bin after the board's Flash/baseline initialization."""
    crc = zlib.crc32(image) & 0xFFFFFFFF
    header = struct.pack("<IIII", 0x50555746, 1, len(image), crc)
    connection.settimeout(180)
    connection.sendall(header)
    if read_line(connection) != "READY":
        raise RuntimeError("Expected READY; remove the old TCP Hello message")
    print("Flash ready, bytes=%d, CRC32=%08X" % (len(image), crc))

    acknowledged = 0
    for offset in range(0, len(image), 256):
        chunk = image[offset:offset + 256]
        target = offset + len(chunk)
        connection.sendall(chunk)
        while acknowledged < target:
            line = read_line(connection)
            if not line.startswith("ACK "):
                raise RuntimeError("Expected cumulative ACK, got " + line)
            value = int(line[4:])
            if value <= acknowledged or value > target:
                raise RuntimeError("ACK progress is out of range")
            acknowledged = value
        if target % 16384 == 0 or target == len(image):
            print("Written: %d/%d" % (target, len(image)))

    if read_line(connection) != "DONE":
        raise RuntimeError("Missing complete-image confirmation")
    print("DONE: candidate stored and CRC verified; no installation performed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin_file", type=Path, help="Raw application .bin")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--host", help="Connect to the board's TCP server at this IPv4 address")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    size = args.bin_file.stat().st_size
    if not 1 <= size <= 0x100000:
        parser.error("Image size must be 1..1048576 bytes")
    image = args.bin_file.read_bytes()
    if len(image) != size:
        parser.error("File changed during reading")
    if not 1 <= args.port <= 65535:
        parser.error("Port must be 1..65535")
    if args.host:
        print("Connecting to %s:%d" % (args.host, args.port))
        with socket.create_connection((args.host, args.port), timeout=10) as connection:
            send_image(connection, image)
        return
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.bind, args.port))
        server.listen(1)
        print("Listening on %s:%d; now restart/connect the board" %
              (args.bind, args.port))
        connection, peer = server.accept()
        with connection:
            print("Board connected:", peer)
            send_image(connection, image)


if __name__ == "__main__":
    main()
