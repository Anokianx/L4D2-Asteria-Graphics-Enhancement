from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
from typing import Any

__author__ = "Anokianx"
ROOT = Path(__file__).resolve().parent
RESULTS = ROOT / "results"
SIDECARS = {
    "native_uniform": Path("NativeUniform/collection_observation.json"),
    "provenance": Path("Provenance/collection_observation.json"),
    "mapped_layer": Path("mapped_layer_collection_observation.json"),
}
READINESS_FLAGS = {
    "native_fg_ready", "game_fg_ready", "fg_ready", "frame_generation_enabled", "frame_generation_active",
    "actual_gpu_inputs_validated", "native_camera_ready", "camera_ready", "camera_semantics_verified",
    "camera_alignment_verified", "main_view_verified", "gpu_frame_aligned", "motion_vectors_verified",
}


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def normpath(value: str | Path) -> str:
    return str(Path(value).resolve()).casefold()


def finite_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def valid_viewport(value: Any) -> bool:
    return (isinstance(value, list) and len(value) == 6 and all(finite_number(item) for item in value)
            and value[2] > 0 and value[3] != 0 and 0 <= value[4] <= 1 and 0 <= value[5] <= 1)


def valid_pipeline_viewport(value, device_generation, command_generation, recording_generation, pipeline, viewport):
    if not isinstance(value, dict):
        return False
    flags = value.get("flags", 0)
    mode = value.get("mode")
    integer_fields = ("status", "flags", "mode", "source", "viewport_count", "rejection_reason", "pipeline_generation", "bind_sequence",
                      "device_generation", "command_generation", "recording_generation", "pipeline", "setting_sequence")
    if any(not isinstance(value.get(field), int) or isinstance(value.get(field), bool) for field in integer_fields):
        return False
    if (value.get("status")!=0 or value.get("rejection_reason")!=0 or value.get("recording_contract_supported") is not True or value.get("observer_only") is not True
            or flags&~7 or flags&3!=3 or value.get("viewport_count")!=1 or mode not in (1,2,3)
            or not value.get("pipeline_generation",0)>0 or not value.get("bind_sequence",0)>0
            or value.get("device_generation")!=device_generation or value.get("command_generation")!=command_generation
            or value.get("recording_generation")!=recording_generation or value.get("pipeline")!=pipeline
            or not device_generation or not command_generation or not recording_generation or not pipeline
            or not valid_viewport(value.get("viewport")) or value["viewport"][4] == value["viewport"][5]
            or value["viewport"]!=viewport): return False
    if mode==1: return value.get("source")==0 and not flags&4 and value.get("setting_sequence")==0
    return bool(flags&4) and value.get("setting_sequence",0)>0 and value.get("source") in ((1,) if mode==2 else (2,3))


def inspect_readiness(value: Any, path: str, failures: list[str], seen: dict[str, list[str]]) -> None:
    if isinstance(value, dict):
        for key, item in value.items():
            name = str(key).casefold()
            location = f"{path}.{key}"
            if name in READINESS_FLAGS:
                seen.setdefault(name, []).append(location)
                if item is not False:
                    failures.append(f"{location} must be false; observed {item!r}")
            elif name in {"generated_frames", "generated_frames_presented"}:
                seen.setdefault(name, []).append(location)
                if not isinstance(item, int) or isinstance(item, bool) or item != 0:
                    failures.append(f"{location} must be zero; observed {item!r}")
            elif name == "camera_probe_enabled":
                seen.setdefault(name, []).append(location)
                if item is not False:
                    failures.append(f"{location} must be false; observed {item!r}")
            elif name == "available" and "camera" in path.casefold() and item is True:
                failures.append(f"{location} reports an available camera while camera probing is disabled")
            inspect_readiness(item, location, failures, seen)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            inspect_readiness(item, f"{path}[{index}]", failures, seen)


def inspect_native_evidence(uniform: dict[str, Any], failures: list[str]) -> dict[str, Any]:
    association = uniform.get("live_draw_association")
    if not isinstance(association, dict):
        failures.append("NativeUniform sidecar has no live_draw_association evidence")
        return {"available": False, "devices": [], "evidence_devices": 0}

    for field in ("gpu_frame_aligned", "main_view_verified", "native_camera_ready", "game_fg_ready"):
        if association.get(field) is not False:
            failures.append(f"live_draw_association.{field} must be false")
    if association.get("execution_events_are_submission_intent_only") is not True:
        failures.append("live_draw_association does not declare execution observations as submission intent only")

    devices = association.get("devices")
    if not isinstance(devices, list) or not devices:
        failures.append("live_draw_association contains no native Vulkan observer device records")
        return {"available": True, "devices": [], "evidence_devices": 0}

    found = 0
    summaries = []
    for index, device in enumerate(devices):
        if not isinstance(device, dict):
            failures.append(f"live_draw_association.devices[{index}] is not an object")
            continue
        prefix = f"live_draw_association.devices[{index}]"
        for field in ("gpu_frame_aligned", "main_view_verified", "native_camera_ready", "game_fg_ready"):
            if device.get(field) is not False:
                failures.append(f"{prefix}.{field} must be false")
        evidence = device.get("last_native_recording_evidence")
        if not isinstance(evidence, dict) or not evidence:
            summaries.append({"owner": device.get("owner"), "evidence_present": False})
            continue
        found += 1
        evidence_prefix = f"{prefix}.last_native_recording_evidence"
        if evidence.get("observer_only") is not True:
            failures.append(f"{evidence_prefix}.observer_only must be true")
        if evidence.get("scene_epoch") != 0 or evidence.get("scene_epoch_verified") is not False:
            failures.append(f"{evidence_prefix} must retain an unknown, unverified scene epoch")
        pipeline_supported=evidence.get("viewport_draw_state_verified") is True
        if not isinstance(evidence.get("viewport_draw_state_verified"), bool):
            failures.append(f"{evidence_prefix} lacks an explicit pipeline viewport recording-state result")
        if pipeline_supported and not valid_pipeline_viewport(evidence.get("pipeline_viewport"),evidence.get("native_device_generation"),
                evidence.get("native_command_allocation_generation"),evidence.get("native_command_recording_generation"),
                evidence.get("pipeline"),evidence.get("viewport")):
            failures.append(f"{evidence_prefix} claims pipeline viewport support without matched recording evidence")
        static_viewport=pipeline_supported and evidence.get("pipeline_viewport",{}).get("mode")==1
        if not static_viewport and (evidence.get("native_viewport_source") not in (1, 2, 3) or not isinstance(evidence.get("native_viewport_set_sequence"), int) or evidence["native_viewport_set_sequence"] <= 0):
            failures.append(f"{evidence_prefix} has no valid native viewport source or set sequence")
        for field in ("native_command", "native_command_allocation_generation", "native_command_recording_generation", "draw_ordinal"):
            value = evidence.get(field)
            if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                failures.append(f"{evidence_prefix}.{field} must be a real nonzero observed integer")
        if evidence.get("viewport_known") is not True:
            failures.append(f"{evidence_prefix}.viewport_known must be true")
        viewport = evidence.get("viewport")
        width, height = evidence.get("width"), evidence.get("height")
        if (not valid_viewport(viewport)
                or not isinstance(width, int) or isinstance(width, bool) or width <= 0
                or not isinstance(height, int) or isinstance(height, bool) or height <= 0):
            failures.append(f"{evidence_prefix} lacks a finite nonzero viewport extent and positive attachment extent")
            viewport_matches_full_positive_attachment = False
        else:
            viewport_matches_full_positive_attachment = viewport == [0, 0, width, height, 0, 1]

        attachment_generations = {}
        for attachment in ("depth", "color"):
            generation = evidence.get(f"{attachment}_generation")
            identity = evidence.get(f"{attachment}_identity")
            if not isinstance(generation, int) or isinstance(generation, bool) or generation <= 0:
                failures.append(f"{evidence_prefix}.{attachment}_generation is missing or zero")
            if not isinstance(identity, dict) or identity.get("generation") != generation or not identity.get("handle"):
                failures.append(f"{evidence_prefix}.{attachment}_identity does not match its observed generation/handle")
            elif identity.get("handle_domain") != "ReShadeResource":
                failures.append(f"{evidence_prefix}.{attachment}_identity has an unexpected handle domain")
            attachment_generations[attachment] = generation

        if device.get("observed_association_available") is not False:
            failures.append(f"{prefix} accepted an association despite its unverified scene epoch")
        accepted = device.get("accepted_recordings")
        if accepted != 0:
            failures.append(f"{prefix}.accepted_recordings must remain zero without a verified scene epoch")
        summaries.append({"owner": device.get("owner"), "evidence_present": True,
            "scene_epoch": evidence.get("scene_epoch"), "scene_epoch_verified": evidence.get("scene_epoch_verified"),
            "native_command": evidence.get("native_command"),
            "native_command_allocation_generation": evidence.get("native_command_allocation_generation"),
            "native_command_recording_generation": evidence.get("native_command_recording_generation"),
            "draw_ordinal": evidence.get("draw_ordinal"), "viewport": viewport,
            "native_viewport_source": evidence.get("native_viewport_source"),
            "native_viewport_set_sequence": evidence.get("native_viewport_set_sequence"),
            "viewport_draw_state_verified": evidence.get("viewport_draw_state_verified"),
            "pipeline_viewport":evidence.get("pipeline_viewport"),
            "viewport_matches_full_positive_attachment": viewport_matches_full_positive_attachment,
            "attachment_generations": attachment_generations, "accepted_recordings": accepted,
            "rejected_recordings": device.get("rejected_recordings"),
            "last_interval_coverage_losses": device.get("last_interval_coverage_losses"),
            "pending_coverage_losses": device.get("pending_coverage_losses"),
            "total_coverage_losses": device.get("total_coverage_losses")})

    if not found:
        failures.append("No last_native_recording_evidence was retained for any observed device")
    return {"available": True, "devices": summaries, "evidence_devices": found,
            "scope": association.get("scope"), "device_tracking_failures": association.get("device_tracking_failures")}


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate archived L4D2 recording-association observer evidence without changing inputs.")
    parser.add_argument("--directory", required=True, type=Path, help="Experiment results directory; relative paths start at InputProbe.")
    args = parser.parse_args()
    directory = args.directory if args.directory.is_absolute() else ROOT / args.directory
    directory = directory.resolve()
    if not directory.is_relative_to(RESULTS.resolve()):
        parser.error("Evidence directory must stay under InputProbe/results")
    output_path = directory / "recording_association_validation.json"
    failures: list[str] = []
    checks: list[dict[str, Any]] = []
    hashes: dict[str, str] = {}
    loaded: dict[str, Any] = {}

    def check(name: str, passed: bool, detail: str) -> None:
        checks.append({"name": name, "passed": bool(passed), "detail": detail})
        if not passed:
            failures.append(f"{name}: {detail}")

    report: dict[str, Any] = {"author": __author__, "schema_version": 1, "passed": False,
        "scope": "actual_vulkan_native_observer_metadata_only", "directory": str(directory),
        "game_started_by_validator": False, "registry_modified_by_validator": False,
        "game_files_modified_by_validator": False, "gpu_frame_aligned": False,
        "main_view_verified": False, "native_camera_ready": False, "game_fg_ready": False,
        "native_fg_ready": False, "checks": checks, "input_sha256": hashes,
        "failures": failures}

    try:
        directory.mkdir(parents=True, exist_ok=True)
        integrity_before = load_json(directory / "run_integrity_before.json")
        integrity_after = load_json(directory / "run_integrity_after.json")
        check("binary_integrity", integrity_after.get("passed") is True and
              integrity_before.get("files") == integrity_after.get("files") and len(integrity_before.get("files", {})) >= 4,
              "listed binaries must have identical hashes before and after the owned game run")
        report["binary_integrity_scope"] = integrity_after.get("scope")
        for filename in ("run_integrity_before.json", "run_integrity_after.json", "layer_registration.json"):
            hashes[str(directory / filename)] = digest(directory / filename)
        registration = load_json(directory / "layer_registration.json")
        check("registration_restore_record", registration.get("registry_view") == 32 and registration.get("restored") is True,
              "temporary explicit layer registration must have been restored")
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Khronos\Vulkan\ExplicitLayers", 0,
                            winreg.KEY_QUERY_VALUE | winreg.KEY_WOW64_32KEY) as key:
            try:
                value, kind = winreg.QueryValueEx(key, registration["manifest"])
                current = {"exists": True, "data": value, "kind": kind}
            except FileNotFoundError:
                current = {"exists": False}
        check("registration_current_state", current == registration["previous"], "current 32-bit value differs from the saved prior state")
        for filename, expected in integrity_before["files"].items():
            original = Path(filename)
            if original.name.casefold() in {"d3d9.dll", "dxvk_d3d9.dll", "engine.dll"}:
                check("installed_binary_" + original.name, original.is_file() and digest(original) == expected["sha256"],
                      "one of the explicitly checked installed game binaries changed")
        launch_path = directory / "launch_status.json"
        if not launch_path.is_file():
            check("launch_status", False, f"missing {launch_path}")
        else:
            loaded["launch_status"] = load_json(launch_path)
            hashes[str(launch_path)] = digest(launch_path)
            launch = loaded["launch_status"]
            pid = launch.get("created_pid")
            check("recording_association_mode", launch.get("mode") == "native-input-game-probe" and
                launch.get("recording_association_probe_enabled") is True, "launcher must report native-input-game-probe recording-association mode")
            check("probe_options", launch.get("camera_probe_enabled") is False and launch.get("uniform_probe_enabled") is True and
                launch.get("pipeline_probe_enabled") is True and isinstance(launch.get("frame_generation_enabled"), bool),
                "expected camera=0, uniform=1, pipeline=1 and explicit boolean frame_generation_enabled")
            fg_requested = launch.get("frame_generation_enabled") is True
            report["frame_generation_requested_by_env"] = fg_requested
            check("created_pid", isinstance(pid, int) and not isinstance(pid, bool) and pid > 0 and launch.get("probe_pid") == pid,
                "created_pid and probe_pid must identify the same nonzero process")
            game_path = launch.get("game_executable")
            check("game_path", isinstance(game_path, str) and Path(game_path).is_absolute() and
                Path(game_path).name.casefold() == "left4dead2.exe", "launch_status must retain an absolute left4dead2.exe path")
            expected_status = directory / "native_input_status.json"
            check("probe_status_path", isinstance(launch.get("probe_status"), str) and
                normpath(launch["probe_status"]) == normpath(expected_status), "probe_status must resolve inside the selected experiment directory")
            environment = launch.get("child_environment", {})
            check("child_environment", isinstance(environment, dict) and
                environment.get("ASTERIA_NATIVE_INPUT_PROBE_OUTPUT") and
                normpath(environment["ASTERIA_NATIVE_INPUT_PROBE_OUTPUT"]) == normpath(expected_status) and
                environment.get("ASTERIA_NATIVE_UNIFORM_PROBE") == "1" and environment.get("ASTERIA_NATIVE_PIPELINE_PROBE") == "1" and
                environment.get("ASTERIA_NATIVE_ENGINE_CAMERA") == "0" and
                environment.get("ASTERIA_FG_ENABLE") == ("1" if fg_requested else "0"),
                "child output paths and probe switches do not match the requested recording-association mode")
            check("collection_complete", launch.get("collection_complete") is True,
                "the launcher must report that collection completed")
            report["launcher_exit_observation"] = {"game_exited": launch.get("game_exited"),
                "game_left_running": launch.get("game_left_running"),
                "normal_quit_requested": launch.get("quit_requested"),
                "exited_after_normal_quit": launch.get("exited_after_normal_quit")}
            check("path_guard", launch.get("probe_process_identity_matches") is True,
                "launcher did not record a matching PID and executable-path guard")

            latest_path = directory / "latest_observation.json"
            if latest_path.is_file():
                loaded["latest_observation"] = load_json(latest_path)
                hashes[str(latest_path)] = digest(latest_path)
                latest = loaded["latest_observation"]
                check("latest_observation_pid", latest.get("pid") == pid,
                    "latest observation PID differs from the created game PID")
                latest_devices = latest.get("devices")
                check("latest_observation_vulkan", isinstance(latest_devices, list) and bool(latest_devices) and
                    all(isinstance(item, dict) and item.get("api") == "Vulkan" for item in latest_devices),
                    "latest observation must contain devices whose API is Vulkan")
            else:
                check("latest_observation", False, f"missing {latest_path}")

            sidecar_paths: dict[str, Path] = {}
            listed = launch.get("auxiliary_collection_observations")
            check("sidecar_manifest", isinstance(listed, list), "launch_status must list archived collection sidecars")
            listed_paths = {normpath(item) for item in listed if isinstance(item, str)} if isinstance(listed, list) else set()
            for name, relative in SIDECARS.items():
                path = directory / relative
                sidecar_paths[name] = path
                exists = path.is_file()
                check(f"{name}_sidecar_exists", exists, f"missing {path}")
                check(f"{name}_sidecar_listed", normpath(path) in listed_paths,
                    f"launch_status auxiliary sidecar list does not include {path}")
                if exists:
                    loaded[name] = load_json(path)
                    hashes[str(path)] = digest(path)
                    check(f"{name}_pid", loaded[name].get("pid") == pid, "collection sidecar PID differs from created game PID")

            finalization_path = directory / "finalization_status.json"
            if finalization_path.is_file():
                loaded["finalization"] = load_json(finalization_path)
                hashes[str(finalization_path)] = digest(finalization_path)
                finalization = loaded["finalization"]
                check("finalization_pid", finalization.get("source_pid") == pid, "finalization source PID differs from created game PID")
                check("finalization_path", isinstance(game_path, str) and isinstance(finalization.get("expected_game_path"), str) and
                    normpath(finalization["expected_game_path"]) == normpath(game_path), "finalization expected executable path differs from launch_status")
                check("finalization_exit", finalization.get("confirmed_game_ended") is True, "finalizer did not confirm that the owned game ended")
                check("finalization_process_live", finalization.get("process_live") is False,
                    "finalization must report that the owned process is no longer live")
            else:
                check("finalization_status", False, f"missing {finalization_path}")

            cleanup_path = directory / "owned_process_cleanup.json"
            if cleanup_path.is_file():
                loaded["owned_process_cleanup"] = load_json(cleanup_path)
                hashes[str(cleanup_path)] = digest(cleanup_path)
                cleanup = loaded["owned_process_cleanup"]
                report["owned_process_cleanup"] = {"present": True, "method": cleanup.get("method"),
                    "requested_method": cleanup.get("requested_method"), "result": cleanup.get("result"),
                    "normal_quit_requested": cleanup.get("normal_quit_requested"),
                    "forced_termination": cleanup.get("forced_termination"), "raw": cleanup}
            else:
                report["owned_process_cleanup"] = {"present": False}

        readiness_seen: dict[str, list[str]] = {}
        for name, artifact in loaded.items():
            inspect_readiness(artifact, name, failures, readiness_seen)
        if loaded.get("launch_status", {}).get("frame_generation_enabled") is True:
            failures[:] = [item for item in failures if not item.startswith("launch_status.frame_generation_enabled must be false")]
        check("readiness_false", not any("must be false" in item or "must be zero" in item or "available camera" in item for item in failures),
            "all camera, main-view, GPU-frame and active frame-generation readiness indicators must remain false/zero")
        check("camera_probe_disabled", bool(loaded.get("launch_status", {}).get("camera_probe_enabled") is False),
            "engine camera probe must remain disabled")

        if isinstance(loaded.get("native_uniform"), dict):
            evidence_summary = inspect_native_evidence(loaded["native_uniform"], failures)
            check("native_recording_evidence", evidence_summary.get("evidence_devices", 0) > 0,
                "at least one unverified scene-epoch record must retain native command, generation, draw and attachment evidence")
            report["native_recording_evidence"] = evidence_summary
            viewport_sources: dict[str, int] = {}
            viewport_samples = 0
            pipeline_samples=0
            negative_height_samples = 0
            for sample in loaded["native_uniform"].get("samples", []):
                draw = sample.get("draw_recording", {})
                native = draw.get("native_viewport_observation", {})
                if draw.get("viewport_draw_state_verified") is True:
                    if valid_pipeline_viewport(draw.get("pipeline_viewport"),draw.get("native_device_generation"),
                            draw.get("native_command_allocation_generation"),draw.get("native_command_recording_generation"),
                            sample.get("vertex_pipeline_handle_u64"),draw.get("viewport")): pipeline_samples+=1
                    else: failures.append("retained draw claims pipeline viewport support without matched descriptor and pipeline identity")
                if not native.get("matched_descriptor_recording_generation"):
                    continue
                valid = (native.get("status") == 0 and native.get("source") in (1, 2, 3) and native.get("viewport_count") == 1
                         and native.get("set_sequence", 0) > 0 and native.get("device_generation", 0) > 0
                         and native.get("command_generation") == draw.get("native_command_allocation_generation")
                         and native.get("recording_generation") == draw.get("native_command_recording_generation")
                         and isinstance(draw.get("viewport_draw_state_verified"),bool) and native.get("observed_only") is True
                         and valid_viewport(draw.get("viewport")))
                if not valid:
                    failures.append("retained native viewport query disagrees with descriptor recording generations or qualification")
                    continue
                viewport_samples += 1
                key = {1: "legacy", 2: "core_with_count", 3: "ext_with_count"}[native["source"]]
                viewport_sources[key] = viewport_sources.get(key, 0) + 1
                negative_height_samples += draw["viewport"][3] < 0
            check("native_viewport_queries", viewport_samples > 0 or pipeline_samples>0,
                  "at least one actual native viewport query must match the same recorded descriptor generations")
            report["native_viewport_query_evidence"] = {"retained_samples_checked": viewport_samples,
                "sources": viewport_sources, "negative_height_samples": negative_height_samples,
                "pipeline_viewport_recording_state_supported_samples":pipeline_samples,
                "pipeline_dynamic_state_verified": False, "gpu_execution_verified": False}
            report["native_uniform_coverage"] = {
                "outcomes": loaded["native_uniform"].get("outcomes"),
                "sampler_devices": [{"device_owner_u64": item.get("device_owner_u64"),
                    "observer_frame": item.get("observer_frame"), "frame_budget_skips": item.get("frame_budget_skips"),
                    "limit_hits": item.get("limit_hits"), "sample_attempts": item.get("sample_attempts")}
                    for item in loaded["native_uniform"].get("devices", []) if isinstance(item, dict)],
            }
        else:
            report["native_recording_evidence"] = {"available": False, "devices": [], "evidence_devices": 0}

        if isinstance(loaded.get("provenance"), dict):
            report["provenance_coverage"] = loaded["provenance"].get("coverage")
        if isinstance(loaded.get("mapped_layer"), dict):
            mapped = loaded["mapped_layer"]
            check("mapped_layer_identity", mapped.get("layer") == "VK_LAYER_Asteria_FrameGeneration",
                "mapped-layer sidecar must identify the isolated Asteria frame-generation layer")
            check("mapped_layer_frame_generation_disabled", mapped.get("frame_generation") is False and
                not mapped.get("swapchain_adapter_active", False),
                "mapped-layer sidecar must explicitly report frame_generation=false and no active swapchain adapter")
            if loaded.get("launch_status", {}).get("frame_generation_enabled") is True:
                devices = mapped.get("devices", {})
                device_values = list(devices.values()) if isinstance(devices, dict) else devices
                swapchains = mapped.get("swapchains", {})
                swapchain_values = list(swapchains.values()) if isinstance(swapchains, dict) else swapchains
                check("mapped_layer_fg_device_prepared_without_provider", bool(device_values) and all(
                    isinstance(item, dict) and item.get("frame_generation_requested") is True and
                    item.get("frame_input_provider_available") is False and
                    item.get("frame_generation_queues_available") is True and
                    item.get("native_frame_inputs_bound") is False for item in device_values),
                    "explicit FG request in game must prepare FG device queues while provider remains unavailable")
                check("mapped_layer_fg_swapchain_fallback_without_provider", bool(swapchain_values) and all(
                    isinstance(item, dict) and item.get("frame_input_provider_available") is False and
                    item.get("frame_input_status") == "valid_frame_input_provider_unavailable" and
                    item.get("frame_generation_active") is False and
                    not item.get("swapchain_adapter_active", False) for item in swapchain_values) and
                    mapped.get("last_fg_create_swapchain_result") == -8,
                    "explicit FG request in game without provider must fall back to native swapchain with VK_ERROR_FEATURE_NOT_PRESENT")
            report["mapped_layer_observations"] = {"mode": mapped.get("mode"), "layer": mapped.get("layer"),
                "calls": mapped.get("calls"), "instances": mapped.get("instances"), "devices": mapped.get("devices"),
                "queues": mapped.get("queues"), "swapchains": mapped.get("swapchains"),
                "frame_generation": mapped.get("frame_generation"),
                "swapchain_adapter_active": mapped.get("swapchain_adapter_active", False),
                "last_fg_create_swapchain_result": mapped.get("last_fg_create_swapchain_result")}

        report["readiness_flags_observed"] = readiness_seen
        report["input_sha256"] = hashes
        for filename, expected in list(hashes.items()):
            check("artifact_stability", digest(Path(filename)) == expected, f"input changed during verification: {filename}")
        report["checks"] = checks
        report["failures"] = failures
        report["passed"] = not failures and all(item["passed"] for item in checks)
        report["result_semantics"] = "Evidence consistency for Vulkan native command-recording observer metadata only; not complete coverage, main-view identity, GPU execution, game frame alignment, motion validity or frame generation."
    except Exception as error:
        failures.append(f"validator_error: {type(error).__name__}: {error}")
        report["error"] = f"{type(error).__name__}: {error}"
        report["input_sha256"] = hashes
        report["checks"] = checks
        report["failures"] = failures
        report["passed"] = False

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_name(output_path.name + ".tmp")
    temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(output_path)
    print(json.dumps({"passed": report["passed"], "scope": report["scope"], "validation": str(output_path),
        "checks": len(checks), "failures": len(failures)}, ensure_ascii=False))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
