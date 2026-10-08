"""Exercise the real window's preview and two sends to a local TCP board, without hardware."""
from pathlib import Path
import importlib.machinery
import importlib.util
import socket
import struct
import sys
import tempfile
import threading
import time
import tkinter as tk
import zlib
from PIL import Image, ImageStat, ImageTk
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "Tools/picture_sender_gui.pyw"
assert ENTRY.is_file(), "Chinese picture sender window is not implemented"
sys.path.insert(0, str(ROOT / "Tools"))
loader = importlib.machinery.SourceFileLoader("picture_sender_gui", str(ENTRY))
spec = importlib.util.spec_from_loader(loader.name, loader)
gui = importlib.util.module_from_spec(spec)
loader.exec_module(gui)
from picture_client import Device


def wait_for(root, predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while not predicate():
        assert time.monotonic() < deadline, "Window did not finish its background work"
        root.update()
        time.sleep(0.01)
    root.update()


class Board:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(2)
        self.listener.settimeout(15)
        self.port = self.listener.getsockname()[1]
        self.images = []
        self.errors = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    @staticmethod
    def exact(connection, length):
        data = bytearray()
        while len(data) < length:
            block = connection.recv(length - len(data))
            assert block, "Sender disconnected early"
            data.extend(block)
        return bytes(data)

    def run(self):
        try:
            for _ in range(2):
                connection, _ = self.listener.accept()
                with connection:
                    connection.settimeout(10)
                    connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    header = struct.unpack("<8I", self.exact(connection, 32))
                    assert header[:6] == (0x31434950, 1, 7, 800, 480, 48000)
                    connection.sendall(b"READY\r\n")
                    body = bytearray()
                    while len(body) < 48000:
                        body.extend(self.exact(connection, min(256, 48000 - len(body))))
                        connection.sendall(f"ACK {len(body)}\r\n".encode())
                    assert zlib.crc32(body) & 0xFFFFFFFF == header[6]
                    self.images.append(bytes(body))
                    connection.sendall(b"DONE\r\n")
        except Exception as exc:
            self.errors.append(exc)
        finally:
            self.listener.close()


with tempfile.TemporaryDirectory() as directory:
    root = tk.Tk()
    root.withdraw()
    app = gui.PictureSenderApp(root, start_search=False,
                               settings_path=Path(directory) / "settings.json")
    try:
        # A 50% black dither pattern must stay mid-gray in a scaled overview.
        # Point sampling at half size used to select only white pixels.
        pixels = b"".join(bytes([0xAA if row % 2 == 0 else 0x55]) * 100
                          for row in range(480))
        app.preview_image = Image.frombytes("1", (800, 480), pixels)
        app.picture_data = pixels
        for width, height in ((400, 240), (1072, 643)):
            with patch.object(app.preview_canvas, "winfo_width", return_value=width + 18), \
                 patch.object(app.preview_canvas, "winfo_height", return_value=height + 18):
                app._draw_images()
            shown = ImageTk.getimage(app._photo_preview).convert("L")
            mean = ImageStat.Stat(shown).mean[0]
            assert 126 <= mean <= 129, f"Dither overview changed 50% gray to {mean:.2f}"
            blended = sum(shown.histogram()[1:255])
            assert blended > shown.width * shown.height * 0.9, \
                "Scaled overview must blend tiny dots rather than emphasize individual pixels"
            assert app.preview_image.tobytes() == app.picture_data == pixels, \
                "Overview rendering changed the original upload pixels"
        app.show_full_preview()
        native_window = next(child for child in root.winfo_children() if isinstance(child, tk.Toplevel))
        native_label = native_window.winfo_children()[0]
        native = ImageTk.getimage(native_label.image).convert("1")
        assert native.tobytes() == pixels, "Native pixel view must remain an exact upload preview"
    finally:
        # Destroying this first test window must not leave its polling callback
        # queued when the second Tk interpreter begins processing events.
        for callback in root.tk.call("after", "info"):
            root.after_cancel(callback)
        app.close()
print("PASS scaled dither overview: gray coverage preserved, native pixels unchanged")


with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    source = Image.new("RGB", (800, 480), (96, 96, 96))
    source.paste((200, 200, 200), (400, 0, 800, 480))
    source.save(directory / "test.png")
    root = tk.Tk()
    root.withdraw()
    app = gui.PictureSenderApp(root, start_search=False, settings_path=directory / "settings.json")
    board = Board()
    try:
        app.dither_var.set(False)
        app.threshold_var.set(128)
        app.slot_var.set(7)
        app.set_devices([Device("127.0.0.1", board.port)])
        app.load_picture(directory / "test.png")
        wait_for(root, lambda: app.picture_data is not None and not app.preview_busy)
        # A disabled threshold must not prevent the independent dither mode.
        app.threshold_var.set("")
        app.dither_var.set(True)
        app.schedule_preview()
        wait_for(root, lambda: not app.preview_busy)
        assert app.picture_data is not None, "Dither mode is blocked by its disabled threshold"
        app.dither_var.set(False)
        app.threshold_var.set(128)
        app.schedule_preview()
        wait_for(root, lambda: app.picture_data is not None and not app.preview_busy)
        # Fractional slot input must never silently overwrite the truncated slot.
        app.slot_var.set("7.9")
        app.start_send()
        assert app.operation is None, "Fractional slot started an upload to a different slot"
        assert "整数" in app.status_var.get()
        app.slot_var.set(7)
        first = app.picture_data
        assert first == app.preview_image.tobytes(), "Preview differs from the upload pixels"
        app.start_send()
        wait_for(root, lambda: app.operation is None)
        assert "保存完成" in app.status_var.get(), app.status_var.get()
        app.threshold_var.set(220)
        app.schedule_preview()
        wait_for(root, lambda: app.picture_data is not None and not app.preview_busy)
        second = app.picture_data
        assert first != second and second == app.preview_image.tobytes()
        app.start_send()
        wait_for(root, lambda: app.operation is None)
        assert "保存完成" in app.status_var.get(), app.status_var.get()
        board.thread.join(2)
        assert not board.errors, board.errors
        assert board.images == [first, second], "Repeated uploads did not use the shown preview"
    finally:
        app.close()
    assert (directory / "settings.json").is_file()
print("PASS real GUI: preview pixels match uploads, changed settings, two independent connections")
