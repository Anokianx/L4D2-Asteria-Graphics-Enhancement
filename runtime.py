from __future__ import annotations

import json
import math
import os
import subprocess
import time
from pathlib import Path

from . import ASTERIA_RUNTIME_ROOT
from .effects import EFFECTS, default_effect_values


class RuntimeClient:
    def __init__(self, root: Path | None = None):
        self.root = Path(root or ASTERIA_RUNTIME_ROOT)
        self.root.mkdir(parents=True, exist_ok=True)
        self.control_path = self.root / "asteria_runtime_control.json"
        self.status_path = self.root / "asteria_runtime_status.json"
        self.loader_process = None
        self.loader_error = ""
        self.preferences_path = self.root / "asteria_preferences.json"
        self.values = {
            "enabled": True, **default_effect_values(),
            "hud": {"enabled": True, "scale": 1.2, "opacity": 0.88,
                    "metrics": ["fps", "frame", "low", "vram", "cpu", "ram"]},
            "frame_generation": {
                "enabled": True,
                "true_depth": True,
                "motion_reconstruction": True,
                "optical_flow": True,
            },
            "updated_at": 0.0,
        }
        self._load_preferences()
        self._dirty = False

    @staticmethod
    def _parameter_value(parameter, value):
        if parameter["kind"] == "choice":
            choice = str(value).lower()
            return choice if choice in parameter["choices"] else parameter["default"]
        try:
            number = float(value)
            if not math.isfinite(number):
                return parameter["default"]
            return max(parameter["min"], min(parameter["max"], number))
        except (TypeError, ValueError):
            return parameter["default"]

    def _load_preferences(self):
        try:
            saved = json.loads(self.preferences_path.read_text(encoding="utf-8"))
            if not isinstance(saved, dict):
                return
            if isinstance(saved.get("enabled"), bool):
                self.values["enabled"] = saved["enabled"]
            for name, effect in EFFECTS.items():
                options = saved.get(name, {})
                if not isinstance(options, dict):
                    continue
                if isinstance(options.get("enabled"), bool):
                    self.values[name]["enabled"] = options["enabled"]
                for parameter in effect["parameters"]:
                    key = parameter["key"]
                    self.values[name][key] = self._parameter_value(parameter, options.get(key, parameter["default"]))
            hud = saved.get("hud", {})
            if isinstance(hud, dict):
                for key, value in hud.items():
                    self._update_hud_option(key, value)
            fg = saved.get("frame_generation", {})
            if isinstance(fg, dict):
                for key in ("enabled", "true_depth", "motion_reconstruction", "optical_flow"):
                    if isinstance(fg.get(key), bool):
                        self.values["frame_generation"][key] = fg[key]
        except (OSError, ValueError, TypeError):
            pass

    def _write(self, save_preferences=True):
        self.values["updated_at"] = time.time()
        temporary = self.control_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(self.values, ensure_ascii=False), encoding="utf-8")
        os.replace(temporary, self.control_path)
        if save_preferences:
            temporary = self.preferences_path.with_suffix(".tmp")
            temporary.write_text(json.dumps(self.values, ensure_ascii=False), encoding="utf-8")
            os.replace(temporary, self.preferences_path)
        self._dirty = False

    def flush(self):
        if self._dirty:
            self._write()

    def start(self):
        self.values["enabled"] = True
        self._write()
        self._start_loader()

    def stop(self, save_preferences=True):
        self.values["enabled"] = False
        self._write(save_preferences)
        self._stop_loader()

    def _loader_path(self):
        candidates = [
            Path(__file__).resolve().parent / "Runtime" / "bin" / "Asteria.exe",
            Path(__file__).resolve().parent / "Runtime" / "Asteria.exe",
        ]
        return next((path for path in candidates if path.exists()), None)

    def _start_loader(self, arguments=None):
        if self.loader_process is not None and self.loader_process.poll() is None:
            return True
        loader = self._loader_path()
        if loader is None:
            self.loader_error = "ASTERIA LOADER MISSING"
            return False
        try:
            self.loader_process = subprocess.Popen(
                [str(loader), *(arguments or ["--watch"])],
                cwd=str(loader.parent),
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            self.loader_error = ""
            return True
        except OSError as error:
            self.loader_error = str(error)
            self.loader_process = None
            return False

    def launch_game(self, game_path):
        if not game_path:
            self.loader_error = "SELECT L4D2 PATH IN DANYRIA"
            return False
        candidate = Path(game_path)
        candidates = [candidate, candidate / "left4dead2.exe", candidate.parent / "left4dead2.exe"]
        executable = next((path for path in candidates if path.is_file() and path.name.lower() == "left4dead2.exe"), None)
        if executable is None:
            self.loader_error = "LEFT4DEAD2.EXE NOT FOUND"
            return False
        self._stop_loader()
        return self._start_loader(["--launch", "--game", str(executable.resolve())])

    def _stop_loader(self):
        process = self.loader_process
        self.loader_process = None
        if process is None or process.poll() is not None:
            return
        try:
            process.terminate()
            process.wait(timeout=1.5)
        except (OSError, subprocess.TimeoutExpired):
            try:
                process.kill()
            except OSError:
                pass

    def set_enabled(self, name: str, enabled: bool):
        key = str(name).lower()
        if key in EFFECTS:
            self.values[key]["enabled"] = bool(enabled)
            self._write()

    def set_parameter(self, name: str, key: str, value, write=True):
        name = str(name).lower()
        effect = EFFECTS.get(name)
        if effect is None:
            return
        parameter = next((item for item in effect["parameters"] if item["key"] == key), None)
        if parameter is None:
            return
        result = self._parameter_value(parameter, value)
        if self.values[name][key] != result:
            self.values[name][key] = result
            self._dirty = True
            if write:
                self._write()

    def set_strength(self, name: str, value: float):
        self.set_parameter(name, "strength", value)

    def set_quality(self, name: str, value: str):
        self.set_parameter(name, "quality", value)

    def _update_hud_option(self, key, value):
        if key == "enabled" and isinstance(value, bool):
            self.values["hud"][key] = value
        elif key in ("scale", "opacity"):
            try:
                number = float(value)
                if math.isfinite(number):
                    low, high = (0.5, 3.0) if key == "scale" else (0.25, 1.0)
                    self.values["hud"][key] = max(low, min(high, number))
            except (TypeError, ValueError):
                pass
        elif key == "metrics" and isinstance(value, (list, tuple)):
            supported = {"fps", "frame", "low", "p95", "vram", "shared", "cpu", "ram", "system", "resolution", "api"}
            self.values["hud"][key] = list(dict.fromkeys(item for item in value if isinstance(item, str) and item in supported))

    def set_hud_option(self, key, value, write=True):
        self._update_hud_option(key, value)
        self._dirty = True
        if write:
            self._write()

    def set_frame_generation_option(self, key: str, value: bool, write=True):
        if key in ("enabled", "true_depth", "motion_reconstruction", "optical_flow"):
            self.values["frame_generation"][key] = bool(value)
            self._dirty = True
            if write:
                self._write()

    def poll(self):
        if not self.values["enabled"]:
            return {"connected": False, "label": "RUNTIME OFF"}
        if self.loader_process is not None and self.loader_process.poll() not in (None, 0):
            try:
                loader_status = json.loads((self.root / "asteria_loader_status.json").read_text(encoding="utf-8"))
                if 0.0 <= time.time() - float(loader_status.get("heartbeat", 0.0)) < 5.0:
                    self.loader_error = str(loader_status.get("detail", "ASTERIA LAUNCH FAILED"))
            except (OSError, ValueError, TypeError):
                self.loader_error = "ASTERIA LAUNCH FAILED"
        if not self.status_path.exists():
            return {"connected": False, "label": self.loader_error or "WAITING FOR RENDER HOST"}
        try:
            status = json.loads(self.status_path.read_text(encoding="utf-8"))
            heartbeat = float(status.get("heartbeat", 0.0))
            age = time.time() - heartbeat
            connected = bool(status.get("connected", False)) and 0.0 <= age < 3.0 and status.get("mode") == "reshade-addon"
            frame_ms = float(status.get("frame_ms", 0.0))
            fps = float(status.get("fps", 0.0))
            if not math.isfinite(frame_ms) or frame_ms < 0.0:
                frame_ms = 0.0
            if not math.isfinite(fps) or fps < 0.0:
                fps = 0.0
            ready = bool(status.get("effect_ready", False))
            label = "RENDER LIVE / FX READY" if ready else "RENDER LIVE / FX MISSING"
            result = {
                "connected": connected,
                "label": label if connected else self.loader_error or "WAITING FOR RENDER HOST",
                "frame_ms": frame_ms if connected else 0.0,
                "fps": fps if connected else 0.0,
                "effect_ready": ready and connected,
                "smaa_ready": bool(status.get("smaa_ready", False)) and connected,
                "cas_ready": bool(status.get("cas_ready", False)) and connected,
                "api": str(status.get("api", "D3D9")),
            }
            for key in ("fps_low_1", "frame_p95_ms", "vram_mb", "vram_budget_mb", "vram_total_mb", "shared_vram_mb",
                        "cpu_percent", "ram_mb", "system_ram_percent", "width", "height", "pid"):
                value = status.get(key)
                try:
                    number = float(value)
                    result[key] = number if connected and math.isfinite(number) and number >= 0.0 else None
                except (ValueError, TypeError, OverflowError):
                    result[key] = None
            result["gpu_name"] = str(status.get("gpu_name") or "") if connected else ""
            result["vram_status"] = str(status.get("vram_status") or "unavailable") if connected else "unavailable"
            for key in ("effects_ready", "effects_rendered"):
                values = status.get(key, {})
                result[key] = {name: connected and bool(values.get(name, False)) for name in EFFECTS} if isinstance(values, dict) else {}
            result["active_effects"] = [name for name in EFFECTS if result["effects_rendered"].get(name)]
            fg_active = False
            fg_dispatches = 0
            fg_depth_resolves = 0
            fg_motion_reconstructions = 0
            fg_path = self.root / "asteria_fg_layer_status.json"
            if connected and fg_path.exists():
                try:
                    fg_status = json.loads(fg_path.read_text(encoding="utf-8"))
                    fg_active = bool(fg_status.get("frame_generation_active", False))
                    fg_dispatches = int(fg_status.get("interpolation_dispatches", 0))
                    fg_depth_resolves = int(fg_status.get("live_scene_depth_resolves", 0))
                    fg_motion_reconstructions = int(fg_status.get("live_scene_motion_reconstructions", 0))
                except (OSError, ValueError, TypeError):
                    pass
            result["fg_active"] = fg_active
            result["fg_dispatches"] = fg_dispatches
            result["fg_depth_resolves"] = fg_depth_resolves
            result["fg_motion_reconstructions"] = fg_motion_reconstructions
            return result
        except Exception:
            return {"connected": False, "label": "WAITING FOR RENDER HOST"}

    def shutdown(self):
        self.flush()
        self.stop(save_preferences=False)


__all__ = ["RuntimeClient"]
