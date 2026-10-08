"""Convert and send pictures to the board using the PIC1 protocol.

Example: python send_picture.py photo.jpg --slot 0
Multiple pictures: python send_picture.py a.png b.png --slot 3
Prepare BIN/preview only: add --prepare-only.
Connect to the board's TCP server: add --host BOARD_IP.
Requires Pillow for image conversion: python -m pip install Pillow.
"""
import argparse
from pathlib import Path
import socket
import struct
import zlib
from send_firmware import read_line

WIDTH, HEIGHT, DATA_SIZE, SLOT_COUNT = 800, 480, 48000, 256
ROOT = Path(__file__).resolve().parents[1]


def wait_reply(connection, stage):
    """connection: board socket; stage: expected READY/ACK/DONE and progress."""
    try:
        return read_line(connection)
    except TimeoutError as exc:
        raise RuntimeError(f"Timed out waiting for {stage}; check the board serial log") from exc


def prepare_image(path, threshold=128, dither=False, fit="contain"):
    """path: PNG/JPG/BMP or 48000-byte BIN; return MSB-first 1-white/0-black pixels.

    threshold: 0..255 grayscale cut-off; dither: enable Floyd-Steinberg for photos.
    fit: contain (white borders), cover (center crop), or stretch (800x480).
    Transparent areas become white. Raw BIN data is preserved unchanged.
    """
    path = Path(path)
    if not 0 <= threshold <= 255:
        raise ValueError("Threshold must be 0..255")
    if fit not in ("contain", "cover", "stretch"):
        raise ValueError("Fit must be contain, cover, or stretch")
    if path.suffix.lower() == ".bin":
        data = path.read_bytes()
    else:
        try:
            from PIL import Image, ImageOps
        except ImportError as exc:
            raise RuntimeError("Install Pillow: python -m pip install Pillow") from exc
        with Image.open(path) as source:
            source = ImageOps.exif_transpose(source).convert("RGBA")
            source = Image.alpha_composite(Image.new("RGBA", source.size, "white"), source).convert("RGB")
            if fit == "contain":
                fitted = ImageOps.contain(source, (WIDTH, HEIGHT), Image.Resampling.LANCZOS)
                canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
                canvas.paste(fitted, ((WIDTH - fitted.width) // 2, (HEIGHT - fitted.height) // 2))
            elif fit == "cover":
                canvas = ImageOps.fit(source, (WIDTH, HEIGHT), Image.Resampling.LANCZOS, centering=(0.5, 0.5))
            else:
                canvas = source.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
            gray = canvas.convert("L")
            if dither:
                mono = gray.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
            else:
                mono = gray.point(lambda value: 255 if value >= threshold else 0).convert("1", dither=Image.Dither.NONE)
            data = mono.tobytes()
    if len(data) != DATA_SIZE:
        raise ValueError("Picture must contain exactly 48000 bytes")
    return data


def send_picture(connection, data, slot, on_progress=None, on_status=None, verbose=True):
    """connection: accepted socket; data: 48000 pixel bytes; slot: image number 0..255.

    Wait for READY before body, then cumulative ACK before each next chunk.
    DONE confirms Flash storage and CRC; actual display completes asynchronously.
    """
    if len(data) != DATA_SIZE or not 0 <= slot < SLOT_COUNT:
        raise ValueError("Invalid image size or slot")
    crc = zlib.crc32(data) & 0xFFFFFFFF
    header = struct.pack("<8I", 0x31434950, 1, slot, WIDTH, HEIGHT, len(data), crc, 0)
    connection.settimeout(180)
    if on_status:
        on_status("preparing")
    connection.sendall(header)
    if wait_reply(connection, "READY (Flash preparation)") != "READY":
        raise RuntimeError("Expected READY")
    if verbose:
        print(f"Flash ready: slot {slot}, {len(data)} bytes, CRC32={crc:08X}", flush=True)
    if on_status:
        on_status("sending")
    if on_progress:
        on_progress(0, len(data))
    acknowledged = 0
    for offset in range(0, len(data), 256):
        chunk = data[offset:offset + 256]
        target = offset + len(chunk)
        connection.sendall(chunk)
        while acknowledged < target:
            line = wait_reply(connection, f"ACK {target} (confirmed {acknowledged}/{len(data)} bytes)")
            if not line.startswith("ACK "):
                raise RuntimeError("Expected ACK, got " + line)
            try:
                value = int(line[4:])
            except ValueError as exc:
                raise RuntimeError("Invalid ACK value: " + line) from exc
            if not acknowledged < value <= target:
                raise RuntimeError("ACK progress out of range")
            acknowledged = value
        if on_progress:
            on_progress(acknowledged, len(data))
        if verbose and (target % 8192 == 0 or target == len(data)):
            print(f"Slot {slot}: {target}/{len(data)} bytes")
    if on_status:
        on_status("verifying")
    if wait_reply(connection, "DONE (Flash CRC/commit)") != "DONE":
        raise RuntimeError("Missing Flash storage confirmation")
    if on_status:
        on_status("done")
    if verbose:
        print(f"DONE: slot {slot} saved, CRC32={crc:08X}; check serial log for display result")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pictures", nargs="+", type=Path)
    parser.add_argument("--slot", type=int, default=0, help="First image slot, 0..255")
    parser.add_argument("--threshold", type=int, default=128, help="Black/white cut-off, 0..255")
    parser.add_argument("--dither", action="store_true", help="Dither photographic images")
    parser.add_argument("--fit", choices=("contain", "cover", "stretch"), default="contain", help="Image fit mode")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--host", help="Connect to the board's TCP server at this IPv4 address")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "OBJ/pictures")
    parser.add_argument("--prepare-only", action="store_true", help="Generate BIN/preview without listening")
    args = parser.parse_args()
    if args.slot < 0 or args.slot + len(args.pictures) > SLOT_COUNT:
        parser.error("Slots must be within 0..255")
    if not 0 <= args.threshold <= 255 or not 1 <= args.port <= 65535:
        parser.error("Invalid threshold or port")
    prepared = []
    try:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        for index, path in enumerate(args.pictures, args.slot):
            data = prepare_image(path, args.threshold, args.dither, args.fit)
            stem = args.output_dir / f"{path.stem}_slot{index:03d}"
            bin_path = Path(str(stem) + ".bin")
            preview_path = Path(str(stem) + ".png")
            bin_path.write_bytes(data)
            try:
                from PIL import Image
                Image.frombytes("1", (WIDTH, HEIGHT), data).save(preview_path)
            except ImportError:
                pass  # Raw BIN sending does not require Pillow.
            prepared.append((index, data))
            print(f"Prepared slot {index}: {bin_path}")
        if args.prepare_only:
            return
        if args.host:
            print(f"Connecting to {args.host}:{args.port}")
            with socket.create_connection((args.host, args.port), timeout=10) as connection:
                for slot, data in prepared:
                    send_picture(connection, data, slot)
            return
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((args.bind, args.port)); server.listen(1)
            print(f"Listening on {args.bind}:{args.port}; now restart/connect the board")
            connection, peer = server.accept()
            with connection:
                print("Board connected:", peer)
                for slot, data in prepared:
                    send_picture(connection, data, slot)
    except (OSError, ValueError, RuntimeError) as exc:
        parser.exit(1, f"Picture transfer failed: {exc}\n")


if __name__ == "__main__":
    main()
