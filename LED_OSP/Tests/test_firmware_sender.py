"""Verify the PC sender without opening a network connection or touching hardware."""
import importlib.util
from pathlib import Path
import struct
import zlib

spec = importlib.util.spec_from_file_location(
    "firmware_sender", Path(__file__).resolve().parents[1] / "Tools" / "send_firmware.py")
sender = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)


class FakeBoard:
    def __init__(self, image):
        self.image = image
        self.header = None
        self.written = bytearray()
        self.replies = bytearray()

    def settimeout(self, seconds):
        assert seconds > 0

    def sendall(self, payload):
        if self.header is None:
            self.header = struct.unpack("<IIII", payload)
            assert self.header == (0x50555746, 1, len(self.image),
                                   zlib.crc32(self.image) & 0xFFFFFFFF)
            self.replies.extend(b"READY\r\n")
            return
        assert not self.replies, "Sent another chunk before consuming ACKs"
        assert 0 < len(payload) <= 256
        old = len(self.written)
        self.written.extend(payload)
        assert self.image[old:len(self.written)] == payload
        if len(payload) > 1:
            self.replies.extend(("ACK %d\r\n" % (old + 1)).encode("ascii"))
        self.replies.extend(("ACK %d\r\n" % len(self.written)).encode("ascii"))
        if len(self.written) == len(self.image):
            self.replies.extend(b"DONE\r\n")

    def recv(self, size):
        assert size == 1 and self.replies
        byte = bytes(self.replies[:1])
        del self.replies[:1]
        return byte


for size in (1, 255, 256, 257, 2003):
    image = bytes(i % 251 for i in range(size))
    board = FakeBoard(image)
    sender.send_image(board, image)
    assert bytes(board.written) == image and not board.replies
print("PASS sender: 5 image sizes, fragmented ACKs, final ACK/DONE coalescing")
