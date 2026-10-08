"""工程根目录运行 python Tests/run_tests.py，验证 Flash、A/B、包解析、日志和应用任务。"""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "OBJ" / "host_tests"


def run(args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def main():
    gcc = os.environ.get("CC") or shutil.which("gcc")
    if not gcc:
        candidate = Path(r"E:\Program Files (x86)\mingw64\bin\gcc.exe")
        if candidate.is_file():
            gcc = str(candidate)
    if not gcc:
        raise SystemExit("请安装 GCC 或设置 CC 环境变量为 gcc.exe 的路径")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    cases = [
        ("usart_rx", ["Tests/stubs/usart", "SYSTEM/usart"], ["SYSTEM/usart/usart.c"], False),
        ("spi", ["Tests/stubs/spi", "Hardware/SPI"], ["Hardware/SPI/spi.c"], False),
        ("epd", ["Tests/stubs/epd", "USER", "Hardware/EPD_4in26"], ["Hardware/EPD_4in26/DEV_Config.c", "Hardware/EPD_4in26/EPD_4in26.c"], False),
        ("epd_picture_task", ["Tests/stubs/epd", "USER", "Hardware/EPD_4in26", "Application", "Application/Picture"], ["Application/epd_app.c"], False),
        ("picture", ["Application/Picture", "Hardware/W25Q128", "Hardware/SPI"], ["Application/Picture/picture_store.c"], False),
        ("download_stream", ["Application", "Application/Picture", "Application/Firmware"], ["Application/download_stream.c", "Application/Firmware/firmware_stream.c"], False),
        ("download_tcp", ["Tests/stubs/tcp", "Application", "Application/Picture", "Application/Firmware", "Hardware/W25Q128", "Hardware/SPI"], ["Application/Firmware/firmware_tcp.c", "Application/download_stream.c", "Application/Firmware/firmware_stream.c"], True),
        ("w25q128", ["Hardware/SPI", "Hardware/W25Q128"], ["Hardware/W25Q128/w25q128.c"], False),
        ("firmware_startup", ["Application/Firmware", "Hardware/W25Q128", "Hardware/SPI"], ["Application/Firmware/firmware_startup.c"], False),
        ("firmware_startup_recovery", ["Application/Firmware", "Hardware/W25Q128", "Hardware/SPI"], ["Application/Firmware/firmware_startup.c"], False),
        ("firmware_stream", ["Application/Firmware"], ["Application/Firmware/firmware_stream.c"], False),
        ("firmware_ab", ["Application/Firmware", "Hardware/W25Q128", "Hardware/SPI"], ["Application/Firmware/firmware_update.c"], True),
        ("easylogger", ["Tests/stubs/easylogger", "USER", "SYSTEM/usart", "SYSTEM/log", "Middlewares/EasyLogger/inc"], ["Middlewares/EasyLogger/src/elog.c", "Middlewares/EasyLogger/src/elog_utils.c", "SYSTEM/log/elog_port.c", "SYSTEM/log/log.c"], True),
        ("app_tasks", ["Tests/stubs/app", "Application", "Hardware/LED", "SYSTEM/usart", "SYSTEM/log", "USER", "Middlewares/EasyLogger/inc"], ["Application/app.c"], False),
    ]
    for name, includes, sources, separate in cases:
        exe = OUTPUT / (name + (".exe" if os.name == "nt" else ""))
        args = [gcc, "-std=c99", "-Wall", "-Wextra", "-Werror"]
        if name == "usart_rx":
            args.append("-Dfputc=test_usart_fputc")  # Keep the host's own stdout available.
        for directory in includes:
            args.extend(["-I", directory])
        args.extend(["-x", "c", *sources, f"Tests/test_{name}.c", "-o", exe])
        run(args)
        if separate:
            names = subprocess.check_output([str(exe), "--list"], cwd=ROOT, text=True).splitlines()
            for case in names:
                run([exe, case])
        else:
            run([exe])
    run([sys.executable, "-W", "error", "Tests/test_firmware_sender.py"])
    run([sys.executable, "-W", "error", "Tests/test_picture_sender.py"])
    run([sys.executable, "-W", "error", "Tests/test_picture_client.py"])
    run([sys.executable, "-W", "error", "Tests/test_picture_gui.py"])
    print("PASS all host regression suites")


if __name__ == "__main__":
    main()
