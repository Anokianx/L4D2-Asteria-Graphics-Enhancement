from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

__author__ = "Anokianx"
ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "FrameAssociation/results/LiveAssociationHost"
ARTIFACTS = (
    "FrameAssociation/ObservedFrameAssociation.h", "FrameAssociation/LiveObservedAssociation.h", "FrameAssociation/LiveAssociationContract.cpp",
    "FrameAssociation/ObservedFrameAssociation.h", "FrameAssociation/Build_Live_Contract.bat",
    "FrameAssociation/bin/AsteriaLiveAssociationContract.exe", "InputProbe/AsteriaNativeInputProbe.cpp",
    "FrameAssociation/PresentTicketContract.cpp", "FrameAssociation/Build_PresentTicket_Contract.bat",
    "FrameAssociation/bin/AsteriaPresentTicketContract.exe",
    "InputProbe/NativeUniformSampler.h", "InputProbe/bin/AsteriaNativeInputProbe.addon",
    "InputProbe/NativeInputHostTest.cpp", "InputProbe/bin/AsteriaNativeInputHostTest.exe",
    "Layer/AsteriaDescriptorEvidenceApi.h", "Layer/AsteriaViewportEvidenceApi.h",
    "Layer/AsteriaDrawViewportEvidenceApi.h", "Layer/GraphicsViewportState.h",
    "Layer/AsteriaDrawViewportDiagnosticApi.h", "Layer/GraphicsPipelineDiagnosticShape.h", "Layer/PipelineDiagnosticCache.h",
)


def require(value: bool, message: str) -> None:
    if not value:
        raise RuntimeError(message)


def digests() -> dict[str, str]:
    return {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in ARTIFACTS}


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    before = digests()
    contract = subprocess.run([str(ROOT / "FrameAssociation/bin/AsteriaLiveAssociationContract.exe")],
                              cwd=ROOT / "FrameAssociation", capture_output=True, timeout=30)
    (OUTPUT / "contract.stdout.log").write_bytes(contract.stdout)
    (OUTPUT / "contract.stderr.log").write_bytes(contract.stderr)
    require(contract.returncode == 0, "Live recording association contract failed")
    checks = json.loads((ROOT / "FrameAssociation/results/live-association-contract.json").read_text(encoding="utf-8"))
    require(checks["passed"] and checks["cpu_mock_contract_only"] and not checks["gpu_frame_aligned"] and not checks["game_fg_ready"],
            "Live recording contract overstated GPU/game readiness")
    ticket = subprocess.run([str(ROOT / "FrameAssociation/bin/AsteriaPresentTicketContract.exe")],
                            cwd=ROOT / "FrameAssociation", capture_output=True, timeout=30)
    (OUTPUT / "present-ticket.stdout.log").write_bytes(ticket.stdout)
    (OUTPUT / "present-ticket.stderr.log").write_bytes(ticket.stderr)
    require(ticket.returncode == 0, "Present ticket contract failed")
    ticket_result = json.loads((ROOT / "FrameAssociation/results/present-ticket-contract.json").read_text(encoding="utf-8"))
    require(ticket_result["passed"] and ticket_result["cpu_mock_contract_only"] and ticket_result["bounded_ticket_history"]
            and ticket_result["exact_owner_generation_image_serial_match"] and ticket_result["stale_reuse_resize_foreign_duplicate_fail_closed"]
            and ticket_result["legacy_no_ticket_path_preserved"] and ticket_result["ticket_never_upgrades_gpu_or_game_readiness"]
            and not ticket_result["gpu_frame_aligned"] and not ticket_result["main_view_verified"] and not ticket_result["game_fg_ready"],
            "Present ticket contract overstated or lost a fail-closed boundary")
    environment = os.environ.copy()
    environment.update({"ASTERIA_NATIVE_UNIFORM_PROBE": "1", "ASTERIA_FG_ENABLE": "0",
                        "ASTERIA_NATIVE_UNIFORM_PROFILES": str(ROOT / "Research/NativeShaders/native-uniform-profiles.json")})
    process = subprocess.Popen([str(ROOT / "InputProbe/bin/AsteriaNativeInputHostTest.exe")], cwd=ROOT,
                               env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    source = ROOT / "InputProbe/results/HostTest/NativeUniform/status.json"
    observed = None
    deadline = time.monotonic() + 45
    try:
        while process.poll() is None:
            if time.monotonic() > deadline:
                process.kill()
                raise RuntimeError("Live recording association ReShade host timed out")
            try:
                current = json.loads(source.read_text(encoding="utf-8-sig"))
                association = current.get("live_draw_association", {})
                devices = association.get("devices", [])
                if current.get("pid") == process.pid and current.get("session_active") and devices and all(device["present_observations"] >= 3 for device in devices):
                    observed = current
            except (OSError, ValueError, KeyError):
                pass
            time.sleep(0.025)
        stdout, stderr = process.communicate(timeout=5)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    (OUTPUT / "host.stdout.log").write_bytes(stdout)
    (OUTPUT / "host.stderr.log").write_bytes(stderr)
    require(process.returncode == 0 and observed is not None, "Actual ReShade host did not publish current-process live association metadata")
    association = observed["live_draw_association"]
    require(all(not association[key] for key in ("gpu_frame_aligned", "main_view_verified", "native_camera_ready", "game_fg_ready")),
            "Live host promoted observer metadata to GPU/game readiness")
    require(all(not device["observed_association_available"] and not device["gpu_frame_aligned"] and not device["game_fg_ready"]
                for device in association["devices"]), "D3D9 host without qualified native uniforms produced an accepted camera association")
    require(before == digests(), "Live association source or binary changed during tests")
    active_output = OUTPUT / "active_uniform_observation.json"
    active_temporary = active_output.with_name(active_output.name + ".tmp")
    active_temporary.write_text(json.dumps(observed, indent=2) + "\n", encoding="utf-8")
    active_temporary.replace(active_output)
    result = {"author": __author__, "passed": True, "cpu_contract_checks": checks["checks"], "present_ticket_contract_checks": ticket_result["checks"], "actual_reshade_host_pid": process.pid,
              "host_scope": "actual_ReShade_D3D9_events_without_qualified_native_uniforms",
              "host_present_observations": [device["present_observations"] for device in association["devices"]],
              "actual_vulkan_native_draw_association_validated": False, "gpu_frame_aligned": False, "main_view_verified": False,
              "native_camera_ready": False, "game_fg_ready": False, "game_started": False, "registry_modified": False,
              "hashes": before}
    validation_output = OUTPUT / "validation.json"
    validation_temporary = validation_output.with_name(validation_output.name + ".tmp")
    validation_temporary.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    validation_temporary.replace(validation_output)
    print(json.dumps({key: value for key, value in result.items() if key != "hashes"}, indent=2))


if __name__ == "__main__":
    main()
