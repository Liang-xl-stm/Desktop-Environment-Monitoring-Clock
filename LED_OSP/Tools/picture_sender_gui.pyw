"""中文图片发送窗口：缩放看整体灰度，原尺寸看发送点阵，网络操作不占用界面线程。"""
from pathlib import Path
import argparse
import ipaddress
import json
import os
import queue
import threading
import tkinter as tk
from tkinter import filedialog, ttk
from PIL import Image, ImageOps, ImageTk

from send_picture import prepare_image
from picture_client import Device, TransferCancelled, discover_devices, send_to_device

FIT_MODES = {"完整显示（补白）": "contain", "裁剪填满": "cover", "拉伸填满": "stretch"}
STAGES = {"connecting": "正在连接开发板…", "preparing": "正在准备图片存储…",
          "sending": "正在发送图片…", "verifying": "正在校验并保存图片…",
          "done": "保存完成，开发板将自动刷新屏幕"}
BG, INK, MUTED, ACCENT = "#f3f5f7", "#18232d", "#53616d", "#176148"


class PictureSenderApp:
    """所有控件只在 Tk 主线程更新，后台线程通过消息队列返回结果。

    root：已经创建的 Tk 窗口。start_search：启动后是否自动搜索设备。
    settings_path：选项保存文件；测试可指定临时文件，正常使用保存到用户应用数据目录。
    """

    def __init__(self, root, start_search=True, settings_path=None):
        self.root = root
        self.settings_path = Path(settings_path) if settings_path else (
            Path(os.environ.get("APPDATA", str(Path.home()))) / "STM32PictureSender/settings.json")
        self.settings = self._read_settings()
        self.messages = queue.Queue()
        self.devices = []
        self.device_labels = []
        self.picture_path = None
        self.picture_data = None
        self.source_image = None
        self.preview_image = None
        self.source_size = None
        self.preview_busy = False
        self.operation = None
        self.cancel_event = None
        self.closed = False
        self.manual_open = False
        self._preview_serial = 0
        self._preview_timer = None
        self._operation_serial = 0
        self._photo_original = None
        self._photo_preview = None
        self.device_var = tk.StringVar(root, "尚未搜索到设备")
        # 使用文本变量保留原始输入，避免 Tk 的 IntVar 把 “7.9” 静默截断为 7。
        self.port_var = tk.StringVar(root, str(self._setting_int("port", 8080, 1, 65535)))
        self.slot_var = tk.StringVar(root, str(self._setting_int("slot", 0, 0, 255)))
        fit = self.settings.get("fit", "contain")
        self.fit_var = tk.StringVar(root, next((k for k, v in FIT_MODES.items() if v == fit), next(iter(FIT_MODES))))
        self.threshold_var = tk.StringVar(root, str(self._setting_int("threshold", 128, 0, 255)))
        self.dither_var = tk.BooleanVar(root, bool(self.settings.get("dither", True)))
        self.manual_host_var = tk.StringVar(root, "")
        self.file_var = tk.StringVar(root, "选择一张图片开始预览")
        self.source_caption = tk.StringVar(root, "原图")
        self.status_var = tk.StringVar(root, "电脑与开发板接入同一 Wi-Fi 或热点，然后搜索设备")
        self.progress_var = tk.DoubleVar(root, 0)
        self.progress_text = tk.StringVar(root, "")
        self._build_window()
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.device_var.trace_add("write", lambda *_: self._sync_buttons())
        self.manual_host_var.trace_add("write", lambda *_: self._sync_buttons())
        self.root.after(50, self._poll)
        self._sync_buttons()
        if start_search:
            self.root.after(250, self.start_search)

    def _read_settings(self):
        try:
            value = json.loads(self.settings_path.read_text(encoding="utf-8"))
            return value if isinstance(value, dict) else {}
        except (OSError, ValueError):
            return {}

    def _setting_int(self, key, default, minimum, maximum):
        value = self.settings.get(key, default)
        return value if type(value) is int and minimum <= value <= maximum else default

    @staticmethod
    def _read_int(variable, label, minimum, maximum):
        """严格读取整数输入，防止控件把小数或空值静默转换成另一项。"""
        raw = str(variable.get()).strip()
        if not raw or not raw.isdecimal():
            raise ValueError(f"{label}必须是整数")
        value = int(raw)
        if not minimum <= value <= maximum:
            raise ValueError(f"{label}必须在 {minimum}～{maximum} 之间")
        return value

    def _build_window(self):
        self.root.title("墨水屏图片发送器")
        width = min(1160, max(960, self.root.winfo_screenwidth() - 80))
        height = min(800, max(690, self.root.winfo_screenheight() - 100))
        self.root.geometry(f"{width}x{height}")
        self.root.minsize(960, 690)
        self.root.configure(bg=BG)
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure(".", font=("Microsoft YaHei UI", 10), foreground=INK)
        style.configure("TFrame", background=BG)
        style.configure("TLabel", background=BG)
        style.configure("Title.TLabel", font=("Microsoft YaHei UI", 21, "bold"))
        style.configure("Sub.TLabel", foreground=MUTED, font=("Microsoft YaHei UI", 9))
        style.configure("TButton", padding=(13, 7))
        style.configure("Send.TButton", foreground="white", background=ACCENT,
                        font=("Microsoft YaHei UI", 11, "bold"), padding=(24, 10))
        style.map("Send.TButton", background=[("disabled", "#d4dbd8"), ("active", "#124c38")],
                  foreground=[("disabled", "#53616d")])
        style.configure("TCombobox", padding=5)
        style.configure("TSpinbox", padding=5)
        style.configure("Horizontal.TProgressbar", background=ACCENT, troughcolor="#e0e5e8")
        container = ttk.Frame(self.root, padding=(24, 18, 24, 15))
        container.pack(fill="both", expand=True)
        container.columnconfigure(0, weight=1)
        container.rowconfigure(5, weight=1)
        heading = ttk.Frame(container)
        heading.grid(row=0, column=0, sticky="ew")
        ttk.Label(heading, text="墨水屏图片发送器", style="Title.TLabel").pack(side="left")
        ttk.Label(heading, text="800 × 480  ·  黑白图片", style="Sub.TLabel").pack(side="right", pady=8)
        ttk.Label(container, text="搜索开发板，预览后发送。换图无需重启。", style="Sub.TLabel").grid(
            row=1, column=0, sticky="w", pady=(3, 14))

        connection = ttk.Frame(container)
        connection.grid(row=2, column=0, sticky="ew")
        connection.columnconfigure(1, weight=1)
        ttk.Label(connection, text="开发板").grid(row=0, column=0, padx=(0, 12))
        self.device_combo = ttk.Combobox(connection, textvariable=self.device_var, state="readonly")
        self.device_combo.grid(row=0, column=1, sticky="ew", padx=(0, 12))
        self.search_button = ttk.Button(connection, text="搜索设备", command=self.start_search)
        self.search_button.grid(row=0, column=2, padx=(0, 14))
        ttk.Label(connection, text="端口").grid(row=0, column=3, padx=(0, 7))
        self.port_spin = ttk.Spinbox(connection, from_=1, to=65535, width=6, textvariable=self.port_var)
        self.port_spin.grid(row=0, column=4, padx=(0, 12))
        self.manual_button = ttk.Button(connection, text="手动地址", command=self._toggle_manual)
        self.manual_button.grid(row=0, column=5)
        self.manual_frame = ttk.Frame(connection)
        self.manual_frame.grid(row=1, column=0, columnspan=6, sticky="ew", pady=(8, 0))
        ttk.Label(self.manual_frame, text="开发板 IPv4 地址").pack(side="left", padx=(0, 12))
        self.manual_entry = ttk.Entry(self.manual_frame, width=20, textvariable=self.manual_host_var)
        self.manual_entry.pack(side="left", padx=(0, 12))
        ttk.Label(self.manual_frame, text="仅在自动搜索未覆盖时使用", style="Sub.TLabel").pack(side="left")
        self.manual_frame.grid_remove()

        file_row = ttk.Frame(container)
        file_row.grid(row=3, column=0, sticky="ew", pady=(14, 13))
        self.choose_button = ttk.Button(file_row, text="选择图片", command=self.choose_picture)
        self.choose_button.pack(side="left", padx=(0, 12))
        ttk.Label(file_row, textvariable=self.file_var, style="Sub.TLabel").pack(side="left", fill="x", expand=True)
        options = ttk.Frame(container)
        options.grid(row=4, column=0, sticky="ew", pady=(0, 14))
        ttk.Label(options, text="适配方式").pack(side="left", padx=(0, 8))
        self.fit_combo = ttk.Combobox(options, textvariable=self.fit_var, values=list(FIT_MODES),
                                      width=18, state="readonly")
        self.fit_combo.pack(side="left", padx=(0, 20))
        self.fit_combo.bind("<<ComboboxSelected>>", lambda _: self.schedule_preview())
        self.dither_check = ttk.Checkbutton(options, text="抖动，保留灰阶细节", variable=self.dither_var,
                                            command=self.schedule_preview)
        self.dither_check.pack(side="left", padx=(0, 20))
        ttk.Label(options, text="黑白阈值").pack(side="left", padx=(0, 8))
        self.threshold_spin = ttk.Spinbox(options, from_=0, to=255, width=5,
                                          textvariable=self.threshold_var, command=self.schedule_preview)
        self.threshold_spin.pack(side="left")
        self.threshold_spin.bind("<KeyRelease>", lambda _: self.schedule_preview())
        self.threshold_spin.bind("<FocusOut>", lambda _: self.schedule_preview())

        views = ttk.Frame(container)
        views.grid(row=5, column=0, sticky="nsew")
        views.columnconfigure(0, weight=1, uniform="views")
        views.columnconfigure(1, weight=1, uniform="views")
        views.rowconfigure(1, weight=1)
        ttk.Label(views, textvariable=self.source_caption, font=("Microsoft YaHei UI", 11, "bold")).grid(
            row=0, column=0, sticky="w", pady=(0, 8))
        ttk.Label(views, text="整体预览", font=("Microsoft YaHei UI", 11, "bold")).grid(
            row=0, column=1, sticky="w", padx=(16, 0), pady=(0, 8))
        self.source_canvas = tk.Canvas(views, width=480, height=288, bg="#e6eaed",
                                      highlightthickness=1, highlightbackground="#ccd3d8")
        self.source_canvas.grid(row=1, column=0, sticky="nsew", padx=(0, 8))
        self.preview_canvas = tk.Canvas(views, width=480, height=288, bg="#e6eaed",
                                       highlightthickness=1, highlightbackground="#ccd3d8")
        self.preview_canvas.grid(row=1, column=1, sticky="nsew", padx=(8, 0))
        self.source_canvas.bind("<Configure>", lambda _: self._draw_images())
        self.preview_canvas.bind("<Configure>", lambda _: self._draw_images())
        self.preview_canvas.bind("<Double-Button-1>", lambda _: self.show_full_preview())
        ttk.Label(container, text="缩放预览合并细小黑白点，便于看整体灰度；双击查看 800×480 原始点阵。\n"
                  "实屏对比度受环境光影响。抖动用小点表现灰度；关闭抖动后可调整阈值，获得更明确的黑白效果。",
                  style="Sub.TLabel").grid(row=6, column=0, sticky="w", pady=(8, 14))

        transfer = ttk.Frame(container)
        transfer.grid(row=7, column=0, sticky="ew")
        transfer.columnconfigure(0, weight=1)
        ttk.Label(transfer, textvariable=self.status_var, wraplength=920).grid(row=0, column=0, sticky="w")
        ttk.Label(transfer, textvariable=self.progress_text, style="Sub.TLabel").grid(row=0, column=1, sticky="e")
        self.progress = ttk.Progressbar(transfer, variable=self.progress_var, maximum=100)
        self.progress.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(8, 12))
        actions = ttk.Frame(container)
        actions.grid(row=8, column=0, sticky="ew")
        ttk.Label(actions, text="图片编号").pack(side="left", padx=(0, 8))
        self.slot_spin = ttk.Spinbox(actions, from_=0, to=255, width=5, textvariable=self.slot_var)
        self.slot_spin.pack(side="left", padx=(0, 10))
        ttk.Label(actions, text="相同编号会替换原图片", style="Sub.TLabel").pack(side="left")
        self.send_button = ttk.Button(actions, text="发送到屏幕", style="Send.TButton", command=self.start_send)
        self.send_button.pack(side="right", padx=(12, 0))
        self.cancel_button = ttk.Button(actions, text="取消", command=self.cancel)
        self.cancel_button.pack(side="right")
        self.send_inputs = [self.choose_button, self.fit_combo, self.dither_check, self.threshold_spin,
                            self.device_combo, self.port_spin, self.slot_spin, self.manual_button, self.manual_entry]

    def _toggle_manual(self):
        self.manual_open = not self.manual_open
        if self.manual_open:
            self.manual_frame.grid()
            self.manual_entry.focus_set()
        else:
            self.manual_frame.grid_remove()
        self._sync_buttons()

    def set_devices(self, devices):
        """devices：已通过专用查询确认的 Device 列表；仅由界面线程调用。"""
        self.devices = list(devices)
        self.device_labels = [f"墨水屏  ·  {d.host}:{d.port}" for d in self.devices]
        self.device_combo.configure(values=self.device_labels)
        if self.devices:
            preferred = self.settings.get("last_host")
            index = next((i for i, device in enumerate(self.devices) if device.host == preferred), 0)
            self.device_var.set(self.device_labels[index])
        else:
            self.device_var.set("未找到开发板，请检查同一 Wi-Fi 和新版程序")
        self._sync_buttons()

    def choose_picture(self):
        filename = filedialog.askopenfilename(parent=self.root, title="选择发送到墨水屏的图片",
            filetypes=[("图片", "*.png *.jpg *.jpeg *.bmp *.webp *.tif *.tiff"),
                       ("屏幕像素 BIN", "*.bin"), ("所有文件", "*.*")])
        if filename:
            self.load_picture(filename)

    def load_picture(self, path):
        """path：本地图片路径。转换在后台执行，完成后才开放发送按钮。"""
        self.picture_path = Path(path)
        self.file_var.set(self.picture_path.name)
        self.source_image = None
        self.preview_image = None
        self.source_caption.set("原图")
        self._draw_images()
        self.schedule_preview()

    def schedule_preview(self):
        """设置改变时废弃旧像素，合并快速输入，避免发送过期预览。"""
        self._sync_buttons()
        if self.picture_path is None or self.operation == "send":
            return
        self._preview_serial += 1
        self.picture_data = None
        self.preview_busy = True
        if self.operation is None:
            self.status_var.set("正在生成黑白预览…")
        if self._preview_timer is not None:
            self.root.after_cancel(self._preview_timer)
        self._preview_timer = self.root.after(150, self._prepare_preview)
        self._sync_buttons()

    def _prepare_preview(self):
        self._preview_timer = None
        serial = self._preview_serial
        path = self.picture_path
        try:
            fit, dither = FIT_MODES[self.fit_var.get()], self.dither_var.get()
            # 抖动模式不使用固定阈值；即使阈值输入框为空也应能预览。
            threshold = 128 if dither else self._read_int(self.threshold_var, "黑白阈值", 0, 255)
        except (ValueError, KeyError, tk.TclError) as exc:
            self.preview_busy = False
            self.status_var.set(str(exc) if isinstance(exc, ValueError) else "请填写有效的黑白阈值")
            self._sync_buttons()
            return

        def work():
            try:
                data = prepare_image(path, threshold, dither, fit=fit)
                preview = Image.frombytes("1", (800, 480), data)
                if path.suffix.lower() == ".bin":
                    original, size = preview.convert("RGB"), (800, 480)
                else:
                    with Image.open(path) as image:
                        original = ImageOps.exif_transpose(image).convert("RGBA")
                        size = original.size
                        original = Image.alpha_composite(Image.new("RGBA", size, "white"), original).convert("RGB")
                        original.thumbnail((1600, 960), Image.Resampling.LANCZOS)
                self.messages.put(("preview", serial, (data, preview, original, size)))
            except Exception as exc:
                self.messages.put(("preview_error", serial, str(exc)))
        threading.Thread(target=work, daemon=True).start()

    def _draw_images(self):
        for canvas, image, key in ((self.source_canvas, self.source_image, "original"),
                                   (self.preview_canvas, self.preview_image, "preview")):
            width, height = canvas.winfo_width(), canvas.winfo_height()
            if width < 3 or height < 3:
                continue
            canvas.delete("all")
            if image is None:
                canvas.create_text(width // 2, height // 2, text="选择图片后显示" if key == "original" else "黑白效果预览",
                                   fill=MUTED, font=("Microsoft YaHei UI", 11))
                continue
            # 1 位图直接缩放会按最近邻取点：一半黑、一半白的抖动图可能缩成全白。
            # 先转灰度再平滑显示，只改变窗口缩略图，不改 preview_image 或发送字节。
            # LANCZOS 对高频黑白点有振铃；BILINEAR 更适合保持抖动图的整体灰度。
            display_image = image.convert("L") if key == "preview" else image
            fitted = ImageOps.contain(display_image, (width - 18, height - 18),
                                     Image.Resampling.BILINEAR if key == "preview" else Image.Resampling.LANCZOS)
            photo = ImageTk.PhotoImage(fitted, master=self.root)
            setattr(self, "_photo_" + key, photo)
            canvas.create_image(width // 2, height // 2, image=photo)

    def show_full_preview(self):
        if self.preview_image is None:
            return
        window = tk.Toplevel(self.root)
        window.title("原始黑白点阵 · 800×480 发送像素")
        window.resizable(False, False)
        photo = ImageTk.PhotoImage(self.preview_image, master=window)
        label = ttk.Label(window, image=photo)
        label.image = photo
        label.pack(padx=12, pady=12)

    def _selected_device(self):
        if self.manual_open and self.manual_host_var.get().strip():
            host = self.manual_host_var.get().strip()
            if ipaddress.ip_address(host).version != 4:
                raise ValueError("请填写开发板的 IPv4 地址")
            port = self._read_int(self.port_var, "端口", 1, 65535)
            return Device(host, port)
        if self.device_var.get() in self.device_labels:
            return self.devices[self.device_labels.index(self.device_var.get())]
        raise ValueError("请先搜索并选择开发板")

    def _sync_buttons(self):
        if not hasattr(self, "send_button"):
            return
        busy = self.operation is not None
        for widget in self.send_inputs:
            widget.state(["disabled"] if self.operation == "send" else ["!disabled"])
        if self.dither_var.get() or self.operation == "send":
            self.threshold_spin.state(["disabled"])
        else:
            self.threshold_spin.state(["!disabled"])
        self.search_button.state(["disabled"] if busy else ["!disabled"])
        self.port_spin.state(["disabled"] if busy else ["!disabled"])
        self.cancel_button.state(["!disabled"] if busy else ["disabled"])
        selected = self.device_var.get() in self.device_labels or (self.manual_open and bool(self.manual_host_var.get().strip()))
        allowed = not busy and not self.preview_busy and self.picture_data is not None and selected
        self.send_button.state(["!disabled"] if allowed else ["disabled"])

    def start_search(self):
        if self.operation is not None:
            return
        try:
            port = self._read_int(self.port_var, "端口", 1, 65535)
        except (ValueError, tk.TclError):
            self.status_var.set("请填写有效端口（1～65535）")
            return
        self._operation_serial += 1
        serial = self._operation_serial
        self.operation = "search"
        self.cancel_event = threading.Event()
        cancel = self.cancel_event
        self.progress_var.set(0)
        self.progress_text.set("")
        self.status_var.set("正在搜索当前局域网的墨水屏…")
        self._sync_buttons()

        def work():
            try:
                devices = discover_devices(port=port, cancel=cancel,
                    on_progress=lambda done, total: self.messages.put(("search_progress", serial, (done, total))))
                if cancel.is_set():
                    raise TransferCancelled("搜索已取消")
                self.messages.put(("devices", serial, devices))
            except TransferCancelled:
                self.messages.put(("cancelled", serial, "搜索已取消，可以重新搜索"))
            except Exception as exc:
                self.messages.put(("error", serial, "搜索失败：" + str(exc)))
        threading.Thread(target=work, daemon=True).start()

    def start_send(self):
        if self.operation is not None or self.picture_data is None or self.preview_busy:
            return
        try:
            device = self._selected_device()
            slot = self._read_int(self.slot_var, "图片编号", 0, 255)
        except (ValueError, tk.TclError) as exc:
            self.status_var.set(str(exc) if isinstance(exc, ValueError) else "请填写有效的图片编号")
            return
        data = self.picture_data  # 固定本次像素，之后任何预览变更都不能覆盖它。
        self._operation_serial += 1
        serial = self._operation_serial
        self.operation = "send"
        self.cancel_event = threading.Event()
        cancel = self.cancel_event
        self.progress_var.set(0)
        self.progress_text.set("0 / 48,000 字节")
        self.status_var.set(STAGES["connecting"])
        self._save_settings(device.host)
        self._sync_buttons()

        def work():
            try:
                send_to_device(device, data, slot, cancel=cancel,
                    on_progress=lambda done, total: self.messages.put(("send_progress", serial, (done, total))),
                    on_status=lambda status: self.messages.put(("stage", serial, status)))
                self.messages.put(("sent", serial, slot))
            except TransferCancelled:
                self.messages.put(("cancelled", serial, "本次发送已取消，可以重新发送"))
            except Exception as exc:
                self.messages.put(("error", serial, "发送未获保存确认：" + str(exc)))
        threading.Thread(target=work, daemon=True).start()

    def cancel(self):
        if self.cancel_event is not None:
            self.cancel_event.set()
            self.status_var.set("正在取消，请稍候…")
            self.cancel_button.state(["disabled"])

    def _poll(self):
        if self.closed:
            return
        for _ in range(256):
            try:
                kind, serial, value = self.messages.get_nowait()
            except queue.Empty:
                break
            if kind.startswith("preview"):
                if serial != self._preview_serial:
                    continue
                self.preview_busy = False
                if kind == "preview":
                    self.picture_data, self.preview_image, self.source_image, self.source_size = value
                    self.source_caption.set(f"原图  ·  {self.source_size[0]} × {self.source_size[1]}")
                    self._draw_images()
                    if self.operation is None:
                        self.status_var.set("预览已准备好，选择开发板后即可发送")
                else:
                    self.picture_data = None
                    self.preview_image = None
                    self.status_var.set("图片转换失败：" + value)
                    self._draw_images()
                self._sync_buttons()
                continue
            if serial != self._operation_serial:
                continue
            if kind in ("search_progress", "send_progress"):
                done, total = value
                self.progress_var.set(100 * done / total if total else 0)
                self.progress_text.set(f"{done:,} / {total:,}" + (" 字节" if kind == "send_progress" else " 个地址"))
            elif kind == "stage":
                self.status_var.set(STAGES.get(value, str(value)))
            elif kind in ("devices", "sent", "cancelled", "error"):
                self.operation = None
                self.cancel_event = None
                if kind == "devices":
                    self.set_devices(value)
                    self.status_var.set(f"找到 {len(value)} 台墨水屏，选择图片后即可发送" if value else
                                        "未找到设备。请确认同一 Wi-Fi、开发板已烧录新版程序；大网段可用手动地址。")
                elif kind == "sent":
                    self.progress_var.set(100)
                    self.status_var.set(f"图片 {value} 保存完成，开发板将自动刷新；可继续发送下一张")
                else:
                    self.status_var.set(value)
                self._sync_buttons()
        self.root.after(50, self._poll)

    def _save_settings(self, host=None):
        try:
            threshold = (128 if self.dither_var.get() else
                         self._read_int(self.threshold_var, "黑白阈值", 0, 255))
            settings = {"port": self._read_int(self.port_var, "端口", 1, 65535),
                        "slot": self._read_int(self.slot_var, "图片编号", 0, 255),
                        "threshold": threshold,
                        "fit": FIT_MODES[self.fit_var.get()],
                        "dither": self.dither_var.get(), "last_host": host or self.settings.get("last_host", "")}
            self.settings_path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.settings_path.with_suffix(".tmp")
            temporary.write_text(json.dumps(settings, ensure_ascii=False, indent=2), encoding="utf-8")
            temporary.replace(self.settings_path)
            self.settings = settings
        except (OSError, ValueError, KeyError, tk.TclError):
            pass  # 保存选项失败不影响已经确认完成的图片传输。

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.cancel_event is not None:
            self.cancel_event.set()
        self._save_settings()
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, help="打开时预览的图片")
    parser.add_argument("--no-search", action="store_true", help="关闭启动时搜索")
    args = parser.parse_args()
    root = tk.Tk()
    app = PictureSenderApp(root, start_search=not args.no_search)
    if args.image:
        app.load_picture(args.image)
    root.mainloop()


if __name__ == "__main__":
    main()
