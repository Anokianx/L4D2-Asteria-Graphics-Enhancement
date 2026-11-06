from __future__ import annotations

import hashlib
import json
from pathlib import Path

__author__ = "Anokianx"


def main() -> None:
    root = Path(__file__).resolve().parent
    lock = json.loads((root / "upstream-lock.json").read_text(encoding="utf-8"))
    checked = []
    for entry in lock["files"]:
        source = root / "Upstream" / entry["repository"].replace("/", "_") / entry["path"]
        payload = source.read_bytes()
        if len(payload) != entry["bytes"]:
            raise ValueError(f"Source size changed: {source}")
        if hashlib.sha256(payload).hexdigest() != entry["sha256"]:
            raise ValueError(f"Source hash changed: {source}")
        checked.append({"path": str(source.relative_to(root)), "sha256": entry["sha256"]})
    rife = root / "Upstream" / "nihui_rife-ncnn-vulkan"
    ncnn = root / "Upstream" / "Tencent_ncnn"
    texts = {
        "rife_header": (rife / "src/rife.h").read_text(encoding="utf-8"),
        "rife_source": (rife / "src/rife.cpp").read_text(encoding="utf-8"),
        "rife_ci": (rife / ".github/workflows/CI.yml").read_text(encoding="utf-8"),
        "ncnn_header": (ncnn / "src/gpu.h").read_text(encoding="utf-8"),
        "ncnn_source": (ncnn / "src/gpu.cpp").read_text(encoding="utf-8"),
    }
    facts = {
        "rife_public_cpu_mat_interface": "process(const ncnn::Mat& in0image, const ncnn::Mat& in1image, float timestep, ncnn::Mat& outimage)" in texts["rife_header"],
        "rife_vulkan_uploads_cpu_input": "cmd.record_clone(in0, in0_gpu, opt);" in texts["rife_source"],
        "rife_vulkan_downloads_cpu_output": "cmd.record_clone(out_gpu, out, opt);" in texts["rife_source"],
        "rife_waits_for_gpu_completion": "cmd.submit_and_wait();" in texts["rife_source"],
        "rife_windows_ci_targets_x64": "cmake -A x64 ../src" in texts["rife_ci"],
        "ncnn_constructs_device_from_gpu_index": "VulkanDevice(int device_index = get_default_gpu_index());" in texts["ncnn_header"],
        "ncnn_creates_own_vkdevice": "vkCreateDevice(info.physicalDevice(), &deviceCreateInfo, 0, &d->device)" in texts["ncnn_source"],
        "third_party_licenses_preserved": (rife / "LICENSE").is_file() and (ncnn / "LICENSE.txt").is_file(),
    }
    if not all(facts.values()):
        raise ValueError(f"Inspected source facts no longer match: {facts}")
    report = {
        "author": __author__,
        "schema_version": 1,
        "date": "2026-10-16",
        "status": "pass",
        "source_files_verified": len(checked),
        "source_bytes_verified": sum(entry["bytes"] for entry in lock["files"]),
        "facts": facts,
        "files": checked,
        "inference_model_downloaded": False,
        "inference_runtime_built": False,
        "x86_vulkan_inference_proven": False,
        "game_frame_generation_qualified": False,
        "production_runtime_modified": False,
    }
    (root / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "files"}, indent=2))


if __name__ == "__main__":
    main()
