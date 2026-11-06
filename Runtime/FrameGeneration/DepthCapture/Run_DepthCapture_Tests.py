from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess


__author__ = "Anokianx"
ROOT = Path(__file__).resolve().parent
SOURCES = ("Build_DepthCapture.bat", "CandidateSelector.h", "DepthCapture.cpp", "DepthCapture.h", "HostTest.cpp",
           "VulkanFunctions.h", "Shaders/Fixture.frag", "Shaders/Fixture.vert", "Shaders/ResolveDepth.comp",
           "bin/AsteriaDepthCaptureHost.exe", "bin/Fixture.frag.spv", "bin/Fixture.vert.spv", "bin/ResolveDepth.spv",
           "ReShadeDepthSemanticBridge.h", "ReShadeDepthSemanticContract.cpp", "Build_Semantic_Tests.bat",
           "Shaders/AsteriaDepthSemanticBridge.fx", "bin/AsteriaReShadeDepthSemanticContract.exe", "Run_DepthCapture_Tests.py")


def snapshot():
    return {relative: hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() for relative in SOURCES}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    before = snapshot()
    environment = os.environ.copy()
    environment.pop("VK_INSTANCE_LAYERS", None)
    environment.update({"DISABLE_VK_LAYER_reshade_1": "1", "VK_LOADER_LAYERS_DISABLE": "~implicit~",
                        "ASTERIA_FG_ENABLE": "0", "ASTERIA_NATIVE_INPUT_PROBE": "0", "ASTERIA_NATIVE_UNIFORM_PROBE": "0",
                        "ASTERIA_NATIVE_ENGINE_CAMERA": "0", "ASTERIA_NATIVE_PIPELINE_PROBE": "0"})
    started = datetime.now(timezone.utc).isoformat()
    outputs = ("results/gpu_depth_capture.json", "results/readback_r32_float.bin", "results/reshade_semantic_contract.json")
    for relative in outputs:
        (ROOT / relative).unlink(missing_ok=True)
    for executable in ("AsteriaDepthCaptureHost.exe", "AsteriaReShadeDepthSemanticContract.exe"):
        completed = subprocess.run([str(ROOT / "bin" / executable)], cwd=ROOT, env=environment, timeout=60,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (ROOT / "results" / (Path(executable).stem + "-latest.log")).write_bytes(completed.stdout)
        require(completed.returncode == 0, f"{executable} failed: {completed.stdout.decode('utf-8', 'replace')[-4000:]}")
    require(snapshot() == before, "Depth capture sources or binaries changed while executing tests")
    gpu = json.loads((ROOT / outputs[0]).read_text(encoding="utf-8"))
    semantic = json.loads((ROOT / outputs[2]).read_text(encoding="utf-8"))
    require(gpu["passed"] is True and gpu["actual_gpu_depth_capture_validated"] is True and gpu["checks"] >= 9276,
            "Real GPU depth fixture contract failed")
    require(gpu["fg_ready"] is False and gpu["native_game_depth_validated"] is False and gpu["game_started"] is False,
            "Isolated depth fixture promoted game readiness")
    require(semantic["passed"] is True and semantic["checks"] >= 44 and semantic["cpu_api_mock_contract"] is True,
            "Semantic mock contract failed")
    require(semantic["fg_ready"] is False and semantic["actual_game_binding_observed"] is False and semantic["game_started"] is False,
            "Semantic mock contract promoted game readiness")
    payload = (ROOT / outputs[1]).read_bytes()
    require(len(payload) == 64 * 48 * 3 * 4, "Depth raw readback length changed")
    values = struct.unpack("<9216f", payload)
    maximum_error = 0.0
    for y in range(48):
        for x in range(64):
            plane = .125 + (x % 8) * .0625 + (y % 4) * .03125
            for index, expected in enumerate((plane, .9375, plane + 3 * .0078125)):
                actual = values[index * 64 * 48 + y * 64 + x]
                require(math.isfinite(actual) and abs(actual - expected) <= 1e-6, "Depth raw pixel mismatch")
                maximum_error = max(maximum_error, abs(actual - expected))
    require(abs(maximum_error - gpu["max_absolute_error"]) <= 1e-12, "Depth GPU error report differs from raw evidence")
    report = {"author": "Anokianx", "passed": True, "gpu_report": "gpu_depth_capture.json", "checks": gpu["checks"],
              "captured_depth_format": "D24S8_MSAA4", "resolved_depth_format": "R32_FLOAT", "game_started": False,
              "fg_ready": False, "hashes": before, "fresh_execution": True, "execution_started_utc": started,
              "execution_completed_utc": datetime.now(timezone.utc).isoformat(), "source_hashes_unchanged_during_run": True,
              "raw_gpu_pixels_independently_checked": len(values), "raw_gpu_maximum_error": maximum_error,
              "semantic_mock_checks": semantic["checks"],
              "output_sha256": {relative: hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() for relative in outputs}}
    (ROOT / "results/validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Depth capture fresh execution passed: {gpu['checks']} checks, {len(values)} independently checked GPU pixels, {semantic['checks']} semantic mock checks")


if __name__ == "__main__":
    main()
