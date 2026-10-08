"""Pixel layout and stop/wait network checks without accessing the board."""
from pathlib import Path
import sys
import tempfile
import struct
import zlib
import subprocess
import inspect
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Tools"))
from PIL import Image
import send_picture as sender

class Board:
    def __init__(self, data, slot):
        self.data = data
        self.slot = slot
        self.header = None
        self.written = bytearray()
        self.replies = bytearray()
    def settimeout(self, seconds):
        assert seconds >= 30
    def sendall(self, data):
        assert not self.replies
        if self.header is None:
            self.header = struct.unpack("<8I", data)
            assert self.header == (0x31434950, 1, self.slot, 800, 480, 48000,
                                   zlib.crc32(self.data) & 0xFFFFFFFF, 0)
            self.replies += b"READY\r\n"
        else:
            before = len(self.written)
            assert 0 < len(data) <= 256
            self.written += data
            assert data == self.data[before:len(self.written)]
            if len(data) > 1:
                self.replies += f"ACK {before + 1}\r\n".encode()
            self.replies += f"ACK {len(self.written)}\r\n".encode()
            if len(self.written) == 48000:
                self.replies += b"DONE\r\n"
    def recv(self, n):
        assert n == 1 and self.replies
        byte = bytes(self.replies[:1]); del self.replies[:1]; return byte

with tempfile.TemporaryDirectory() as name:
    directory = Path(name)
    image = Image.new("RGB", (800, 480), "white")
    image.paste("black", (0, 0, 8, 480))
    image.save(directory / "bands.png")
    data = sender.prepare_image(directory / "bands.png")
    assert len(data) == 48000
    for y in range(480):
        assert data[y * 100] == 0 and data[y * 100 + 1:y * 100 + 100] == b"\xff" * 99
    # Transparent pixels and aspect-ratio borders become white, not black.
    Image.new("RGBA", (10, 10), (0, 0, 0, 0)).save(directory / "alpha.png")
    assert sender.prepare_image(directory / "alpha.png") == b"\xff" * 48000
    Image.new("RGB", (100, 100), "black").save(directory / "square.png")
    fitted = sender.prepare_image(directory / "square.png")
    assert fitted[0] == 255 and fitted[50] == 0 and fitted[99] == 255
    # All GUI fit modes must use the exact same one-bit output as the preview.
    assert "fit" in inspect.signature(sender.prepare_image).parameters, "Image fit modes are missing"
    assert sender.prepare_image(directory / "square.png", fit="cover") == b"\x00" * 48000
    assert sender.prepare_image(directory / "square.png", fit="stretch") == b"\x00" * 48000
    panorama = Image.new("RGB", (1600, 480), "white")
    panorama.paste("black", (400, 0, 1200, 480))
    panorama.save(directory / "裁剪图.png")
    cropped = Image.frombytes("1", (800, 480), sender.prepare_image(directory / "裁剪图.png", fit="cover"))
    stretched = Image.frombytes("1", (800, 480), sender.prepare_image(directory / "裁剪图.png", fit="stretch"))
    assert cropped.getpixel((20, 240)) == 0
    assert stretched.getpixel((20, 240)) == 255 and stretched.getpixel((400, 240)) == 0
    oriented = Image.new("RGB", (480, 800), "white")
    oriented.paste("black", (0, 0, 480, 100))
    exif = Image.Exif()
    exif[274] = 6
    oriented.save(directory / "旋转图.png", exif=exif)
    rotated = Image.frombytes("1", (800, 480), sender.prepare_image(directory / "旋转图.png"))
    assert rotated.getpixel((750, 240)) == 0 and rotated.getpixel((50, 240)) == 255
    try:
        sender.prepare_image(directory / "square.png", fit="invalid")
        raise AssertionError("Invalid fit mode accepted")
    except ValueError:
        pass
    (directory / "raw.bin").write_bytes(data)
    assert sender.prepare_image(directory / "raw.bin") == data
    (directory / "short.bin").write_bytes(b"\x00")
    try:
        sender.prepare_image(directory / "short.bin")
        raise AssertionError("Short image accepted")
    except ValueError:
        pass
    board = Board(data, 255)
    sender.send_picture(board, data, 255)
    assert bytes(board.written) == data and not board.replies
    # A silent board must report exactly which acknowledgement stalled.
    class SilentBodyBoard(Board):
        def sendall(self, packet):
            if self.header is None:
                super().sendall(packet)
            else:
                self.written += packet
        def recv(self, count):
            if self.replies:
                return super().recv(count)
            raise TimeoutError("timed out")
    try:
        sender.send_picture(SilentBodyBoard(data, 0), data, 0)
        raise AssertionError("Silent board accepted")
    except RuntimeError as exc:
        assert "ACK 256" in str(exc) and "0/48000" in str(exc)
    try:
        sender.send_picture(board, data, 256)
        raise AssertionError("Invalid slot accepted")
    except ValueError:
        pass
    # A dot in a source filename must not remove its slot suffix or collide with other outputs.
    image.save(directory / "first.multi.png")
    image.save(directory / "second.multi.png")
    subprocess.run([sys.executable, str(Path(sender.__file__)), str(directory / "first.multi.png"),
                    str(directory / "second.multi.png"), "--slot", "5", "--prepare-only",
                    "--output-dir", str(directory / "out")], check=True)
    assert (directory / "out/first.multi_slot005.bin").read_bytes() == data
    assert (directory / "out/second.multi_slot006.bin").read_bytes() == data
print("PASS picture sender: MSB/white format, transparency, fit, raw input, fragmented ACK/DONE")
