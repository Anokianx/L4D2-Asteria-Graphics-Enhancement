from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import struct

__author__ = "Anokianx"
ROOT = Path(__file__).resolve().parents[1]
CASES = ("", "NoDepth", "Disabled", "Invalidated", "Resize", "Unqualified")
SOURCES = ("LiveInputs/ReShadeInputSnapshot.cpp", "LiveInputs/InputSnapshotHost.cpp",
           "LiveInputs/VulkanTextureInterop.h", "LiveInputs/README.md",
           "LiveInputs/Shaders/AsteriaInputSnapshot.fx", "LiveInputs/Build_InputSnapshot.bat",
           "LiveInputs/bin/AsteriaInputSnapshot.addon", "LiveInputs/bin/AsteriaInputSnapshotFixture.addon", "LiveInputs/bin/AsteriaInputSnapshotHost.exe",
           "LiveInputs/Run_InputSnapshot_Tests.py", "LiveInputs/Validate_InputSnapshot.py",
           "LiveInputs/References/ReShade-v6.7.2/source-manifest.json",
           "DepthCapture/ReShadeDepthSemanticBridge.h")


class Validator:
    def __init__(self):
        self.checks = 0
        self.artifacts = {}
        self.pixels = 0
        self.captures = 0

    def require(self, value, message):
        self.checks += 1
        if not value:
            raise ValueError(message)

    def read(self, path):
        path = path.resolve()
        self.require(ROOT in path.parents, f"Artifact escaped prototype: {path}")
        data = path.read_bytes()
        self.artifacts[path.relative_to(ROOT).as_posix()] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        return data

    def json(self, path):
        return json.loads(self.read(path).decode("utf-8-sig"))

    def case(self, suffix):
        directory = ROOT / "LiveInputs/results" / ("InputSnapshotVulkan" + suffix)
        host = self.json(directory / "validation.json")
        self.require(host["passed"] and host["dxvk_vulkan"] and host["process_bits"] == 32, f"Host failed: {suffix}")
        self.require(not host["game_started"] and not host["native_fg_ready"] and not host["layer_registration_changed"] and
                     not host["game_files_modified"] and not host["cpu_color_uploaded"] and not host["cpu_depth_uploaded"], "Host overstated production readiness")
        deployed = self.read(directory / "AsteriaFixtureDXVK.dll")
        self.require(hashlib.sha256(Path(host["dxvk_source"]).read_bytes()).digest() == hashlib.sha256(deployed).digest(), "DXVK fixture differs from requested existing source")
        addon = "AsteriaInputSnapshotFixture.addon" if suffix == "Invalidated" else "AsteriaInputSnapshot.addon"
        self.require(self.read(directory / "TestAddons/AsteriaInputSnapshot.addon") == self.read(ROOT / "LiveInputs/bin" / addon),
                     "Snapshot host loaded a different add-on build")
        self.require(self.read(directory / "Effects/AsteriaInputSnapshot.fx") == self.read(ROOT / "LiveInputs/Shaders/AsteriaInputSnapshot.fx"),
                     "Snapshot host compiled a different effect")
        if suffix in ("Disabled","Unqualified"):
            self.require(not (directory / "snapshot_status.json").exists() and "phases" not in host, "Disabled probe produced snapshot evidence")
            self.require(host["unqualified_context_control"] == (suffix=="Unqualified"), "Controlled-host admission metadata mismatch")
            return {"passed": True, "captures": 0, "disabled_control": suffix=="Disabled", "unqualified_context_rejected":suffix=="Unqualified"}
        phases = host["phases"]
        self.require(len(phases) == (2 if suffix == "Resize" else 1), "Resize phase count mismatch")
        generations = []
        for phase in phases:
            state = phase["observation"]
            self.require(state["backend"] == 0x20000 and state["private_gpu_color_and_depth"] and state["borrow_retired_after_callback"] and
                         state["last_issue"] == "" and state["completed_readbacks"] >= 60, "Owned snapshot capture failed")
            self.require(all(not state[key] for key in ("source_main_view_verified", "camera_association_verified", "hud_absence_verified",
                         "motion_producer_connected", "frame_input_provider_registered", "game_frame_generation")), "Capture promoted unverified semantics")
            self.require(state["source_outputs_overwritten_after_private_copy_fixture"] == (suffix == "Invalidated"), "Source-overwrite control metadata mismatch")
            self.require(state["observer_sequences_only"] and state["controlled_single_graphics_queue_host_only"] and state["source_semantic_identity_observed_during_begin_only"] and
                         host["startup_overlay_wait_milliseconds_per_phase"] >= 6000, "Snapshot observation or display-control scope mismatch")
            self.require(len(state["captures"]) == 6, "Snapshot evidence requires six distinct inputs")
            previous = 0
            phase_generations = set()
            for capture in state["captures"]:
                self.captures += 1
                frame = phase["present_id_offset"] + capture["present_observer_id"]
                self.require(0 < frame <= len(host["rendered_depth_frames"]) and frame > previous, "Capture Present ID is stale or out of range")
                previous = frame
                expected = host["rendered_depth_frames"][frame - 1]
                width, height = capture["width"], capture["height"]
                self.require((width, height) == (expected["width"], expected["height"]), "Owned snapshot does not match actual rendered extent")
                phase_generations.add(capture["generation"])
                self.require(capture["generation"] > 0 and capture["same_callback_and_command"] and capture["command_native"] and
                             capture["completed_by_flush_wait_idle_and_map"] and capture["resource_handle_domain"] == "ReShadeResource" and
                             capture["owned_color_resource"] and capture["owned_depth_resource"] and
                             capture["owned_color_resource"] != capture["owned_depth_resource"] != capture["source_semantic_resource"], "Snapshot identity/ownership contract failed")
                self.require(capture["runtime_immediate_command_list_verified"] and capture["transfer_to_host_barrier_recorded"] and
                             capture["readback_host_coherent_by_pinned_backend"] and capture["observer_sequences_only"] and
                             "frame_id" not in capture, "Snapshot submission/readback or observer contract failed")
                self.require(capture["vulkan_native_mapping_contract_checked"] and capture["native_vk_device"] and capture["native_vk_graphics_queue"] and
                             capture["native_vk_color_image"]==capture["owned_color_resource"] and capture["native_vk_depth_image"]==capture["owned_depth_resource"],
                             "Explicit Vulkan texture mapping disagrees with private ReShade texture identity")
                for label in ("color", "depth"):
                    heap = capture[f"native_vk_{label}_heap"]
                    self.require(heap["requested"] == "gpu_only" and heap["queried"] in ("unknown", "gpu_only") and
                                 type(heap["queried_known"]) is bool and heap["queried_known"] == (heap["queried"] != "unknown"),
                                 f"Private {label} allocation heap request/query evidence mismatch")
                ready = suffix != "NoDepth"
                self.require(capture["generic_depth_ready"] == ready, "No-depth control was accepted as scene depth")
                color = self.read(directory / capture["color_file"])
                depth_bytes = self.read(directory / capture["depth_file"])
                display = self.read(directory / capture["display_file"])
                self.require(len(color) == len(depth_bytes) == len(display) == width * height * 4, "Raw GPU snapshot size mismatch")
                depth = struct.unpack(f"<{width * height}f", depth_bytes)
                if suffix == "Invalidated":
                    source_color = self.read(directory / capture["source_color_file"])
                    source_depth = self.read(directory / capture["source_depth_file"])
                    self.require(len(source_color) == len(source_depth) == len(color), "Source overwrite evidence size mismatch")
                    values = struct.unpack(f"<{width * height}f", source_depth)
                    for pixel, value in enumerate(values):
                        self.require(math.isfinite(value) and abs(value - .9375) < 1e-7, "Original GPU depth output was not overwritten after private copy")
                        self.require(max(abs(source_color[pixel * 4 + channel] - (239, 16, 128, 255)[channel]) for channel in range(4)) <= 1,
                                     "Original GPU color output was not overwritten after private copy")
                else:
                    self.require(capture["source_color_file"] is None and capture["source_depth_file"] is None, "Unexpected source-overwrite evidence")
                for y in range(height):
                    if y in (59, 60, 299, 300):
                        continue
                    for x in range(width):
                        if x in (39, 40, 279, 280, 359, 360, 599, 600):
                            continue
                        left = 40 < x < 280 and 60 < y < 300
                        right = 360 < x < 600 and 60 < y < 300
                        wanted = expected["left_rgba8"] if left else expected["right_rgba8"] if right else (4, 8, 16, 255)
                        target_depth = (expected["left_device_depth"] if left else expected["right_device_depth"] if right else 1) if ready else 0
                        pixel = y * width + x
                        self.require(math.isfinite(depth[pixel]) and abs(depth[pixel] - target_depth) <= 2e-6,
                                     f"Owned R32 depth differs from GPU draw at Present {frame}, ({x},{y})")
                        self.require(max(abs(color[pixel * 4 + channel] - wanted[channel]) for channel in range(4)) <= 1,
                                     f"Owned RGBA color differs from GPU draw at Present {frame}, ({x},{y})")
                        self.require(max(abs(display[pixel * 4 + channel] - wanted[channel]) for channel in range(3)) <= 1,
                                     "Offscreen snapshot changed the visible backbuffer")
                        self.pixels += 1
            self.require(len(phase_generations) == 1, "One runtime phase mixed allocation generations")
            generations.append(next(iter(phase_generations)))
        self.require(len(set(generations)) == len(generations), "Resize reused a retired allocation generation")
        return {"passed": True, "captures": len(phases) * 6, "allocation_generations": generations, "source_overwrite_control": suffix == "Invalidated"}

    def run(self):
        for source in SOURCES:
            self.read(ROOT / source)
        reference = ROOT / "LiveInputs/References/ReShade-v6.7.2"
        manifest = self.json(reference / "source-manifest.json")
        self.require(manifest["version"] == "v6.7.2" and len(manifest["files"]) == 9, "Pinned backend reference coverage mismatch")
        for item in manifest["files"]:
            payload = self.read(reference / item["path"])
            self.require(len(payload) == item["bytes"] and hashlib.sha256(payload).hexdigest() == item["sha256"],
                         "Pinned ReShade backend reference changed")
        reports = {suffix or "Normal": self.case(suffix) for suffix in CASES}
        for addon in ("AsteriaInputSnapshot.addon", "AsteriaInputSnapshotFixture.addon"):
            addon_time = (ROOT / "LiveInputs/bin" / addon).stat().st_mtime_ns
            for source in ("LiveInputs/ReShadeInputSnapshot.cpp", "LiveInputs/VulkanTextureInterop.h", "DepthCapture/ReShadeDepthSemanticBridge.h"):
                self.require((ROOT / source).stat().st_mtime_ns <= addon_time, "Add-on predates its source")
        self.require((ROOT / "LiveInputs/InputSnapshotHost.cpp").stat().st_mtime_ns <=
                     (ROOT / "LiveInputs/bin/AsteriaInputSnapshotHost.exe").stat().st_mtime_ns, "Snapshot host predates its source")
        for relative, wanted in list(self.artifacts.items()):
            data = (ROOT / relative).read_bytes()
            self.require(len(data) == wanted["bytes"] and hashlib.sha256(data).hexdigest() == wanted["sha256"], "Artifact changed during validation")
        return reports


def validate():
    verifier = Validator()
    report = {"author": __author__, "passed": False, "scope": "actual_ReShade_Vulkan_owned_color_and_depth_snapshot",
              "game_started": False, "game_frame_generation": False, "main_view_verified": False,
              "camera_association_verified": False, "hud_absence_verified": False, "asynchronous_resource_ring": False,
              "real_game_input_producer_available": False, "frame_input_provider_registered": False}
    try:
        report["cases"] = verifier.run()
        report["passed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
    report.update({"checks": verifier.checks, "pixels_independently_checked": verifier.pixels,
                   "captures_independently_checked": verifier.captures, "artifacts": verifier.artifacts})
    destination = ROOT / "LiveInputs/results/input-snapshot-validation.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


if __name__ == "__main__":
    result = validate()
    print(json.dumps({key: value for key, value in result.items() if key != "artifacts"}, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
