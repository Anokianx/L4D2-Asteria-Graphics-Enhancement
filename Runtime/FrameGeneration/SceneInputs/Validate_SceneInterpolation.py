from __future__ import annotations

import hashlib
import json
import math
import re
import statistics
import struct
import sys
from pathlib import Path

__author__ = "Anokianx"
ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT, FRAME_COUNT = 640, 360, 8
OUTPUT = Path("SceneInputs/results/SceneInterpolation")


class SceneEvidenceValidator:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.checks = 0
        self.artifacts = {}
        self.summaries = {}
        self.readbacks = []

    def require(self, condition, message):
        self.checks += 1
        if not condition:
            raise ValueError(message)

    def path(self, relative):
        target = (self.root / str(relative).replace("\\", "/")).resolve()
        self.require(self.root in target.parents, f"Evidence path escapes prototype: {relative}")
        return target

    def read(self, relative, readback=False):
        path = self.path(relative)
        payload = path.read_bytes()
        self.artifacts[path.relative_to(self.root).as_posix()] = {
            "sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload)
        }
        if readback:
            self.readbacks.append(path)
        return payload

    def fields(self, value, expected):
        for key, wanted in expected.items():
            actual = value.get(key)
            self.require(type(actual) is type(wanted) and actual == wanted,
                         f"Report field mismatch: {key}={actual!r}, expected {wanted!r}")

    def near(self, actual, expected, tolerance=1e-6):
        self.require(type(actual) in (int, float) and math.isfinite(actual) and
                     abs(actual - expected) <= tolerance,
                     f"Reported numeric value differs from independent readback: {actual!r} versus {expected!r}")

    def ppm(self, relative):
        payload = self.read(relative, readback=True)
        header = re.match(rb"P6\n([0-9]+) ([0-9]+)\n255\n", payload)
        self.require(header is not None, f"Invalid PPM header: {relative}")
        width, height = map(int, header.groups())
        self.require((width, height) == (WIDTH, HEIGHT), f"Unexpected image dimensions: {relative}")
        pixels = payload[header.end():]
        self.require(len(pixels) == WIDTH * HEIGHT * 3, f"Incomplete PPM payload: {relative}")
        return pixels

    def floats(self, relative):
        payload = self.read(relative, readback=True)
        self.require(len(payload) == WIDTH * HEIGHT * 4, f"Float readback size mismatch: {relative}")
        return payload, struct.unpack(f"<{WIDTH * HEIGHT}f", payload)

    @staticmethod
    def texture_hash(x, y):
        value = (x * 0x9E3779B9 + y * 0x85EBCA6B + 17) & 0xFFFFFFFF
        value ^= value >> 16
        value = value * 0x7FEB352D & 0xFFFFFFFF
        return value ^ (value >> 15)

    def scene(self, color, depth, frame_number):
        maximum_color_error = 0
        maximum_depth_error = 0.0
        object_pixels = 0
        for y in range(HEIGHT):
            for x in range(WIDTH):
                local_x, local_y = x - 220 - frame_number * 12, y - 108
                inside = 0 <= local_x < 112 and 0 <= local_y < 136
                noise = self.texture_hash(local_x if inside else x, local_y if inside else y)
                expected = ((180 + noise % 70, 70 + (noise >> 8) % 70, 15 + (noise >> 16) % 40)
                            if inside else
                            (15 + noise % 50, 25 + (noise >> 8) % 50, 60 + (noise >> 16) % 50))
                pixel = y * WIDTH + x
                offset = pixel * 3
                color_error = max(abs(color[offset + channel] - expected[channel]) for channel in range(3))
                self.require(color_error <= 1,
                             f"GPU scene texture hash mismatch at frame {frame_number}, pixel ({x},{y})")
                wanted_depth = 0.4 if inside else 0.8
                actual_depth = depth[pixel]
                depth_error = abs(actual_depth - wanted_depth)
                self.require(math.isfinite(actual_depth) and depth_error <= 1e-6,
                             f"GPU pre-clear depth/sample resolve mismatch at frame {frame_number}, pixel ({x},{y})")
                maximum_color_error = max(maximum_color_error, color_error)
                maximum_depth_error = max(maximum_depth_error, depth_error)
                object_pixels += inside
        self.require(object_pixels == 112 * 136, "GPU scene object extent changed")
        return {"color_pixels_checked": WIDTH * HEIGHT, "depth_pixels_checked": WIDTH * HEIGHT,
                "maximum_color_error_lsb": maximum_color_error, "maximum_depth_error": maximum_depth_error,
                "object_pixels": object_pixels}

    def centroid(self, pixels):
        position_sum = count = 0
        for y in range(100, 260):
            for x in range(WIDTH):
                offset = (y * WIDTH + x) * 3
                red, green, blue = pixels[offset:offset + 3]
                if red > 180 and 60 < green < 180 and blue < 100:
                    position_sum += x
                    count += 1
        self.require(count > 0, "Rendered object disappeared from centroid region")
        return position_sum / count

    def motion(self, directory, number):
        raw = self.read(directory / "estimated_motion.rg16f", readback=True)
        self.require(len(raw) == WIDTH * HEIGHT * 4, "Estimated motion byte size mismatch")
        vectors = tuple(struct.iter_unpack("<ee", raw))
        _, qualities = self.floats(directory / "confidence.r32f")
        for index, ((x, y), confidence) in enumerate(zip(vectors, qualities)):
            self.require(math.isfinite(x) and math.isfinite(y) and abs(x) <= 1 and abs(y) <= 1 and
                         math.isfinite(confidence) and 0 <= confidence <= 1,
                         f"Invalid motion/confidence at frame {number}, pixel {index}")
            self.require((confidence == 0 and x == 0 and y == 0) or confidence >= 0.25,
                         f"Rejected estimated motion retained displacement at frame {number}, pixel {index}")
        accepted = tested = 0
        error = 0.0
        expected_x = -12 if number else 0
        for y in range(128, 224):
            for x in range(240 + number * 12, 312 + number * 12):
                tested += 1
                index = y * WIDTH + x
                if qualities[index] >= 0.25:
                    accepted += 1
                    u, v = vectors[index]
                    error += math.hypot(u * WIDTH - expected_x, v * HEIGHT)
        self.require(tested == 6912 and accepted > 0, "Estimated object region has no verified motion")
        coverage = accepted / tested
        mean_error = error / accepted
        self.require(coverage > 0.30 and mean_error < 1.25, "Estimated motion coverage or endpoint accuracy failed")
        return {"accepted": accepted, "tested": tested, "coverage": coverage, "mean_endpoint_error_pixels": mean_error,
                "confidence_pixels_checked": WIDTH * HEIGHT}

    def flow(self, directory, number):
        flow_width, flow_height = (WIDTH + 7) // 8, (HEIGHT + 7) // 8
        samples = 0
        for name, wanted, x0 in (("forward", -12 if number else 0, 240 + number * 12),
                                 ("reverse", 12 if number else 0, 240 + max(0, number - 1) * 12)):
            raw = self.read(directory / (name + "_flow.rg16i"), readback=True)
            self.require(len(raw) == flow_width * flow_height * 4, f"{name} block-flow size mismatch")
            for y in range(144, 208, 8):
                for x in range(x0 + 16, x0 + 56, 8):
                    dx, dy = struct.unpack_from("<hh", raw, ((y // 8) * flow_width + x // 8) * 4)
                    self.require(abs(dx - wanted) <= 1 and abs(dy) <= 1,
                                 f"{name} optical flow disagrees with rendered frame pair {number}")
                    samples += 1
        return samples

    def frames(self, report):
        entries = report.get("frames")
        self.require(isinstance(entries, list) and len(entries) == FRAME_COUNT, "Eight continuous input frames are required")
        prior_color = prior_depth = None
        summaries = []
        for number, frame in enumerate(entries):
            self.fields(frame, {"frame_id": number, "provider_frame_id": number, "reset": number == 0,
                                "depth_pixels_verified": WIDTH * HEIGHT, "optical_flow_dispatches": 3})
            self.require(frame["forward_context_frame_index"] > 5 and frame["reverse_context_frame_index"] > 5,
                         "SDK warmup output was accepted as initialized estimated motion")
            directory = OUTPUT / f"frame_{number}"
            before, current, generated = (self.ppm(directory / (name + ".ppm")) for name in ("previous", "current", "generated"))
            previous_raw, previous_depth = self.floats(directory / "previous_depth.r32f")
            current_raw, current_depth = self.floats(directory / "current_depth.r32f")
            previous_scene = self.scene(before, previous_depth, max(0, number - 1))
            current_scene = self.scene(current, current_depth, number)
            self.near(frame["max_depth_absolute_error"], current_scene["maximum_depth_error"], 1e-7)
            if number:
                self.require(before == prior_color and previous_raw == prior_depth,
                             "Consecutive inputs do not retain the preceding GPU color/depth pair")
            else:
                self.require(before == current and previous_raw == current_raw, "Initial history must seed the actual initial GPU scene")
            prior_color, prior_depth = current, current_raw
            centers = [self.centroid(pixels) for pixels in (before, current, generated)]
            differences = [sum(generated[index:index + 3] != source[index:index + 3]
                               for index in range(0, len(source), 3)) for source in (before, current)]
            expected = (centers[0] + centers[1]) / 2
            error = abs(centers[2] - expected)
            for key, measured in zip(("previous_centroid_x", "current_centroid_x", "generated_centroid_x"), centers):
                self.near(frame[key], measured)
            self.near(frame["expected_centroid_x"], expected)
            self.near(frame["midpoint_centroid_error_pixels"], error)
            self.require(differences == [frame["pixels_different_from_previous"], frame["pixels_different_from_current"]],
                         "Reported generated-pixel differences disagree with image readback")
            if number:
                self.require(frame["midpoint_verified"] is True and error < 1 and min(differences) > 500 and
                             centers[0] + 1 < centers[2] < centers[1] - 1,
                             "Generated output is an input copy or does not lie at the actual midpoint")
            else:
                self.require(frame["midpoint_verified"] is False, "Initial reset was counted as a generated midpoint")
            measured = self.motion(directory, number)
            self.require(frame["accepted_object_pixels"] == measured["accepted"] and frame["tested_object_pixels"] == measured["tested"],
                         "Reported estimated-motion pixel counts disagree")
            self.near(frame["coverage"], measured["coverage"])
            self.near(frame["mean_motion_endpoint_error_pixels"], measured["mean_endpoint_error_pixels"])
            summaries.append({"frame_id": number, "previous_scene": previous_scene, "current_scene": current_scene,
                              "midpoint_error_pixels_recomputed": error, "distinct_pixel_counts_recomputed": differences,
                              "estimated_motion": measured, "raw_flow_samples_checked": self.flow(directory, number)})
        self.summaries["continuous_gpu_scene"] = {"input_frames": FRAME_COUNT, "verified_midpoints": FRAME_COUNT - 1,
                "color_pixels_independently_checked": WIDTH * HEIGHT * FRAME_COUNT * 2,
                "depth_pixels_independently_checked": WIDTH * HEIGHT * FRAME_COUNT * 2,
                "synthetic_scene": True, "real_game_inputs": False, "frames": summaries}

    def gpu_timing(self, report):
        metadata, summary = report["gpu_timing"], report["gpu_timing_summary"]
        names = ("forward_of", "reverse_seed_of", "reverse_pair_of", "filter", "prepare_and_fi")
        self.fields(metadata, {"counter_difference": "masked_modulo_queue_timestampValidBits", "query_slots": 10,
                               "game_fps_measured": False, "four_k_performance_measured": False,
                               "excludes_scene_graphics_capture_copy_and_readback": True})
        self.require(type(metadata.get("available")) is bool, "GPU timing availability is not explicit")
        available = metadata["available"]
        bits, period = metadata["queue_timestamp_valid_bits"], metadata["timestamp_period_nanoseconds"]
        if available:
            self.require(type(bits) is int and 1 <= bits <= 64 and type(period) in (int, float) and
                         math.isfinite(period) and period > 0 and metadata["unavailable_reason"] is None,
                         "Invalid available GPU timestamp metadata")
        else:
            self.require(isinstance(metadata["unavailable_reason"], str) and bool(metadata["unavailable_reason"]),
                         "Unavailable GPU timing has no explanation")
        mask = (1 << bits) - 1 if available else 0
        durations = {name: [] for name in (*names, "sum")}
        timed = segments = 0
        for frame in report["frames"]:
            self.require(type(frame["gpu_timing_available"]) is bool, "Per-frame timing availability is not explicit")
            work, raw = frame["gpu_work_ms"], frame["gpu_timestamp_ticks"]
            if not frame["gpu_timing_available"]:
                self.require(isinstance(frame["gpu_timing_unavailable_reason"], str) and bool(frame["gpu_timing_unavailable_reason"]) and
                             all(work.get(name) is None for name in durations) and not raw,
                             "Unavailable GPU measurements must retain null times, empty ticks, and a reason")
                continue
            self.require(available and frame["gpu_timing_unavailable_reason"] is None and set(raw) == set(names),
                         "GPU frame timestamp metadata/segments incomplete")
            total = 0.0
            for name in names:
                ticks = raw[name]
                values = [ticks[key] for key in ("begin", "end", "masked_delta")]
                self.require(all(type(value) is int and 0 <= value < 1 << 64 for value in values), "GPU query is not uint64")
                begin, end, declared = values
                delta = ((end & mask) - (begin & mask)) & mask
                self.require(delta == declared, "Masked GPU timestamp delta differs from raw query counters")
                milliseconds = delta * period / 1_000_000
                self.near(work[name], milliseconds, 1e-9)
                durations[name].append(milliseconds)
                total += milliseconds
                segments += 1
            self.near(work["sum"], total, 1e-9)
            durations["sum"].append(total)
            timed += 1
        complete = timed == FRAME_COUNT
        self.fields(summary, {"available": complete, "frame_count": FRAME_COUNT, "timed_frame_count": timed if complete else 0})
        recalculated = {}
        for name, values in durations.items():
            if complete:
                median, maximum = statistics.median(values), max(values)
                self.near(summary["gpu_work_ms"][name]["median"], median, 1e-9)
                self.near(summary["gpu_work_ms"][name]["max"], maximum, 1e-9)
                recalculated[name] = {"median": median, "max": maximum}
            else:
                self.require(summary["gpu_work_ms"][name]["median"] is None and summary["gpu_work_ms"][name]["max"] is None,
                             "Incomplete GPU timing was reported as measured zero time")
                recalculated[name] = {"median": None, "max": None}
        self.summaries["gpu_timing"] = {"available": complete, "timed_frames": timed,
                "segments_independently_recomputed": segments, "gpu_work_ms_recomputed": recalculated,
                "excludes_scene_graphics_capture_copy_and_readback": True,
                "game_fps_measured": False, "four_k_performance_measured": False}

    def artifacts_and_freshness(self):
        source_extensions = {".h", ".hpp", ".cpp", ".py", ".bat", ".vert", ".frag", ".comp", ".spv", ".exe"}
        scene_root = self.path("SceneInputs")
        for path in sorted(scene_root.rglob("*")):
            if path.is_file() and path.suffix.lower() in source_extensions and "results" not in path.relative_to(scene_root).parts:
                self.read(path.relative_to(self.root))
        dependencies = ("DepthCapture/DepthCapture.cpp", "DepthCapture/DepthCapture.h", "DepthCapture/VulkanFunctions.h",
                        "DepthCapture/Shaders/ResolveDepth.comp", "DepthCapture/bin/ResolveDepth.spv",
                        "EstimatedMotion/GpuFlowContract.cpp", "EstimatedMotion/EstimatedInterpolationProbe.cpp",
                        "EstimatedMotion/BidirectionalFlowMotion.h", "EstimatedMotion/FilterFlow.comp", "EstimatedMotion/FilterFlow.spv",
                        "Port/InterpolationProbe.cpp", "Layer/FrameGenerationInputs.h", "Port/include/AsteriaVkHandles.h",
                        "Layer/AsteriaFrameInputApi.h", "Layer/FrameInputBridge.h", "Layer/FrameInputExports.h",
                        "Layer/Tests/FrameInputProducerFixture.h", "Layer/Tests/FrameInputProducerFixture.cpp",
                        "Layer/Tests/bin/AsteriaFrameInputProducerFixture.dll")
        for relative in dependencies:
            self.read(relative)
        executables = list((scene_root / "bin").glob("*.exe"))
        self.require(len(executables) == 1, "Scene interpolation requires one identifiable built host executable")
        executable = executables[0]
        built = executable.stat().st_mtime_ns
        evidence_time = self.path(OUTPUT / "scene_interpolation.json").stat().st_mtime_ns
        self.require(built <= evidence_time, "Scene interpolation report predates its executable")
        for path in scene_root.rglob("*"):
            if path.is_file() and path.suffix.lower() in {".h", ".hpp", ".cpp"} and "results" not in path.relative_to(scene_root).parts:
                self.require(path.stat().st_mtime_ns <= built, f"Built host predates source: {path.name}")
        for relative in dependencies:
            path = self.path(relative)
            boundary = evidence_time if path.suffix == ".spv" else built
            self.require(path.stat().st_mtime_ns <= boundary, f"Host/report predates dependency: {relative}")
        shader_pairs = [(self.path("DepthCapture/Shaders/ResolveDepth.comp"), self.path("DepthCapture/bin/ResolveDepth.spv")),
                        (self.path("EstimatedMotion/FilterFlow.comp"), self.path("EstimatedMotion/FilterFlow.spv"))]
        for shader in scene_root.rglob("*"):
            if not shader.is_file() or shader.suffix.lower() not in {".vert", ".frag", ".comp"}:
                continue
            candidates = list(scene_root.rglob(shader.name + ".spv"))
            self.require(len(candidates) == 1, f"Shader has no unambiguous compiled SPIR-V: {shader.name}")
            shader_pairs.append((shader, candidates[0]))
        self.require(len(shader_pairs) >= 4, "GPU scene vertex/fragment shader source and SPIR-V evidence missing")
        for shader, spirv in shader_pairs:
            payload = self.read(spirv.relative_to(self.root))
            self.require(len(payload) >= 20 and struct.unpack_from("<I", payload)[0] == 0x07230203,
                         f"Compiled shader is not SPIR-V: {spirv.name}")
            self.require(shader.stat().st_mtime_ns <= spirv.stat().st_mtime_ns <= evidence_time,
                         f"Shader source/SPIR-V/report timestamp order differs: {shader.name}")
        for path in self.readbacks:
            self.require(built <= path.stat().st_mtime_ns <= evidence_time,
                         f"Scene readback predates executable or follows report: {path.name}")
        for relative, expected in self.artifacts.copy().items():
            payload = self.path(relative).read_bytes()
            self.require(len(payload) == expected["bytes"] and hashlib.sha256(payload).hexdigest() == expected["sha256"],
                         f"Artifact changed during validation: {relative}")
        self.summaries["build_freshness"] = {"executable": executable.relative_to(self.root).as_posix(),
                "readbacks_checked": len(self.readbacks), "shader_source_chains_checked": len(shader_pairs),
                "timestamp_check_is_not_reproducible_build_proof": True}

    def run(self):
        report = json.loads(self.read(OUTPUT / "scene_interpolation.json").decode("utf-8-sig"))
        self.require(isinstance(report, dict), "Scene interpolation report is not an object")
        self.fields(report, {"author": __author__, "passed": True, "stage": "complete", "process_bits": 32,
                             "synthetic_scene": True, "real_game_inputs": False, "game_started": False,
                             "game_frame_generation": False, "ground_truth_motion_uploaded": False,
                             "motion_source": "Estimated", "generated_frames_presented": 0,
                             "width": WIDTH, "height": HEIGHT, "cpu_color_uploaded": False,
                             "cpu_depth_uploaded": False, "cpu_motion_uploaded": False,
                             "color_and_depth_written_by_same_gpu_graphics_pass": True,
                             "depth_copied_before_original_clear": True, "normal_z": True, "source_samples": 4,
                             "source_depth_format": "VK_FORMAT_D24_UNORM_S8_UINT", "resolved_depth_format": "VK_FORMAT_R32_SFLOAT",
                             "synchronous_queue_idle_fixture": True, "camera_source": "explicit_controlled_fixture_constants",
                             "hud_absent_in_controlled_fixture": True, "hud_composition_validated": False,
                             "provider_scope": "cross_dll_C_ABI_controlled_fixture_only", "production_provider_registered": False,
                             "main_game_view_confirmed": False, "gpu_l4d2_frame_association_verified": False,
                             "warmup_frames_are_real_game_frames": False,
                             "active_optical_flow_dispatches_per_input_frame": 3,
                             "gpu_scene_depth_estimated_interpolation_verified": True,
                             "frame_input_session_contract_verified": True})
        self.require(report["input_frames"] == report["official_interpolation_dispatches"] == FRAME_COUNT and
                     report["verified_estimated_motion_midpoints"] == FRAME_COUNT - 1,
                     "Scene interpolation frame/dispatch/midpoint counts differ")
        self.require(report["forward_of_dispatches_total"] == 14 and report["reverse_of_dispatches_total"] == 22 and
                     report["all_of_dispatches_including_synthetic_warmup"] == 36,
                     "Forward/reverse dispatch counts do not match synthetic warmup and eight actual pairs")
        self.fields(report["cross_dll_native_image_leases"], {"submissions": FRAME_COUNT, "releases": FRAME_COUNT,
                    "actual_gpu_images": True, "consumer_session_is_controlled": True, "actual_swapchain_present_ticket": False})
        self.frames(report)
        self.gpu_timing(report)
        self.artifacts_and_freshness()


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT
    validator = SceneEvidenceValidator(root)
    result = {"author": __author__, "schema_version": 1, "passed": False,
              "scope": "isolated_continuous_GPU_rendered_color_depth_estimated_motion_interpolation",
              "synthetic_scene": True, "real_game_inputs": False, "game_started": False,
              "game_frame_generation": False, "native_fg_ready": False, "main_view_verified": False,
              "real_game_input_producer_available": False, "hud_composition_validated": False,
              "generated_frames_presented": 0, "performance_benchmark": False}
    try:
        validator.run()
        result["passed"] = True
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    result["checks"] = validator.checks
    result["modules"] = validator.summaries
    result["artifacts"] = dict(sorted(validator.artifacts.items()))
    output = validator.root / OUTPUT / "validation.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key not in {"artifacts", "modules"}}, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
