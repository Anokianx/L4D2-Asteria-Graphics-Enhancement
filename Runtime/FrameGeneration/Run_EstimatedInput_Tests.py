from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

__author__ = "Anokianx"

ROOT = Path(__file__).resolve().parent


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxvk", type=Path)
    parser.add_argument("--recording-evidence", type=Path)
    options = parser.parse_args()
    output = ROOT / "results/estimated-input-runs"
    output.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment.pop("VK_INSTANCE_LAYERS", None)
    environment.update({"DISABLE_VK_LAYER_reshade_1": "1", "ASTERIA_FG_ENABLE": "0",
                        "ASTERIA_NATIVE_INPUT_PROBE": "0", "ASTERIA_NATIVE_UNIFORM_PROBE": "0",
                        "ASTERIA_NATIVE_ENGINE_CAMERA": "0", "ASTERIA_NATIVE_PIPELINE_PROBE": "0"})
    depth = subprocess.run([sys.executable, str(ROOT / "DepthCapture/Run_DepthCapture_Tests.py")],
                           cwd=ROOT, env=environment, capture_output=True, timeout=60)
    (output / "depth-validation.log").write_bytes(depth.stdout + depth.stderr)
    if depth.returncode:
        raise RuntimeError((depth.stdout + depth.stderr).decode("utf-8", errors="replace"))
    print(depth.stdout.decode("utf-8", errors="replace"), end="")
    runs = (("FrameAssociation", "bin/AsteriaFrameAssociationContract.exe", ()),
            ("LiveInputs", "bin/AsteriaProducerFrameCoordinatorContract.exe", ()),
            ("MotionReconstruction", "build/AsteriaGpuMotionContract.exe", ()),
            ("EstimatedMotion", "bin/AsteriaGpuFlowContract.exe", (".",)),
            ("EstimatedMotion", "bin/AsteriaEstimatedInterpolationProbe.exe", (".",)),
            ("SceneInputs", "bin/AsteriaSceneInterpolationProbe.exe", (".",)))
    for directory, executable, arguments in runs:
        path = ROOT / directory
        result = subprocess.run([str(path / executable), *arguments], cwd=path, env=environment,
                                capture_output=True, timeout=60)
        (output / (Path(executable).stem + ".log")).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"{executable} failed ({result.returncode}):\n"
                               + (result.stdout + result.stderr).decode("utf-8", errors="replace")[-6000:])
        print(f"Passed: {directory}/{executable}", flush=True)
    semantic = subprocess.run([sys.executable, str(ROOT / "DepthCapture/Run_Semantic_Live_Tests.py")],
                              cwd=ROOT, env=environment, capture_output=True, timeout=60)
    (output / "semantic-live-validation.log").write_bytes(semantic.stdout + semantic.stderr)
    if semantic.returncode:
        raise RuntimeError((semantic.stdout + semantic.stderr).decode("utf-8", errors="replace"))
    print(semantic.stdout.decode("utf-8", errors="replace"), end="")
    association = subprocess.run([sys.executable, str(ROOT / "FrameAssociation/Run_LiveAssociation_Tests.py")],
                                 cwd=ROOT, env=environment, capture_output=True, timeout=60)
    (output / "live-association-validation.log").write_bytes(association.stdout + association.stderr)
    if association.returncode:
        raise RuntimeError((association.stdout + association.stderr).decode("utf-8", errors="replace"))
    print(association.stdout.decode("utf-8", errors="replace"), end="")
    metadata = subprocess.run([sys.executable, str(ROOT / "InputProbe/Tests/PipelineViewportMetadataContract.py")],
                              cwd=ROOT, env=environment, capture_output=True, timeout=20)
    (output / "pipeline-viewport-metadata.log").write_bytes(metadata.stdout + metadata.stderr)
    if metadata.returncode:
        raise RuntimeError((metadata.stdout + metadata.stderr).decode("utf-8", errors="replace"))
    print(metadata.stdout.decode("utf-8", errors="replace"), end="")
    diagnostics = subprocess.run([sys.executable, str(ROOT / "InputProbe/Tests/PipelineDiagnosticJoinContract.py")],
                                 cwd=ROOT, env=environment, capture_output=True, timeout=20)
    (output / "pipeline-diagnostic-join.log").write_bytes(diagnostics.stdout + diagnostics.stderr)
    if diagnostics.returncode:
        raise RuntimeError((diagnostics.stdout + diagnostics.stderr).decode("utf-8", errors="replace"))
    print(diagnostics.stdout.decode("utf-8", errors="replace"), end="")
    if options.dxvk:
        vulkan = subprocess.run([sys.executable, str(ROOT / "DepthCapture/Run_Semantic_Live_Tests.py"),
                                 "--dxvk", str(options.dxvk.resolve())],
                                cwd=ROOT, env=environment, capture_output=True, timeout=60)
        (output / "semantic-vulkan-live-validation.log").write_bytes(vulkan.stdout + vulkan.stderr)
        if vulkan.returncode:
            raise RuntimeError((vulkan.stdout + vulkan.stderr).decode("utf-8", errors="replace"))
        print(vulkan.stdout.decode("utf-8", errors="replace"), end="")
        snapshot = subprocess.run([sys.executable, str(ROOT / "LiveInputs/Run_InputSnapshot_Tests.py"),
                                   "--dxvk", str(options.dxvk.resolve())],
                                  cwd=ROOT, env=environment, capture_output=True, timeout=180)
        (output / "input-snapshot-validation.log").write_bytes(snapshot.stdout + snapshot.stderr)
        if snapshot.returncode:
            raise RuntimeError((snapshot.stdout + snapshot.stderr).decode("utf-8", errors="replace"))
        print(snapshot.stdout.decode("utf-8", errors="replace"), end="")
    result = subprocess.run([sys.executable, str(ROOT / "Validate_EstimatedInputs.py")], cwd=ROOT,
                            env=environment, capture_output=True, timeout=60)
    (output / "validation.log").write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr).decode("utf-8", errors="replace"))
    print(result.stdout.decode("utf-8", errors="replace"), end="")
    scene = subprocess.run([sys.executable, str(ROOT / "SceneInputs/Validate_SceneInterpolation.py")],
                           cwd=ROOT, env=environment, capture_output=True, timeout=60)
    (output / "scene-validation.log").write_bytes(scene.stdout + scene.stderr)
    if scene.returncode:
        raise RuntimeError((scene.stdout + scene.stderr).decode("utf-8", errors="replace"))
    print(scene.stdout.decode("utf-8", errors="replace"), end="")


if __name__ == "__main__":
    main()
