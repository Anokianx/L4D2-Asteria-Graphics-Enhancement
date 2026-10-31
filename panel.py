from __future__ import annotations

import math
import random
import time

try:
    from PySide6.QtCore import Qt, QTimer, QSize, QPointF, QVariantAnimation, QEasingCurve, QRectF
    from PySide6.QtGui import QPixmap, QColor, QPainter, QPen, QPolygonF, QLinearGradient
    from PySide6.QtWidgets import QWidget, QLabel, QFrame, QVBoxLayout, QHBoxLayout, QSlider, QAbstractButton, QComboBox, QGridLayout, QPushButton, QScrollArea, QStackedWidget, QSizePolicy, QButtonGroup
except Exception:
    from PyQt6.QtCore import Qt, QTimer, QSize, QPointF, QVariantAnimation, QEasingCurve, QRectF
    from PyQt6.QtGui import QPixmap, QColor, QPainter, QPen, QPolygonF, QLinearGradient
    from PyQt6.QtWidgets import QWidget, QLabel, QFrame, QVBoxLayout, QHBoxLayout, QSlider, QAbstractButton, QComboBox, QGridLayout, QPushButton, QScrollArea, QStackedWidget, QSizePolicy, QButtonGroup

from . import ASTERIA_VERSION, SHADER_ROOT, ASTERIA_ROOT
from .effects import EFFECTS
from .runtime import RuntimeClient
from .telemetry import TelemetryOverlay, HUD_METRICS


class AsteriaSwitch(QAbstractButton):
    def __init__(self, text, parent=None, width=250):
        super().__init__(parent)
        self.setText(text)
        self.setCheckable(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFixedSize(width, 40)
        self._progress = 0.0
        self._slide = QVariantAnimation(self)
        self._slide.setDuration(220)
        self._slide.setEasingCurve(QEasingCurve.Type.InOutCubic)
        self._slide.valueChanged.connect(self._advance)
        self.toggled.connect(self._toggle)

    def _advance(self, value):
        self._progress = float(value)
        self.update()

    def _toggle(self, checked):
        self._slide.stop()
        self._slide.setStartValue(self._progress)
        self._slide.setEndValue(1.0 if checked else 0.0)
        self._slide.start()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setPen(QPen(QColor("#a0a8b6"), 1))
        painter.setBrush(QColor("#13151b"))
        painter.drawPolygon(QPolygonF([QPointF(10, 7), QPointF(70, 7), QPointF(60, 33), QPointF(0, 33)]))
        x = 4 + 31 * self._progress
        painter.setBrush(QColor("#ffffff") if self.isChecked() else QColor("#636b78"))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawPolygon(QPolygonF([QPointF(x + 9, 10), QPointF(x + 30, 10), QPointF(x + 21, 30), QPointF(x, 30)]))
        painter.setPen(QColor("#ffffff"))
        font = painter.font()
        font.setFamily("Segoe UI")
        font.setBold(True)
        font.setItalic(True)
        font.setPixelSize(15)
        painter.setFont(font)
        painter.drawText(QRectF(82, 0, self.width() - 82, 40), Qt.AlignmentFlag.AlignVCenter, self.text())
        painter.end()


class AsteriaBackdrop(QWidget):
    def __init__(self, owner):
        super().__init__(owner)
        self._owner = owner
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)

    def paintEvent(self, event):
        self._owner._paint_background(self, event)


class AsteriaMeteorPanel(QWidget):
    def __init__(self, parent=None, game_path_provider=None):
        super().__init__(parent)
        self._game_path_provider = game_path_provider
        self.setObjectName("AsteriaPanel")
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.runtime = RuntimeClient()
        self._write_timer = QTimer(self)
        self._write_timer.setSingleShot(True)
        self._write_timer.setInterval(100)
        self._write_timer.timeout.connect(self.runtime.flush)
        self.telemetry = TelemetryOverlay() if TelemetryOverlay else None
        self._effect_controls = {}
        self._effect_sections = {}
        self._metric_switches = {}
        self._last_state = {}
        self._phase = 0.0
        self._clock = time.monotonic()
        self._random = random.Random(7152)
        self._meteors = [self._new_meteor(-index * .44) for index in range(7)]
        self._stars = [(self._random.random(), self._random.random(), self._random.uniform(.55, 1.3), self._random.uniform(0, math.tau)) for _ in range(28)]
        self._background_logo = QPixmap(str(ASTERIA_ROOT / "assets" / "asteria_logo_trim.png"))
        if self._background_logo.isNull():
            self._background_logo = QPixmap(str(ASTERIA_ROOT / "assets" / "asteria_logo.png"))
        self._logo_scaled = QPixmap()
        self._backdrop = AsteriaBackdrop(self)
        self._backdrop.lower()
        self._timer = QTimer(self)
        self._timer.setInterval(16)
        self._timer.timeout.connect(self._tick)

        root = QVBoxLayout(self)
        root.setContentsMargins(40, 16, 40, 18)
        root.setSpacing(16)
        header = QHBoxLayout()
        self.master_switch = AsteriaSwitch("ASTERIA MASTER", width=265)
        self.master_switch.setChecked(bool(self.runtime.values.get("enabled", True)))
        self.master_switch._slide.stop()
        self.master_switch._progress = 1.0 if self.master_switch.isChecked() else 0.0
        self.runtime_status = QLabel("WAITING FOR RENDER HOST")
        self.runtime_status.setObjectName("AsteriaRuntimeStatus")
        self.runtime_status.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        header.addWidget(self.master_switch)
        header.addStretch(1)
        header.addWidget(self.runtime_status)
        root.addLayout(header)

        navigation = QHBoxLayout()
        navigation.setSpacing(14)
        self._nav_group = QButtonGroup(self)
        self._nav_group.setExclusive(True)
        self.pages = QStackedWidget()
        self.pages.setObjectName("AsteriaPages")
        self.nav_buttons = []
        for index, title in enumerate(("QUALITY", "COLOR / LIGHT", "PERFORMANCE", "RUNTIME")):
            button = QPushButton(title)
            button.setObjectName("AsteriaNav")
            button.setCheckable(True)
            button.setCursor(Qt.CursorShape.PointingHandCursor)
            self._nav_group.addButton(button, index)
            self.nav_buttons.append(button)
            navigation.addWidget(button)
        navigation.addStretch(1)
        self._nav_group.idClicked.connect(self.pages.setCurrentIndex)
        self.nav_buttons[0].setChecked(True)
        root.addLayout(navigation)
        root.addWidget(self.pages, 1)

        self._build_effect_page(("smaa", "cas", "deband"))
        self._build_effect_page(("color", "bloom", "tonemap", "vignette"))
        self._build_performance_page()
        self._build_runtime_page()

        line = QFrame()
        line.setObjectName("AsteriaLine")
        line.setFixedHeight(1)
        root.addWidget(line)
        footer = QHBoxLayout()
        footer.setSpacing(24)
        self.version = QLabel(f"ASTERIA CORE  /  {ASTERIA_VERSION}")
        self.version.setObjectName("AsteriaVersion")
        self.subtitle = QLabel(f"{len(EFFECTS)} SHADER MODULES")
        self.subtitle.setObjectName("AsteriaSubtitle")
        self.status = QLabel("WAITING FOR RENDER HOST")
        self.status.setObjectName("AsteriaStatus")
        self.status.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        footer.addWidget(self.version)
        footer.addWidget(self.subtitle)
        footer.addStretch(1)
        footer.addWidget(self.status)
        root.addLayout(footer)

        self.master_switch.toggled.connect(self._master_toggled)
        hud = self.runtime.values.get("hud", {})
        if self.telemetry is not None:
            self.telemetry.set_scale(float(hud.get("scale", 1.2)))
            self.telemetry.setWindowOpacity(float(hud.get("opacity", .88)))
            self.telemetry.set_metrics(hud.get("metrics", ["fps", "frame", "low", "vram", "cpu", "ram"]))
        self._runtime_timer = QTimer(self)
        self._runtime_timer.setInterval(250)
        self._runtime_timer.timeout.connect(self._runtime_tick)
        self._runtime_timer.start()
        if self.master_switch.isChecked():
            self.runtime.start()
        self.setStyleSheet("""
        QWidget#AsteriaPanel { background: #030406; color: #F4F7FF; }
        QLabel { color: #FFFFFF; font-family: Segoe UI; font-size: 14px; font-weight: 700; font-style: italic; }
        QWidget#AsteriaPages, QWidget#AsteriaPage, QWidget#AsteriaViewport, QScrollArea#AsteriaScroll { background: transparent; border: 0px; }
        QLabel#AsteriaVersion, QLabel#AsteriaSubtitle, QLabel#AsteriaStatus { font-size: 12px; letter-spacing: 1px; }
        QFrame#AsteriaLine { background: #343946; }
        QLabel#AsteriaRuntimeStatus { font-size: 14px; letter-spacing: 1px; }
        QFrame#AsteriaCard { background: transparent; border: 0px; }
        QLabel#AsteriaCardTitle { font-size: 20px; font-weight: 800; letter-spacing: 2px; }
        QLabel#AsteriaCardDesc { color: #B0B7C4; font-size: 13px; font-weight: 600; }
        QLabel#AsteriaNumber { font-size: 15px; }
        QLabel#AsteriaStateValue { font-size: 17px; }
        QSlider::groove:horizontal { height: 4px; background: #343842; border: 0; border-radius: 0; }
        QSlider::sub-page:horizontal { background: #eeeeee; border: 0; border-radius: 0; }
        QSlider::add-page:horizontal { background: #343842; border: 0; border-radius: 0; }
        QSlider::handle:horizontal { width: 10px; margin: -6px 0px; background: #ffffff; border: 1px solid #ffffff; border-radius: 0; }
        QSlider::handle:horizontal:hover { background: #cccccc; border-color: #cccccc; }
        QComboBox { background: #11151D; color: #FFFFFF; border: 1px solid #626A78; border-radius: 0; padding: 6px 10px; font-family: Segoe UI; font-size: 13px; font-weight: 700; font-style: italic; }
        QComboBox QAbstractItemView { background: #11151D; color: #FFFFFF; selection-background-color: #343842; }
        QPushButton { background: #11151D; color: #FFFFFF; border: 1px solid #626A78; border-radius: 0; padding: 10px 16px; font-family: Segoe UI; font-size: 14px; font-weight: 700; font-style: italic; }
        QPushButton:hover { background: #252C38; border-color: #FFFFFF; }
        QPushButton#AsteriaNav { background: transparent; border: 0; border-bottom: 2px solid transparent; padding: 12px 20px; font-size: 15px; letter-spacing: 1px; }
        QPushButton#AsteriaNav:checked { border-bottom-color: #FFFFFF; background: #141820; }
        QPushButton#AsteriaNav:hover { background: #141820; }
        QScrollBar:vertical { background: transparent; width: 6px; margin: 0; }
        QScrollBar::handle:vertical { background: #5B6474; min-height: 38px; border-radius: 3px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        """)

    def _new_page(self):
        page = QWidget()
        page.setObjectName("AsteriaPage")
        layout = QGridLayout(page)
        layout.setContentsMargins(0, 22, 18, 20)
        layout.setHorizontalSpacing(60)
        layout.setVerticalSpacing(38)
        layout.setColumnStretch(0, 1)
        layout.setColumnStretch(1, 1)
        scroll = QScrollArea()
        scroll.setObjectName("AsteriaScroll")
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.viewport().setObjectName("AsteriaViewport")
        scroll.viewport().setAutoFillBackground(False)
        scroll.setWidget(page)
        self.pages.addWidget(scroll)
        return layout

    def _section(self, title, description="", switch=None):
        section = QFrame()
        section.setObjectName("AsteriaCard")
        section.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Maximum)
        layout = QVBoxLayout(section)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(16)
        header = QHBoxLayout()
        heading = QLabel(title)
        heading.setObjectName("AsteriaCardTitle")
        header.addWidget(heading)
        header.addStretch(1)
        if switch is not None:
            header.addWidget(switch)
        layout.addLayout(header)
        if description:
            detail = QLabel(description)
            detail.setObjectName("AsteriaCardDesc")
            detail.setWordWrap(True)
            layout.addWidget(detail)
        rows = QGridLayout()
        rows.setHorizontalSpacing(18)
        rows.setVerticalSpacing(14)
        rows.setColumnMinimumWidth(0, 128)
        rows.setColumnStretch(1, 1)
        rows.setColumnMinimumWidth(2, 66)
        layout.addLayout(rows)
        return section, rows

    def _build_effect_page(self, names):
        layout = self._new_page()
        for index, name in enumerate(names):
            section = self._effect_section(name)
            layout.addWidget(section, index // 2, index % 2, Qt.AlignmentFlag.AlignTop)
        layout.setRowStretch((len(names) + 1) // 2, 1)

    def _effect_section(self, name):
        effect = EFFECTS[name]
        values = self.runtime.values.get(name, {})
        switch = AsteriaSwitch("ENABLE", width=162)
        switch.setChecked(bool(values.get("enabled", effect.get("enabled", False))))
        switch._slide.stop()
        switch._progress = 1.0 if switch.isChecked() else 0.0
        switch.toggled.connect(lambda checked, module=name: self.runtime.set_enabled(module, checked))
        section, rows = self._section(effect["title"], effect.get("description", ""), switch)
        self._effect_sections[name] = section
        controls = {"enabled": switch}
        for row, parameter in enumerate(effect.get("parameters", ())):
            key = parameter["key"]
            value = values.get(key, parameter["default"])
            label = parameter.get("label", key).upper()
            if parameter["kind"] == "choice":
                control = QComboBox()
                for choice in parameter["choices"]:
                    control.addItem(str(choice).upper(), choice)
                position = control.findData(value)
                control.setCurrentIndex(max(0, position))
                control.currentIndexChanged.connect(lambda current, module=name, param=key, combo=control: self.runtime.set_parameter(module, param, combo.itemData(current)))
                rows.addWidget(QLabel(label), row, 0)
                rows.addWidget(control, row, 1, 1, 2)
            else:
                step = float(parameter.get("step", .01))
                minimum = float(parameter["min"])
                maximum = float(parameter["max"])
                formatter = self._number_formatter(parameter)
                control = self._parameter_slider(rows, row, label, minimum, maximum, float(value), step, formatter)
                control.valueChanged.connect(lambda current, module=name, param=key, increment=step: self._queue_parameter(module, param, current * increment))
            controls[key] = control
        self._effect_controls[name] = controls
        if name in ("smaa", "cas", "marker"):
            setattr(self, f"{name}_check", switch)
            setattr(self, f"{name}_section", section)
            if "strength" in controls:
                setattr(self, f"{name}_strength", controls["strength"])
            if "quality" in controls:
                setattr(self, f"{name}_quality", controls["quality"])
        return section

    @staticmethod
    def _number_formatter(parameter):
        step = float(parameter.get("step", .01))
        decimals = min(3, max(0, int(math.ceil(-math.log10(step))))) if step < 1 else 0
        if parameter.get("max", 0) == 1 and parameter.get("min", 0) == 0:
            return lambda value: f"{value * 100:.0f}%"
        suffix = " EV" if parameter["key"] == "exposure" else ""
        return lambda value: f"{value:.{decimals}f}{suffix}"

    def _parameter_slider(self, rows, row, title, minimum, maximum, value, step=1, formatter=None):
        slider = QSlider(Qt.Orientation.Horizontal)
        slider.setRange(round(minimum / step), round(maximum / step))
        slider.setValue(round(value / step))
        slider.setMinimumWidth(150)
        slider.setMinimumHeight(30)
        slider.setPageStep(max(1, (slider.maximum() - slider.minimum()) // 10))
        display = formatter or (lambda number: f"{number:.0f}%")
        number = QLabel(display(slider.value() * step))
        number.setObjectName("AsteriaNumber")
        number.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        slider.valueChanged.connect(lambda current: number.setText(display(current * step)))
        rows.addWidget(QLabel(title), row, 0)
        rows.addWidget(slider, row, 1)
        rows.addWidget(number, row, 2)
        return slider

    def _build_performance_page(self):
        layout = self._new_page()
        hud = self.runtime.values.get("hud", {})
        self.telemetry_check = AsteriaSwitch("ENABLE", width=162)
        self.telemetry_check.setChecked(bool(hud.get("enabled", True)))
        self.telemetry_check._slide.stop()
        self.telemetry_check._progress = 1.0 if self.telemetry_check.isChecked() else 0.0
        self.telemetry_check.toggled.connect(self._set_telemetry_visible)
        self.hud_section, rows = self._section("PERFORMANCE HUD", "Floats above the desktop and game. Drag the highlighted point to reposition it.", self.telemetry_check)
        self.telemetry_size = self._parameter_slider(rows, 0, "SIZE", 50, 300, float(hud.get("scale", 1.2)) * 100)
        self.telemetry_size.valueChanged.connect(self._telemetry_size_changed)
        self.telemetry_opacity = self._parameter_slider(rows, 1, "OPACITY", 25, 100, float(hud.get("opacity", .88)) * 100)
        self.telemetry_opacity.valueChanged.connect(self._telemetry_opacity_changed)
        self.hud_reset_button = QPushButton("RESET HUD POSITION")
        self.hud_reset_button.clicked.connect(self._reset_telemetry_position)
        rows.addWidget(self.hud_reset_button, 2, 0, 1, 3)
        metrics, metric_rows = self._section("MONITORING", "Frame statistics come from rendering callbacks. GAME VRAM, CPU and RAM measure the game process.")
        selected = hud.get("metrics", ["fps", "frame", "low", "vram", "cpu", "ram"])
        for index, (name, specification) in enumerate(HUD_METRICS.items()):
            switch = AsteriaSwitch(specification["label"], width=245)
            switch.setChecked(name in selected)
            switch._slide.stop()
            switch._progress = 1.0 if switch.isChecked() else 0.0
            switch.setToolTip(specification.get("description", ""))
            switch.toggled.connect(self._metrics_changed)
            self._metric_switches[name] = switch
            metric_rows.addWidget(switch, index // 2, index % 2)
        layout.addWidget(self.hud_section, 0, 0, Qt.AlignmentFlag.AlignTop)
        layout.addWidget(metrics, 0, 1, Qt.AlignmentFlag.AlignTop)
        fg = self.runtime.values.get("frame_generation", {})
        self.fg_check = AsteriaSwitch("ENABLE", width=162)
        self.fg_check.setChecked(bool(fg.get("enabled", True)))
        self.fg_check._slide.stop()
        self.fg_check._progress = 1.0 if self.fg_check.isChecked() else 0.0
        self.fg_check.toggled.connect(lambda checked: self._fg_option_changed("enabled", checked))
        self.fg_section, fg_rows = self._section("FSR 3 FRAME GENERATION", "Vulkan presentation pipeline with True Depth capture, Camera Motion Reconstruction, and AMD Optical Flow assistance.", self.fg_check)
        self.fg_true_depth_check = AsteriaSwitch("TRUE DEPTH", width=245)
        self.fg_true_depth_check.setChecked(bool(fg.get("true_depth", True)))
        self.fg_true_depth_check._slide.stop()
        self.fg_true_depth_check._progress = 1.0 if self.fg_true_depth_check.isChecked() else 0.0
        self.fg_true_depth_check.toggled.connect(lambda checked: self._fg_option_changed("true_depth", checked))
        self.fg_motion_check = AsteriaSwitch("MOTION RECONSTRUCTION", width=245)
        self.fg_motion_check.setChecked(bool(fg.get("motion_reconstruction", True)))
        self.fg_motion_check._slide.stop()
        self.fg_motion_check._progress = 1.0 if self.fg_motion_check.isChecked() else 0.0
        self.fg_motion_check.toggled.connect(lambda checked: self._fg_option_changed("motion_reconstruction", checked))
        self.fg_optical_flow_check = AsteriaSwitch("OPTICAL FLOW ASSIST", width=245)
        self.fg_optical_flow_check.setChecked(bool(fg.get("optical_flow", True)))
        self.fg_optical_flow_check._slide.stop()
        self.fg_optical_flow_check._progress = 1.0 if self.fg_optical_flow_check.isChecked() else 0.0
        self.fg_optical_flow_check.toggled.connect(lambda checked: self._fg_option_changed("optical_flow", checked))
        fg_rows.addWidget(self.fg_true_depth_check, 0, 0)
        fg_rows.addWidget(self.fg_motion_check, 0, 1)
        fg_rows.addWidget(self.fg_optical_flow_check, 1, 0)
        self.fg_status_label = QLabel("READY (TRUE DEPTH + MOTION + OF)" if self.fg_check.isChecked() else "DISABLED")
        self.fg_status_label.setObjectName("AsteriaStateValue")
        fg_rows.addWidget(QLabel("PIPELINE"), 2, 0)
        fg_rows.addWidget(self.fg_status_label, 2, 1, 1, 2)
        layout.addWidget(self.fg_section, 1, 0, Qt.AlignmentFlag.AlignTop)
        layout.setRowStretch(2, 1)

    def _build_runtime_page(self):
        layout = self._new_page()
        self.runtime_section, rows = self._section("RENDER HOST", "Launch the game through Asteria to apply effects directly to rendering. Parameter changes sync live.")
        self.launch_button = QPushButton("START GAME WITH ASTERIA")
        self.launch_button.setCursor(Qt.CursorShape.PointingHandCursor)
        self.launch_button.clicked.connect(self._launch_runtime_game)
        rows.addWidget(self.launch_button, 0, 0, 1, 3)
        self._runtime_details = {}
        for index, (name, label) in enumerate((("api", "RENDERER"), ("resolution", "RESOLUTION"), ("gpu", "ADAPTER"), ("effects", "SHADER STATUS"), ("fg", "FRAME GENERATION")), 1):
            rows.addWidget(QLabel(label), index, 0)
            value = QLabel("--")
            value.setObjectName("AsteriaStateValue")
            value.setWordWrap(True)
            rows.addWidget(value, index, 1, 1, 2)
            self._runtime_details[name] = value
        self.shader_path = QLabel(str(SHADER_ROOT))
        self.shader_path.setObjectName("AsteriaCardDesc")
        self.shader_path.setWordWrap(True)
        self.shader_path.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        rows.addWidget(QLabel("SHADER ROOT"), 6, 0)
        rows.addWidget(self.shader_path, 6, 1, 1, 2)
        validation = self._effect_section("marker")
        layout.addWidget(self.runtime_section, 0, 0, Qt.AlignmentFlag.AlignTop)
        layout.addWidget(validation, 0, 1, Qt.AlignmentFlag.AlignTop)
        layout.setRowStretch(1, 1)

    def _master_toggled(self, enabled):
        if enabled:
            self.runtime.start()
        else:
            self.runtime.stop()
        self._set_telemetry_visible(self.telemetry_check.isChecked(), persist=False)
        self._runtime_tick()

    def _set_pipeline_enabled(self, name, enabled):
        self.runtime.set_enabled(name, enabled)

    def _set_pipeline_intensity(self, name, value):
        self._queue_parameter(str(name).lower(), "strength", float(value) / 100.0)

    def _set_pipeline_quality(self, name, value):
        self.runtime.set_quality(name, value)

    def _queue_parameter(self, name, key, value):
        self.runtime.set_parameter(name, key, value, write=False)
        self._queue_write()

    def _queue_write(self):
        if not self._write_timer.isActive():
            self._write_timer.start()

    def _launch_runtime_game(self):
        game_path = self._game_path_provider() if self._game_path_provider else None
        if not self.master_switch.isChecked():
            self.master_switch.setChecked(True)
        if not self.runtime.launch_game(game_path):
            self.runtime_status.setText(self.runtime.loader_error)

    def _set_telemetry_visible(self, visible, persist=True):
        if persist:
            self.runtime.set_hud_option("enabled", bool(visible))
        if self.telemetry is not None:
            self.telemetry.setVisible(bool(visible) and self.master_switch.isChecked())

    def _runtime_tick(self):
        state = self.runtime.poll()
        self._last_state = state
        connected = bool(state.get("connected") and self.master_switch.isChecked())
        self.runtime_status.setText(state.get("label", "WAITING FOR RENDER HOST"))
        readiness = state.get("effects_ready", {})
        for name, section in self._effect_sections.items():
            available = bool(readiness.get(name, state.get(f"{name}_ready", False)))
            section.setToolTip("Shader ready" if available else "Waiting for shader compilation")
        if connected:
            self.status.setText(f"{state.get('api', '--')}  /  {state.get('fps', 0.0):.0f} FPS  /  {state.get('frame_ms', 0.0):.2f} MS")
            self._runtime_details["api"].setText(state.get("api", "--"))
            width, height = state.get("width", 0), state.get("height", 0)
            self._runtime_details["resolution"].setText(f"{width:.0f} × {height:.0f}" if width and height else "--")
            self._runtime_details["gpu"].setText(state.get("gpu_name") or "--")
            self._runtime_details["effects"].setText(f"{sum(bool(value) for value in readiness.values())} / {len(EFFECTS)} READY")
            fg_text = self._format_fg_status(state, connected=True)
            self._runtime_details["fg"].setText(fg_text)
            self.fg_status_label.setText(fg_text)
            if self.telemetry is not None:
                self.telemetry.update_stats(**state)
        else:
            self.status.setText("WAITING FOR RENDER HOST" if self.master_switch.isChecked() else "ASTERIA MASTER  /  OFF")
            for value in self._runtime_details.values():
                value.setText("--")
            self.fg_status_label.setText(self._format_fg_status(state, connected=False))
            if self.telemetry is not None:
                self.telemetry.update_stats()

    def _fg_option_changed(self, key, checked):
        self.runtime.set_frame_generation_option(key, bool(checked))
        self.fg_status_label.setText(self._format_fg_status(self._last_state, connected=bool(self._last_state.get("connected") and self.master_switch.isChecked())))

    def _format_fg_status(self, state, connected=False):
        fg = self.runtime.values.get("frame_generation", {})
        if not fg.get("enabled", True):
            return "DISABLED"
        stages = []
        if fg.get("true_depth", True):
            stages.append("TRUE DEPTH")
        if fg.get("motion_reconstruction", True):
            stages.append("MOTION")
        if fg.get("optical_flow", True):
            stages.append("OF")
        summary = " + ".join(stages) if stages else "FSR 3"
        if connected and state.get("fg_active"):
            return f"ACTIVE ({summary})"
        return f"READY ({summary})"

    def _telemetry_size_changed(self, value):
        scale = float(value) / 100.0
        self.runtime.set_hud_option("scale", scale, write=False)
        self._queue_write()
        if self.telemetry is not None:
            self.telemetry.set_scale(scale)
            self._place_telemetry()

    def _telemetry_opacity_changed(self, value):
        opacity = float(value) / 100.0
        self.runtime.set_hud_option("opacity", opacity, write=False)
        self._queue_write()
        if self.telemetry is not None:
            self.telemetry.setWindowOpacity(opacity)

    def _metrics_changed(self, checked):
        names = [name for name, switch in self._metric_switches.items() if switch.isChecked()]
        self.runtime.set_hud_option("metrics", names)
        if self.telemetry is not None:
            self.telemetry.set_metrics(names)
            self._place_telemetry()

    def _reset_telemetry_position(self):
        if self.telemetry is not None:
            self.telemetry.setProperty("userMoved", False)
            self._place_telemetry()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._backdrop.setGeometry(self.rect())
        self._backdrop.lower()
        if not self._background_logo.isNull():
            self._logo_scaled = self._background_logo.scaled(QSize(max(1, int(self.width() * .54)), max(1, int(self.height() * .50))), Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
        self._place_telemetry()

    def _place_telemetry(self):
        if self.telemetry is not None and not self.telemetry.property("userMoved"):
            position = self.mapToGlobal(QPointF(self.width() - self.telemetry.width() - 28, self.height() - self.telemetry.height() - 48).toPoint())
            screen = self.screen()
            if screen is not None:
                area = screen.availableGeometry()
                position.setX(max(area.left(), min(position.x(), area.right() - self.telemetry.width() + 1)))
                position.setY(max(area.top(), min(position.y(), area.bottom() - self.telemetry.height() + 1)))
            self.telemetry.move(position)

    def showEvent(self, event):
        super().showEvent(event)
        self._clock = time.monotonic()
        self._timer.start()
        if self.telemetry is not None and self.master_switch.isChecked() and self.telemetry_check.isChecked():
            self.telemetry.show()
            self._place_telemetry()

    def hideEvent(self, event):
        self._timer.stop()
        super().hideEvent(event)

    def shutdown(self):
        self._timer.stop()
        self._write_timer.stop()
        self._runtime_timer.stop()
        self.runtime.shutdown()
        if self.telemetry is not None:
            self.telemetry.close()

    def _new_meteor(self, age=None):
        duration = self._random.uniform(1.6, 2.6)
        return {"x": self._random.uniform(-.20, .87), "y": self._random.uniform(-.15, .66), "angle": self._random.uniform(.36, .47), "length": self._random.uniform(.12, .24), "speed": self._random.uniform(340, 530), "duration": duration, "age": self._random.uniform(-1.4, -.08) if age is None else age, "brightness": self._random.uniform(.65, 1.0)}

    def _tick(self):
        current = time.monotonic()
        elapsed = max(0.0, min(.1, current - self._clock))
        self._clock = current
        self._phase += elapsed
        for index, meteor in enumerate(self._meteors):
            meteor["age"] += elapsed
            if meteor["age"] > meteor["duration"]:
                self._meteors[index] = self._new_meteor()
        self._backdrop.update()

    def _paint_background(self, target, event):
        painter = QPainter(target)
        painter.setClipRegion(event.region())
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        w, h = self.width(), self.height()
        if not self._logo_scaled.isNull():
            painter.save()
            painter.setOpacity(.115 + .010 * math.sin(self._phase * .34))
            painter.drawPixmap((w - self._logo_scaled.width()) // 2, (h - self._logo_scaled.height()) // 2, self._logo_scaled)
            painter.restore()
        painter.setPen(Qt.PenStyle.NoPen)
        for x, y, radius, offset in self._stars:
            alpha = int(10 + 12 * (.5 + .5 * math.sin(self._phase * .48 + offset)))
            painter.setBrush(QColor(176, 191, 214, alpha))
            painter.drawEllipse(QPointF(x * w, y * h), radius, radius)
        scale = max(.55, w / 1600.0)
        for meteor in self._meteors:
            age = meteor["age"]
            if age <= 0:
                continue
            progress = age / meteor["duration"]
            envelope = math.sin(progress * math.pi) ** 1.7
            alpha = int(118 * envelope * meteor["brightness"])
            direction_x, direction_y = math.cos(meteor["angle"]), math.sin(meteor["angle"])
            traveled = meteor["speed"] * age * scale
            head = QPointF(meteor["x"] * w + direction_x * traveled, meteor["y"] * h + direction_y * traveled)
            tail = head - QPointF(direction_x * meteor["length"] * w, direction_y * meteor["length"] * w)
            gradient = QLinearGradient(tail, head)
            gradient.setColorAt(0.0, QColor(151, 170, 213, 0))
            gradient.setColorAt(.64, QColor(171, 188, 225, alpha // 3))
            gradient.setColorAt(.94, QColor(210, 221, 246, alpha))
            gradient.setColorAt(1.0, QColor(244, 247, 255, alpha))
            painter.setPen(QPen(gradient, 1.2 * scale, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            painter.drawLine(tail, head)
            glow = QLinearGradient(tail, head)
            glow.setColorAt(0.0, QColor(136, 165, 217, 0))
            glow.setColorAt(1.0, QColor(176, 199, 239, alpha // 7))
            painter.setPen(QPen(glow, 5.0 * scale, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            painter.drawLine(tail, head)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(237, 244, 255, alpha))
            painter.drawEllipse(head, 1.4 * scale, 1.4 * scale)
        painter.end()


__all__ = ["AsteriaMeteorPanel"]
