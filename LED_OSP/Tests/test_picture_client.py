"""Local socket integration tests; discovery never scans real network adapters."""
from contextlib import AbstractContextManager
import inspect
import ipaddress
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Tools"))
import send_firmware
import send_picture
try:
    import picture_client as client
except ImportError:
    client = None

PIXELS = bytes(range(256)) * 187 + bytes(range(128))


def receive_exact(connection, length):
    data = bytearray()
    while len(data) < length:
        packet = connection.recv(length - len(data))
        if not packet:
            raise AssertionError("Client disconnected prematurely")
        data.extend(packet)
    return bytes(data)


class LocalBoard(AbstractContextManager):
    def __init__(self, handler, connections=1):
        self.handler = handler
        self.connections = connections
        self.error = None
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(32)
        self.listener.settimeout(3)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        try:
            for _ in range(self.connections):
                connection, _ = self.listener.accept()
                with connection:
                    connection.settimeout(3)
                    self.handler(connection)
        except BaseException as exc:
            self.error = exc

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, kind, value, traceback):
        self.thread.join(4)
        self.listener.close()
        if self.thread.is_alive():
            raise AssertionError("Local test server did not stop")
        if self.error and kind is None:
            raise self.error


def accept_picture(connection, expected_slot=0, fragmented=False):
    header = struct.unpack("<8I", receive_exact(connection, 32))
    assert header == (0x31434950, 1, expected_slot, 800, 480, 48000,
                      zlib.crc32(PIXELS) & 0xFFFFFFFF, 0)
    def reply(data):
        if fragmented:
            for byte in data:
                connection.sendall(bytes([byte]))
        else:
            connection.sendall(data)
    reply(b"READY\r\n")
    received = bytearray()
    while len(received) < 48000:
        amount = min(256, 48000 - len(received))
        received.extend(receive_exact(connection, amount))
        assert received == PIXELS[:len(received)]
        response = f"ACK {len(received)}\r\n".encode()
        if len(received) == 48000:
            response += b"DONE\r\n"
        reply(response)


class PictureClientTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(client, "picture_client API is not implemented")

    def test_device_defaults(self):
        device = client.Device("192.168.43.9")
        self.assertEqual((device.port, device.width, device.height, device.slots), (8080, 800, 480, 256))

    def test_exact_info_identity_with_fragmented_response(self):
        def respond(connection):
            self.assertEqual(receive_exact(connection, 2), b"IN")
            self.assertEqual(receive_exact(connection, 2), b"FO")
            for byte in b"EPD1 800 480 256\r\n":
                connection.sendall(bytes([byte]))
        with LocalBoard(respond) as board:
            self.assertEqual(client.probe_device("127.0.0.1", board.port), client.Device("127.0.0.1", board.port))

    def test_foreign_port_is_never_a_device(self):
        for response in (b"HTTP/1.1 200 OK\r\n", b"EPD1 800 480 255\r\n",
                         b" EPD1 800 480 256\r\n", b"EPD1 800 480 256\n", b"ERR query\r\n"):
            with self.subTest(response=response):
                def respond(connection):
                    receive_exact(connection, 4)
                    connection.sendall(response)
                with LocalBoard(respond) as board:
                    self.assertIsNone(client.probe_device("127.0.0.1", board.port))

    def test_repeated_connections_send_without_reset(self):
        progress, statuses = [], []
        with LocalBoard(lambda sock: accept_picture(sock, 7, fragmented=True), connections=2) as board:
            device = client.Device("127.0.0.1", board.port)
            for _ in range(2):
                client.send_to_device(device, PIXELS, 7,
                                      on_progress=lambda done, total: progress.append((done, total)),
                                      on_status=statuses.append)
        self.assertEqual(statuses, ["connecting", "preparing", "sending", "verifying", "done"] * 2)
        self.assertEqual(progress[0], (0, 48000))
        self.assertEqual(progress[-1], (48000, 48000))

    def test_errors_and_invalid_ack_never_report_done(self):
        for response, stage in ((b"ERR flash\r\n", "header"), (b"ACK 257\r\n", "body"),
                                (b"ACK 0\r\n", "body"), (b"ACK bad\r\n", "body")):
            with self.subTest(response=response):
                statuses = []
                def reject(connection):
                    receive_exact(connection, 32)
                    if stage == "body":
                        connection.sendall(b"READY\r\n")
                        receive_exact(connection, 256)
                    connection.sendall(response)
                with LocalBoard(reject) as board:
                    with self.assertRaises((RuntimeError, ValueError)):
                        client.send_to_device(client.Device("127.0.0.1", board.port), PIXELS, 0, on_status=statuses.append)
                self.assertNotIn("done", statuses)

    def test_disconnect_before_done_does_not_confirm_storage(self):
        statuses = []
        def incomplete(connection):
            receive_exact(connection, 32)
            connection.sendall(b"READY\r\n")
            received = 0
            while received < 48000:
                received += len(receive_exact(connection, min(256, 48000 - received)))
                connection.sendall(f"ACK {received}\r\n".encode())
        with LocalBoard(incomplete) as board:
            with self.assertRaisesRegex(RuntimeError, "confirmation"):
                client.send_to_device(client.Device("127.0.0.1", board.port), PIXELS, 0, on_status=statuses.append)
        self.assertNotIn("done", statuses)

    def test_cancel_during_ready_wait_closes_connection_quickly(self):
        cancel = threading.Event()
        received = threading.Event()
        statuses = []
        def silent(connection):
            receive_exact(connection, 32)
            received.set()
            self.assertEqual(connection.recv(1), b"")
        with LocalBoard(silent) as board:
            timer = threading.Thread(target=lambda: (received.wait(1), cancel.set()), daemon=True)
            timer.start()
            started = time.monotonic()
            with self.assertRaises(client.TransferCancelled):
                client.send_to_device(client.Device("127.0.0.1", board.port), PIXELS, 0, cancel=cancel, on_status=statuses.append)
            self.assertLess(time.monotonic() - started, 1)
            timer.join(1)
        self.assertNotIn("done", statuses)

    def test_cancel_during_body_and_commit_wait(self):
        for stage in ("ACK", "DONE"):
            with self.subTest(stage=stage):
                cancel, ready_to_cancel = threading.Event(), threading.Event()
                statuses = []
                def pause(connection):
                    receive_exact(connection, 32)
                    connection.sendall(b"READY\r\n")
                    if stage == "ACK":
                        receive_exact(connection, 256)
                    else:
                        received = 0
                        while received < 48000:
                            received += len(receive_exact(connection, min(256, 48000 - received)))
                            connection.sendall(f"ACK {received}\r\n".encode())
                    ready_to_cancel.set()
                    try:
                        self.assertEqual(connection.recv(1), b"")
                    except ConnectionResetError:
                        pass  # Closing before consuming the final ACK may send TCP RST.
                with LocalBoard(pause) as board:
                    trigger = threading.Thread(target=lambda: (ready_to_cancel.wait(1), cancel.set()), daemon=True)
                    trigger.start()
                    started = time.monotonic()
                    with self.assertRaises(client.TransferCancelled):
                        client.send_to_device(client.Device("127.0.0.1", board.port), PIXELS, 0,
                                              cancel=cancel, on_status=statuses.append)
                    self.assertLess(time.monotonic() - started, 1)
                    trigger.join(1)
                self.assertNotIn("done", statuses)

    def test_connect_wait_is_cancellable_without_real_network(self):
        class PendingSocket:
            closed = False
            def setblocking(self, value):
                pass
            def setsockopt(self, *values):
                pass
            def connect_ex(self, address):
                return client.errno.EINPROGRESS
            def close(self):
                self.closed = True
        connection = PendingSocket()
        cancel = threading.Event()
        def no_ready(*args):
            time.sleep(0.01)
            return [], [], []
        trigger = threading.Timer(0.05, cancel.set)
        trigger.start()
        with patch.object(client.socket, "socket", return_value=connection), patch.object(client.select, "select", side_effect=no_ready):
            started = time.monotonic()
            with self.assertRaises(client.TransferCancelled):
                client.send_to_device(client.Device("192.168.1.2"), PIXELS, 0, cancel=cancel)
            self.assertLess(time.monotonic() - started, 1)
        trigger.join(1)
        self.assertTrue(connection.closed)

    def test_blocked_socket_send_is_cancellable(self):
        # 不依赖操作系统 TCP 缓冲区是否能瞬间接收大块数据；模拟一次已
        # 可写、但下一次轮询前被取消的非阻塞发送。
        cancel = threading.Event()

        class WritableSocket:
            def send(self, data):
                cancel.set()
                return 1

            def close(self):
                pass

        fake = WritableSocket()
        with patch.object(client.select, "select", return_value=([], [fake], [])):
            with self.assertRaises(client.TransferCancelled):
                client._SocketConnection(fake, cancel, 180).sendall(b"ab")

    def test_probe_timeout_is_total_even_for_slow_reply(self):
        def slow(connection):
            receive_exact(connection, 4)
            for byte in b"EPD1 800 480 256\r\n":
                try:
                    connection.sendall(bytes([byte]))
                    time.sleep(0.035)
                except (ConnectionError, OSError):
                    break
        with LocalBoard(slow) as board:
            started = time.monotonic()
            self.assertIsNone(client.probe_device("127.0.0.1", board.port, timeout=0.13))
            self.assertLess(time.monotonic() - started, 0.4)

    def test_read_line_timeout_is_total_for_slow_bytes(self):
        self.assertIn("timeout", inspect.signature(send_firmware.read_line).parameters)
        def slow(connection):
            for byte in b"READY\r\n":
                try:
                    connection.sendall(bytes([byte]))
                    time.sleep(0.035)
                except (ConnectionError, OSError):
                    break
        with LocalBoard(slow) as board:
            with socket.create_connection(("127.0.0.1", board.port)) as connection:
                started = time.monotonic()
                with self.assertRaises(TimeoutError):
                    send_firmware.read_line(connection, timeout=0.12)
                self.assertLess(time.monotonic() - started, 0.35)

    def test_uses_real_windows_prefix_and_ignores_nonlocal_adapters(self):
        addresses = [
            {"Address": "192.168.43.21", "PrefixLength": 28},
            {"Address": "192.168.43.22", "PrefixLength": 28},
            {"Address": "10.44.7.8", "PrefixLength": 16},
            {"Address": "127.0.0.1", "PrefixLength": 8},
            {"Address": "169.254.1.2", "PrefixLength": 16},
            {"Address": "8.8.8.8", "PrefixLength": 24},
        ]
        with patch.object(client, "_read_active_ipv4", return_value=addresses):
            networks = client.get_search_networks()
        self.assertEqual(networks, [ipaddress.ip_network("192.168.43.16/28"), ipaddress.ip_network("10.44.7.0/24")])
        hosts = client.candidate_hosts(networks)
        self.assertIn("192.168.43.17", hosts)
        self.assertNotIn("192.168.43.15", hosts)
        self.assertNotIn("192.168.43.32", hosts)
        self.assertEqual(len(hosts), 14 + 254)

    def test_windows_adapter_reader_uses_hidden_absolute_powershell(self):
        class Process:
            returncode = 0
            def __enter__(self):
                return self
            def __exit__(self, *args):
                pass
            def communicate(self, **options):
                return '[{"Address":"192.168.43.21","PrefixLength":28}]', ''
        with patch.object(client.subprocess, "Popen", return_value=Process()) as launch:
            self.assertEqual(client._read_active_ipv4(), [{"Address": "192.168.43.21", "PrefixLength": 28}])
        args, options = launch.call_args
        self.assertTrue(os.path.isabs(args[0][0]), "Adapter reader must not depend on PowerShell being in PATH")
        self.assertEqual(options["creationflags"], subprocess.CREATE_NO_WINDOW)
        self.assertIn("NetAdapter.Status -eq 'Up'", args[0][-1])
        self.assertIn("$entry.PrefixLength", args[0][-1])

    def test_candidate_scan_is_bounded_and_deduplicated(self):
        networks = [ipaddress.ip_network("10.0.0.0/20"), ipaddress.ip_network("10.0.0.0/24"),
                    ipaddress.ip_network("192.168.0.0/20")]
        hosts = client.candidate_hosts(networks)
        self.assertLessEqual(len(hosts), 4096)
        self.assertEqual(len(hosts), len(set(hosts)))

    def test_discovery_uses_info_and_progress_without_scanning_real_network(self):
        progress = []
        def respond(connection):
            self.assertEqual(receive_exact(connection, 4), b"INFO")
            connection.sendall(b"EPD1 800 480 256\r\n")
        with LocalBoard(respond) as board:
            with patch.object(client, "get_search_networks", return_value=[ipaddress.ip_network("127.0.0.1/32")]):
                devices = client.discover_devices(port=board.port, on_progress=lambda done, total: progress.append((done, total)))
        self.assertEqual(devices, [client.Device("127.0.0.1", board.port)])
        self.assertEqual(progress, [(0, 1), (1, 1)])

    def test_cancelled_operations_do_not_open_connections(self):
        cancel = threading.Event()
        cancel.set()
        with self.assertRaises(client.TransferCancelled):
            client.probe_device("127.0.0.1", cancel=cancel)
        with self.assertRaises(client.TransferCancelled):
            client.send_to_device(client.Device("127.0.0.1"), PIXELS, 0, cancel=cancel)
        with self.assertRaises(client.TransferCancelled):
            client.discover_devices(cancel=cancel)

    def test_discovery_cancellation_limits_workers_and_drains_all_threads(self):
        cancel, started = threading.Event(), threading.Event()
        lock = threading.Lock()
        active = maximum_active = 0
        def pause(host, port, timeout, combined_cancel):
            nonlocal active, maximum_active
            with lock:
                active += 1
                maximum_active = max(maximum_active, active)
                started.set()
            try:
                while not combined_cancel.is_set():
                    time.sleep(0.01)
                raise client.TransferCancelled("Test search cancelled")
            finally:
                with lock:
                    active -= 1
        trigger = threading.Thread(target=lambda: (started.wait(1), cancel.set()), daemon=True)
        trigger.start()
        with patch.object(client, "get_search_networks", return_value=[]), \
             patch.object(client, "candidate_hosts", return_value=[f"192.168.1.{i}" for i in range(1, 100)]), \
             patch.object(client, "probe_device", side_effect=pause):
            before = time.monotonic()
            with self.assertRaises(client.TransferCancelled):
                client.discover_devices(cancel=cancel)
            self.assertLess(time.monotonic() - before, 1)
        trigger.join(1)
        self.assertLessEqual(maximum_active, 32)
        self.assertEqual(active, 0)

    def test_picture_cli_host_accepts_two_images_on_one_connection(self):
        self.assertIn("--host", subprocess.run([sys.executable, str(Path(send_picture.__file__)), "--help"], capture_output=True, text=True, check=True).stdout)
        def receive_two(connection):
            accept_picture(connection, 2)
            accept_picture(connection, 3)
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "图片.bin"
            path.write_bytes(PIXELS)
            with LocalBoard(receive_two) as board:
                result = subprocess.run([sys.executable, "-W", "error", str(Path(send_picture.__file__)), str(path), str(path),
                                         "--host", "127.0.0.1", "--port", str(board.port), "--slot", "2",
                                         "--output-dir", str(Path(name) / "output")], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.count("DONE:"), 2)

    def test_firmware_cli_host_keeps_fwup_protocol(self):
        self.assertIn("--host", subprocess.run([sys.executable, str(Path(send_firmware.__file__)), "--help"], capture_output=True, text=True, check=True).stdout)
        payload = b"firmware test"
        def receive_firmware(connection):
            self.assertEqual(struct.unpack("<4I", receive_exact(connection, 16)),
                             (0x50555746, 1, len(payload), zlib.crc32(payload) & 0xFFFFFFFF))
            connection.sendall(b"READY\r\n")
            self.assertEqual(receive_exact(connection, len(payload)), payload)
            connection.sendall(f"ACK {len(payload)}\r\nDONE\r\n".encode())
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "firmware.bin"
            path.write_bytes(payload)
            with LocalBoard(receive_firmware) as board:
                result = subprocess.run([sys.executable, "-W", "error", str(Path(send_firmware.__file__)), str(path),
                                         "--host", "127.0.0.1", "--port", str(board.port)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("DONE:", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
