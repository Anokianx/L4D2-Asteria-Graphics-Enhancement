from __future__ import annotations

import math

try:
    from PySide6.QtCore import Qt, QPointF, QRectF
    from PySide6.QtGui import QPainter, QPen, QColor
    from PySide6.QtWidgets import QFrame, QLabel, QVBoxLayout, QHBoxLayout, QWidget
except ImportError:
    from PyQt6.QtCore import Qt, QPointF, QRectF
    from PyQt6.QtGui import QPainter, QPen, QColor
    from PyQt6.QtWidgets import QFrame, QLabel, QVBoxLayout, QHBoxLayout, QWidget


HUD_METRICS = {
    "fps": {"label": "RENDER FPS", "description": "Frame rate measured from actual rendering Present callbacks.", "default": True},
    "frame": {"label": "FRAME", "description": "Time between rendered frames, in milliseconds.", "default": True},
    "low": {"label": "1% LOW", "description": "FPS calculated from the mean duration of the slowest 1% of frames over the last 3 seconds. Requires at least 100 frames.", "default": True},
    "p95": {"label": "P95 FRAME", "description": "The frame time at or below which 95% of frames fall over the last 3 seconds.", "default": False},
    "vram": {"label": "GAME VRAM", "description": "Game-process local video memory usage / WDDM memory budget. This is not total GPU usage.", "default": True},
    "shared": {"label": "SHARED", "description": "Non-local GPU memory used by the game process, in MB.", "default": False},
    "cpu": {"label": "GAME CPU", "description": "Game-process CPU usage as a percentage of total logical-processor capacity.", "default": True},
    "ram": {"label": "GAME RAM", "description": "Resident physical memory used by the game process, in MB.", "default": True},
    "system": {"label": "SYSTEM RAM", "description": "Physical memory usage across the entire system.", "default": False},
    "resolution": {"label": "RESOLUTION", "description": "Current rendering backbuffer resolution.", "default": False},
    "api": {"label": "RENDER API", "description": "Graphics API used by the current ReShade runtime.", "default": False},
}


class TelemetryOverlay(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        if parent is None:
            self.setWindowFlags(Qt.WindowType.Tool | Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint)
            self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setObjectName("AsteriaTelemetry")
        self._scale = 1.0
        self._drag_offset = None
        self._stats = {}
        self._metrics = [name for name, details in HUD_METRICS.items() if details["default"]]
        self._rows = {}
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        self.setToolTip("Drag the highlighted point in the top-left corner to move the HUD to any monitor.")
        layout = QVBoxLayout(self)
        header = QWidget(self)
        header.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        header_layout = QHBoxLayout(header)
        header_layout.setContentsMargins(0, 0, 0, 0)
        self.title = QLabel("ASTERIA", header)
        self.title.setObjectName("AsteriaTelemetryHeading")
        self.connection = QLabel("WAITING", header)
        self.connection.setObjectName("AsteriaTelemetryState")
        header_layout.addWidget(self.title)
        header_layout.addStretch(1)
        header_layout.addWidget(self.connection)
        layout.addWidget(header)
        for name, details in HUD_METRICS.items():
            row = QWidget(self)
            row.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
            line = QHBoxLayout(row)
            line.setContentsMargins(0, 0, 0, 0)
            title = QLabel(details["label"], row)
            title.setObjectName("AsteriaTelemetryLabel")
            value = QLabel("--", row)
            value.setObjectName("AsteriaTelemetryValue")
            value.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            line.addWidget(title)
            line.addStretch(1)
            line.addWidget(value)
            layout.addWidget(row)
            self._rows[name] = (row, title, value)
        self.fps = self._rows["fps"][2]
        self.frame = self._rows["frame"][2]
        self.vram = self._rows["vram"][2]
        self.set_metrics(self._metrics)

    @staticmethod
    def _number(value):
        if isinstance(value, bool):
            return None
        try:
            numeric = float(value)
        except (TypeError, ValueError, OverflowError):
            return None
        return numeric if math.isfinite(numeric) and numeric >= 0 else None

    def set_metrics(self, names):
        try:
            selected = set(names) if not isinstance(names, (str, bytes)) else set()
        except TypeError:
            selected = set()
        self._metrics = [name for name in HUD_METRICS if name in selected]
        for name, (row, _, _) in self._rows.items():
            row.setVisible(name in selected)
        self.set_scale(self._scale)

    def set_scale(self, scale):
        try:
            scale = float(scale)
        except (TypeError, ValueError):
            scale = 1.0
        self._scale = max(.5, min(3.0, scale)) if math.isfinite(scale) else 1.0
        self.setFixedSize(round(344 * self._scale), round((57 + 27 * len(self._metrics)) * self._scale))
        self.layout().setContentsMargins(round(20 * self._scale), round(13 * self._scale), round(20 * self._scale), round(14 * self._scale))
        self.layout().setSpacing(round(4 * self._scale))
        self.title.setContentsMargins(round(17 * self._scale), 0, 0, 0)
        self.setStyleSheet(
            f"QFrame#AsteriaTelemetry {{ background: transparent; border: none; }}"
            f"QWidget {{ background: transparent; border: none; }}"
            f"QLabel {{ font-family: 'Segoe UI'; font-weight: bold; font-style: italic; }}"
            f"QLabel#AsteriaTelemetryHeading {{ color: #f4f6fb; font-size: {round(12 * self._scale)}px; letter-spacing: 2px; }}"
            f"QLabel#AsteriaTelemetryState {{ color: #8894a5; font-size: {round(9 * self._scale)}px; letter-spacing: 1px; }}"
            f"QLabel#AsteriaTelemetryLabel {{ color: #b1bac8; font-size: {round(11 * self._scale)}px; }}"
            f"QLabel#AsteriaTelemetryValue {{ color: #f5f7ff; font-size: {round(15 * self._scale)}px; }}"
        )
        self.update()

    def update_stats(self, frame_ms=None, vram_mb=None, fps=None, **stats):
        self._stats = {**stats, "fps": fps, "frame_ms": frame_ms, "vram_mb": vram_mb}
        numeric = {key: self._number(value) for key, value in self._stats.items()}
        fps = numeric.get("fps")
        self.connection.setText("LIVE · FSR 3 FG" if (fps is not None and stats.get("fg_active")) else ("LIVE" if fps is not None else "WAITING"))
        for name, field, places, suffix in (
            ("fps", "fps", 1, ""), ("frame", "frame_ms", 2, " ms"),
            ("low", "fps_low_1", 1, ""), ("p95", "frame_p95_ms", 2, " ms"),
            ("shared", "shared_vram_mb", 0, " MB"), ("cpu", "cpu_percent", 1, "%"),
            ("ram", "ram_mb", 0, " MB"), ("system", "system_ram_percent", 1, "%"),
        ):
            value = numeric.get(field)
            self._rows[name][2].setText("--" if value is None else f"{value:.{places}f}{suffix}")
        vram = numeric.get("vram_mb")
        budget = numeric.get("vram_budget_mb")
        if vram is None:
            self.vram.setText("--")
        elif budget is not None and budget > 0:
            self.vram.setText(f"{vram:.0f} / {budget:.0f} MB")
        else:
            self.vram.setText(f"{vram:.0f} MB")
        width, height = numeric.get("width"), numeric.get("height")
        self._rows["resolution"][2].setText(f"{width:.0f} × {height:.0f}" if width and height else "--")
        api = stats.get("api")
        self._rows["api"][2].setText(api if isinstance(api, str) and api else "--")
        total = numeric.get("vram_total_mb")
        gpu = stats.get("gpu_name")
        description = "" if not isinstance(gpu, str) else gpu
        if total is not None:
            description += f" · {total:.0f} MB"
        self._rows["vram"][0].setToolTip(description)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setBrush(QColor(7, 9, 13, 244))
        painter.setPen(QPen(QColor(153, 168, 193, 100), 1))
        painter.drawRoundedRect(QRectF(self.rect()).adjusted(1, 1, -1, -1), 8 * self._scale, 8 * self._scale)
        painter.setPen(QPen(QColor(200, 211, 232, 45), 1))
        y = 39 * self._scale
        painter.drawLine(QPointF(20 * self._scale, y), QPointF(self.width() - 20 * self._scale, y))
        point = QPointF(22 * self._scale, 23 * self._scale)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(220, 231, 255, 28))
        painter.drawEllipse(point, 9 * self._scale, 9 * self._scale)
        painter.setBrush(QColor(226, 237, 255, 60))
        painter.drawEllipse(point, 6 * self._scale, 6 * self._scale)
        painter.setBrush(QColor("#f5faff"))
        painter.drawEllipse(point, 3.5 * self._scale, 3.5 * self._scale)
        painter.end()

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_offset = event.globalPosition().toPoint() - self.pos()
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event):
        if self._drag_offset is not None and event.buttons() & Qt.MouseButton.LeftButton:
            self.move(event.globalPosition().toPoint() - self._drag_offset)
            self.setProperty("userMoved", True)
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        self._drag_offset = None
        super().mouseReleaseEvent(event)
