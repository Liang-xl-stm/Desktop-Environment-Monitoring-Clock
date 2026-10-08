"""Tk-independent board discovery and cancellable PIC1 client transport.

Discovery reads only active Windows IPv4 adapters, respects their actual prefix,
and identifies devices by the exact INFO reply. Large networks are restricted to
the adapter's containing /24; GUI callers can display get_search_networks().
All discovery scans are bounded to 4096 hosts and 32 concurrent connections.
"""
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from dataclasses import dataclass
import errno
import ipaddress
import json
import os
import select
import socket
import subprocess
import threading
import time

from send_picture import DATA_SIZE, HEIGHT, SLOT_COUNT, WIDTH, send_picture

MAX_HOSTS = 4096
MAX_WORKERS = 32
POLL_INTERVAL = 0.15
IDENTITY = b"EPD1 800 480 256\r\n"
PRIVATE_NETWORKS = tuple(ipaddress.ip_network(value) for value in
                         ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


@dataclass(frozen=True)
class Device:
    host: str
    port: int = 8080
    width: int = WIDTH
    height: int = HEIGHT
    slots: int = SLOT_COUNT


class TransferCancelled(RuntimeError):
    """The caller cancelled a discovery or transfer before its confirmation."""


def _check_cancel(cancel):
    if cancel is not None and cancel.is_set():
        raise TransferCancelled("Operation cancelled")


class _CombinedCancel:
    def __init__(self, external):
        self.external = external
        self.local = threading.Event()

    def is_set(self):
        return self.local.is_set() or (self.external is not None and self.external.is_set())


class _SocketConnection:
    """Nonblocking socket facade with a bounded, cancellable wait on every I/O."""
    def __init__(self, connection, cancel, timeout):
        self.connection = connection
        self.cancel = cancel
        self.timeout = timeout

    def settimeout(self, seconds):
        self.timeout = seconds

    def gettimeout(self):
        return self.timeout

    def _wait(self, deadline, writing=False):
        while True:
            _check_cancel(self.cancel)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Socket operation deadline expired")
            readers = [] if writing else [self.connection]
            writers = [self.connection] if writing else []
            ready_read, ready_write, errors = select.select(
                readers, writers, [self.connection], min(POLL_INTERVAL, remaining))
            _check_cancel(self.cancel)
            if errors:
                error = self.connection.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                raise OSError(error, os.strerror(error))
            if ready_read or ready_write:
                return

    def recv(self, size):
        deadline = time.monotonic() + self.timeout
        while True:
            self._wait(deadline)
            try:
                return self.connection.recv(size)
            except BlockingIOError:
                continue

    def sendall(self, data):
        deadline = time.monotonic() + self.timeout
        remaining = memoryview(data)
        while remaining:
            self._wait(deadline, writing=True)
            try:
                sent = self.connection.send(remaining)
            except BlockingIOError:
                continue
            if not sent:
                raise RuntimeError("Board disconnected before confirmation")
            remaining = remaining[sent:]

    def close(self):
        self.connection.close()


def _connect(host, port, timeout, cancel):
    _check_cancel(cancel)
    # Manual entry is an IPv4 address; avoiding hostname resolution also makes
    # cancellation independent of a potentially slow OS DNS request.
    address = str(ipaddress.IPv4Address(host))
    if not 1 <= port <= 65535 or timeout <= 0:
        raise ValueError("Invalid port or connection timeout")
    connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        connection.setblocking(False)
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        result = connection.connect_ex((address, port))
        pending = {0, errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EALREADY,
                   getattr(errno, "WSAEWOULDBLOCK", 10035), getattr(errno, "WSAEINPROGRESS", 10036)}
        if result not in pending:
            raise OSError(result, os.strerror(result))
        wrapped = _SocketConnection(connection, cancel, timeout)
        if result:
            wrapped._wait(time.monotonic() + timeout, writing=True)
            result = connection.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            if result:
                raise OSError(result, os.strerror(result))
        _check_cancel(cancel)
        return wrapped
    except BaseException:
        connection.close()
        raise


def probe_device(host, port=8080, timeout=1.2, cancel=None):
    """Return a Device only after its exact INFO reply; refused/foreign ports return None."""
    _check_cancel(cancel)
    connection = None
    deadline = time.monotonic() + timeout
    try:
        connection = _connect(host, port, timeout, cancel)
        connection.settimeout(max(0, deadline - time.monotonic()))
        connection.sendall(b"INFO")
        response = bytearray()
        while len(response) < 80:
            connection.settimeout(max(0, deadline - time.monotonic()))
            byte = connection.recv(1)
            if not byte:
                return None
            response.extend(byte)
            if byte == b"\n":
                return Device(str(ipaddress.IPv4Address(host)), port) if response == IDENTITY else None
        return None
    except TransferCancelled:
        raise
    except (OSError, ValueError, RuntimeError):
        return None
    finally:
        if connection is not None:
            connection.close()


def _read_active_ipv4(cancel=None):
    """Read addresses and true prefixes from connected Windows network adapters."""
    _check_cancel(cancel)
    if os.name != "nt":
        raise RuntimeError("Automatic adapter discovery requires Windows; enter the board IPv4 address manually")
    command = (
        "$ErrorActionPreference='Stop'; "
        "@(Get-NetIPConfiguration | Where-Object { $_.NetAdapter.Status -eq 'Up' } | "
        "ForEach-Object { foreach ($entry in $_.IPv4Address) { "
        "[PSCustomObject]@{Address=$entry.IPAddress;PrefixLength=$entry.PrefixLength} "
        "} }) | ConvertTo-Json -Compress"
    )
    powershell = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                              "System32", "WindowsPowerShell", "v1.0", "powershell.exe")
    with subprocess.Popen(
        [powershell, "-NoProfile", "-NonInteractive", "-Command", command],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        encoding="utf-8", errors="replace", creationflags=subprocess.CREATE_NO_WINDOW,
    ) as process:
        # 首次启动 PowerShell 的冷启动在部分 Windows 环境约需 6 秒；
        # 给网卡查询留出余量，但仍保持有限等待并可由界面取消。
        deadline = time.monotonic() + 15
        try:
            while True:
                _check_cancel(cancel)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError("Timed out reading active network adapters; enter the board IPv4 address manually")
                try:
                    output, error = process.communicate(timeout=min(POLL_INTERVAL, remaining))
                    break
                except subprocess.TimeoutExpired:
                    continue
        except BaseException:
            process.kill()
            process.communicate()
            raise
        if process.returncode:
            raise RuntimeError("Unable to read active IPv4 adapters: " + error.strip())
    if not output.strip():
        return []
    parsed = json.loads(output)
    return [parsed] if isinstance(parsed, dict) else parsed or []


def get_search_networks(cancel=None):
    """Return eligible scan ranges; a >4096-address subnet uses its local /24.

    The returned ranges are suitable for display in the GUI. Addresses from
    loopback, link-local, disconnected, and public adapters are never scanned.
    Advanced manual IPv4 entry can reach a board outside a narrowed scan range.
    """
    networks = []
    seen = set()
    for record in _read_active_ipv4(cancel):
        _check_cancel(cancel)
        try:
            address = ipaddress.IPv4Address(record["Address"])
            prefix = int(record["PrefixLength"])
            network = ipaddress.ip_network(f"{address}/{prefix}", strict=False)
        except (KeyError, TypeError, ValueError):
            continue
        if not any(address in private for private in PRIVATE_NETWORKS):
            continue
        if network.num_addresses > MAX_HOSTS:
            network = ipaddress.ip_network(f"{address}/24", strict=False)
        # A very broad prefix must not make a local address trigger public scans.
        if not any(network.subnet_of(private) for private in PRIVATE_NETWORKS):
            continue
        if network not in seen:
            seen.add(network)
            networks.append(network)
    return networks


def candidate_hosts(networks=None):
    """Deduplicate eligible network hosts and cap a scan at MAX_HOSTS addresses."""
    if networks is None:
        networks = get_search_networks()
    hosts, seen = [], set()
    for network in networks:
        for address in network.hosts():
            host = str(address)
            if host not in seen:
                seen.add(host)
                hosts.append(host)
                if len(hosts) == MAX_HOSTS:
                    return hosts
    return hosts


def discover_devices(port=8080, timeout=0.6, cancel=None, on_progress=None):
    """Probe only local candidate addresses with up to 32 cancellable workers."""
    _check_cancel(cancel)
    if not 1 <= port <= 65535 or timeout <= 0:
        raise ValueError("Invalid port or discovery timeout")
    combined_cancel = _CombinedCancel(cancel)
    hosts = candidate_hosts(get_search_networks(cancel=combined_cancel))
    total = len(hosts)
    if on_progress:
        on_progress(0, total)
    if not hosts:
        return []
    executor = ThreadPoolExecutor(max_workers=min(MAX_WORKERS, total), thread_name_prefix="EPD-info")
    pending = set()
    devices = {}
    done = 0
    try:
        for host in hosts:
            _check_cancel(combined_cancel)
            pending.add(executor.submit(probe_device, host, port, timeout, combined_cancel))
        while pending:
            _check_cancel(combined_cancel)
            finished, pending = wait(pending, timeout=POLL_INTERVAL, return_when=FIRST_COMPLETED)
            for future in finished:
                device = future.result()
                if device is not None:
                    devices[(device.host, device.port)] = device
                done += 1
                if on_progress:
                    on_progress(done, total)
        _check_cancel(combined_cancel)
        return sorted(devices.values(), key=lambda device: (int(ipaddress.IPv4Address(device.host)), device.port))
    finally:
        combined_cancel.local.set()
        executor.shutdown(wait=True, cancel_futures=True)


def send_to_device(device, data, slot, on_progress=None, on_status=None, cancel=None):
    """Send PIC1 to a board; only DONE produces the 'done' status callback."""
    _check_cancel(cancel)
    if len(data) != DATA_SIZE or not 0 <= slot < SLOT_COUNT:
        raise ValueError("Invalid image size or slot")
    if (device.width, device.height, device.slots) != (WIDTH, HEIGHT, SLOT_COUNT):
        raise ValueError("Unsupported display dimensions or picture slots")
    if on_status:
        on_status("connecting")
    connection = _connect(device.host, device.port, 1.2, cancel)
    try:
        send_picture(connection, data, slot, on_progress=on_progress, on_status=on_status, verbose=False)
    finally:
        connection.close()
