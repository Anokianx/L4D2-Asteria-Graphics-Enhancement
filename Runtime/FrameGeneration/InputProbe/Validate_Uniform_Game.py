import argparse
import hashlib
import json
import math
import struct
import winreg
from pathlib import Path

AUTHOR = "Anokianx"
PROBE = Path(__file__).resolve().parent
ROOT = PROBE.parent
FALSE_KEYS = ("native_camera_ready", "native_fg_ready", "camera_semantics_verified", "frame_timing_verified",
              "motion_vectors_verified", "native_motion_vectors_verified", "native_binding_offset_verified",
              "actual_descriptor_type_verified", "dynamic_offsets_observed", "native_frame_inputs_bound",
              "engine_thread_safety_verified")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def false_readiness(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if key in FALSE_KEYS:
                require(item is False, "Unexpected verified state: " + key)
            false_readiness(item)
    elif isinstance(value, list):
        for item in value:
            false_readiness(item)


def float_bytes(values):
    require(isinstance(values, list) and len(values) == 16, "Expected sixteen native float registers")
    require(all(type(item) in (int, float) and math.isfinite(item) for item in values), "Nonfinite raw register")
    packed = struct.pack("<16f", *values)
    unpacked = struct.unpack("<16f", packed)
    require(all(original == converted for original, converted in zip(values, unpacked)), "Registers not exact float32")
    require(len(packed) == 64, "Invalid matrix byte length")
    return packed, unpacked


def integer_handles(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if key.endswith("_hex") and isinstance(item, str):
                integer_key = key[:-4] + "_u64"
                if integer_key in value:
                    require(int(item, 16) == value[integer_key], "Full-width handle mismatch")
            integer_handles(item)
    elif isinstance(value, list):
        for item in value:
            integer_handles(item)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, default=PROBE / "results/GameNativeUniform")
    args = parser.parse_args()
    directory = args.directory.resolve()
    require(directory.is_relative_to(PROBE / "results"), "Result directory must stay in InputProbe/results")
    hashes = {}

    def evidence(relative, base=directory):
        path = base / relative
        hashes[str(path.relative_to(ROOT))] = sha(path)
        return read(path)

    launch = evidence("launch_status.json")
    require(launch.get("collection_complete") is True, "Collection has not completed; validate after the game exits")
    require(launch["uniform_probe_enabled"] is True and launch["camera_probe_enabled"] is True
            and launch["pipeline_probe_enabled"] is True, "Required diagnostic modes not enabled")
    require(launch["game_files_modified"] is False and launch["layer_registration_changed"] is False, "Launcher modified external state")
    require(launch["child_environment"]["SteamAppId"] == "550"
            and launch["child_environment"]["SteamGameId"] == "550", "Incorrect native Steam app identity")
    require(set(launch["child_environment"]["VK_INSTANCE_LAYERS"].split(";")) >=
            {"VK_LAYER_Asteria_ReShade", "VK_LAYER_Asteria_FrameGeneration"}, "Native uniform layer chain absent")
    pid = launch["created_pid"]
    native = evidence("latest_observation.json")
    uniform = evidence("NativeUniform/collection_observation.json")
    provenance = evidence("Provenance/collection_observation.json")
    mapped = evidence("mapped_layer_collection_observation.json")
    finalization = evidence("finalization_status.json")
    require(finalization.get("confirmed_game_ended") is True, "Owned game is still active; validate after confirmed exit")
    final = evidence("final_observation.json")
    for report in (native, uniform, provenance, mapped):
        require(report["pid"] == pid, "Sidecar PID differs from created game")
        false_readiness(report)
    sidecars = {Path(path).resolve() for path in launch["auxiliary_collection_observations"]}
    require({directory / "NativeUniform/collection_observation.json", directory / "Provenance/collection_observation.json",
             directory / "mapped_layer_collection_observation.json"}.issubset(sidecars), "Collection sidecar archive incomplete")
    require(final["pid"] == finalization["source_pid"] == pid
            and final["session_active"] is False and final["resources_are_live"] is False, "Historical final snapshot mismatch")
    require(finalization["confirmed_game_ended"] is True and finalization["process_live"] is False
            and finalization["telemetry_live"] is False and finalization["addon_uninit_verified"] is False,
            "Game exit not independently confirmed")
    false_readiness(final)
    final_sidecars = []
    for relative_path in ("NativeUniform/final_observation.json", "Provenance/final_observation.json", "mapped_layer_final_observation.json"):
        sidecar = evidence(relative_path)
        require(sidecar["pid"] == pid and sidecar["session_active"] is False and sidecar["resources_are_live"] is False,
                "Auxiliary final observation not historical")
        false_readiness(sidecar)
        final_sidecars.append(relative_path)
    require(finalization.get("exit_code") == 100, "Unexpected recorded process exit code")
    require(native["device_limit_hits"] == 0 and native["native_fg_ready"] is False, "Resource probe capacity/readiness changed")
    require(native["devices"] and native["devices"][0]["frame_id_observed"] >= 1000, "Insufficient game presentation observation")
    for device in native["devices"]:
        require(device["limit_hits"] == 0 and all(count == 0 for count in device["limit_reasons"].values()), "Resource capacity omissions")
    require(uniform["enabled"] is True and uniform["read_only"] is True and uniform["device_limit_hits"] == 0, "Uniform observation scope changed")
    require(uniform["sample_attempts"] > 1000 and uniform["samples"], "No sustained native buffer readback")
    limits = uniform["limits"]
    require(limits["sample_attempts_per_device_frame"] == 16 and limits["samples_retained"] == 1024
            and limits["observer_frames_retained"] == 64, "Uniform sampling limits changed")
    require(len(uniform["samples"]) <= 1024 and len(uniform["first_successful_samples"]) <= 16, "Uniform retention limit exceeded")
    require(uniform["sample_attempts"] == len(uniform["samples"]) + uniform["samples_evicted"], "Sample eviction accounting mismatch")
    for device in uniform["devices"]:
        require(device["limit_hits"] == 0 and device["missing_table_binds"] == 0 and device["unknown_copy_sources"] == 0
                and device["unknown_layout_binds"] == 0 and device["unknown_pipeline_binds"] == 0, "Uniform binding observation incomplete")
        require(device["commands_cached"] <= limits["commands_per_device"]
                and device["tables_cached"] <= limits["tables_per_device"], "Uniform cache capacity exceeded")
    profiles = evidence("Research/NativeShaders/native-uniform-profiles.json", ROOT)
    profile_map = {profile["shader_sha256"]: profile for profile in profiles["profiles"]}
    require(len(profile_map) == 26 and len(profiles["profiles"]) == 26, "Native profile whitelist changed")
    require(profiles["main_view_and_frame_semantics_verified"] is False, "Shader profiles are not a main-view contract")
    require(uniform["profile_manifest"]["verified_profiles"] == 26 and uniform["profile_manifest"]["rejected_profiles"] == 0
            and uniform["profile_manifest"]["profile_bytes"] == 733816, "Profile load not verified")
    require(Path(uniform["profile_manifest"]["source"]).resolve() == ROOT / "Research/NativeShaders/native-uniform-profiles.json",
            "Loaded profile manifest identity mismatch")
    for profile in profile_map.values():
        path = Path(profile["spirv_file"])
        require(path.resolve().is_relative_to(ROOT), "Profile file escapes experimental tree")
        require(sha(path) == profile["shader_sha256"], "Native profile bytecode changed")
        require((profile["stage"], profile["descriptor_set"], profile["binding"], profile["relative_byte_offset"], profile["byte_length"])
                == ("vertex", 2, 0, 384, 64), "Unsupported native register profile")
    for shader in provenance["shaders"]:
        path = directory / "Provenance" / shader["file"]
        require(path.resolve().is_relative_to(directory), "Captured shader path escapes test")
        require(path.stat().st_size == shader["bytes"] and sha(path) == shader["sha256"], "Captured game shader SHA mismatch")
    require(all(value == 0 for value in provenance["coverage"]["limits_hit"].values()), "Pipeline capture capacity omitted records")
    require(mapped["layer"] == "VK_LAYER_Asteria_FrameGeneration" and mapped["passthrough"] is True
            and mapped["frame_generation"] is False and mapped["calls"]["successful_present_calls"] >= 1000,
            "Actual game Vulkan layer is not active passthrough")

    comparison = uniform["presentation_comparison"]
    require(comparison["matrix_layout"] == "direct_row_major_no_transpose" and comparison["observer_interval_only"] is True,
            "Comparison must not infer transpose, main view or GPU frame")
    absolute = comparison["absolute_tolerance"]
    relative = comparison["relative_tolerance"]
    require(absolute == 0.0001 and relative == 0.00001, "Unexpected comparison tolerances")
    require(comparison["maximum_retained_presentations"] == 64 and len(comparison["presentations"]) <= 64, "Presentation retention exceeded")
    require(comparison["compared_samples"] == comparison["matched_samples"] + comparison["mismatched_samples"], "Comparison accounting mismatch")
    require(comparison["matched_samples"] > 0 and comparison["mismatched_samples"] > 0, "Expected distinct observed view matrices")
    presentations = {}
    for observation in comparison["presentations"]:
        key = (observation["device_owner_u64"], observation["draw_observer_frame"])
        require(key not in presentations, "Duplicate owner/presentation interval")
        require(observation["present_observer_frame"] == observation["draw_observer_frame"] + 1
                and observation["observer_interval_only"] is True, "Presentation interval mismatch")
        require(observation["sample_count"] == observation["compared_samples"] + observation["unavailable_samples"]
                and observation["compared_samples"] == observation["matched_samples"] + observation["mismatched_samples"],
                "Per-presentation comparison accounting mismatch")
        presentations[key] = observation
    for metric in ("matched_samples", "mismatched_samples", "unavailable_samples"):
        require(sum(profile[metric] for profile in comparison["profiles"]) == comparison[metric], "Per-profile comparison accounting mismatch")
    require(all(profile["shader_sha256"] in profile_map for profile in comparison["profiles"]), "Unknown compared shader profile")
    recomputed = {"matched": 0, "mismatched": 0, "unavailable": 0, "unretained_presentation": 0}
    raw_matrices = set()
    sample_ids = set()
    for sample in uniform["first_successful_samples"] + uniform["samples"]:
        if sample["sample_id"] in sample_ids:
            continue
        sample_ids.add(sample["sample_id"])
        require(sample["status"] == "copied_profile_candidate" and sample["copy_stable"] is True and sample["finite"] is True
                and sample["bridge_copy_first"] is True and sample["bridge_copy_second"] is True
                and sample["bridge_resolved"] is True and sample["spirv_binding_profile_verified"] is True,
                "Retained sample is not a stable native candidate")
        require(sample["profile_shader_sha256"] in profile_map, "Sample bypasses native whitelist")
        descriptor = sample["descriptor"]
        require(descriptor["actual_native_dynamic_offset_count"] is None and descriptor["profile_relative_byte_offset"] == 384
                and descriptor["parameter_index"] == 2 and descriptor["binding"] == 0 and descriptor["stage_bits"] & 1,
                "Unverified native binding interpretation changed")
        require(descriptor["absolute_buffer_byte_offset"] == descriptor["range_offset_bytes"] + 384
                and descriptor["range_size_bytes"] >= 448, "Profile offset not inside descriptor")
        require(descriptor["absolute_buffer_byte_offset"] + 64 <= sample["buffer"]["size_bytes"], "Copy outside buffer")
        dimensions = sample["last_present_dimensions"]
        require(dimensions["width"] == sample["bound_depth"]["width"] and dimensions["height"] == sample["bound_depth"]["height"]
                and any(target["width"] == dimensions["width"] and target["height"] == dimensions["height"]
                        for target in sample["bound_targets"]), "Candidate dimensions do not match observed presentation")
        raw_bytes, raw = float_bytes(sample["raw_register_floats"])
        raw_matrices.add(raw_bytes)
        integer_handles(sample)
        if "native_getter_comparison" not in sample:
            continue
        reported = sample["native_getter_comparison"]
        observation = presentations.get((sample["device_owner_u64"], sample["observer_frame"]))
        if observation is None:
            recomputed["unretained_presentation"] += 1
            continue
        require(reported["draw_observer_frame"] == sample["observer_frame"]
                and reported["present_observer_frame"] == observation["present_observer_frame"], "Sample interval association mismatch")
        if not observation["native_getter"]["available"]:
            require(reported["status"] == "unavailable" and reported["max_absolute_error"] is None,
                    "Unavailable camera compared as valid")
            recomputed["unavailable"] += 1
            continue
        native_bytes, native_matrix = float_bytes(observation["native_getter"]["raw_world_to_screen"])
        differences = [abs(a - b) for a, b in zip(raw, native_matrix)]
        magnitudes = [max(abs(a), abs(b)) for a, b in zip(raw, native_matrix)]
        failures = sum(error > absolute + relative * magnitude for error, magnitude in zip(differences, magnitudes))
        relative_error = max(error / max(magnitude, 1e-12) for error, magnitude in zip(differences, magnitudes))
        within = failures == 0
        require(reported["status"] == ("matched" if within else "mismatched") and reported["within_tolerance"] is within
                and reported["bitwise_equal"] is (raw_bytes == native_bytes)
                and reported["components_outside_tolerance"] == failures, "Independent matrix comparison disagrees")
        require(math.isclose(reported["max_absolute_error"], max(differences), rel_tol=1e-12, abs_tol=1e-12)
                and math.isclose(reported["max_relative_error"], relative_error, rel_tol=1e-12, abs_tol=1e-12),
                "Independent error calculation disagrees")
        recomputed["matched" if within else "mismatched"] += 1
    require(recomputed["matched"] + recomputed["mismatched"] > 100 and len(raw_matrices) > 1,
            "Insufficient retained native matrix comparisons")

    lifetime = evidence("InputProbe/results/NativeUniformLifetimeContract.json", ROOT)
    require(lifetime["passed"] is True and lifetime["readers"] == 8 and lifetime["publish_retire_cycles"] == 1000
            and lifetime["callback_calls"] > 0 and lifetime["held_after_unpublish"] is True
            and lifetime["released_after_callbacks"] is True, "Sampler lifetime contract failed")
    contract = evidence("InputProbe/results/NativeUniformComparisonContract.json", ROOT)
    require(contract["passed"] is True and contract["checks"] >= 26 and contract["game_started"] is False,
            "Presentation comparison contract failed")
    false_readiness(contract)
    loader = evidence("Layer/Tests/results/loader_chain_validation.json", ROOT)
    manifest = ROOT / "Layer/AsteriaFrameGenerationLayer.json"
    require(loader["passed"] is True and loader["numeric_implementation_version_rejected"] is True
            and loader["manifest_sha256"] == sha(manifest), "System loader manifest validation failed")
    require(read(manifest)["layer"]["implementation_version"] == "1", "Vulkan loader requires string implementation_version")
    require(len(loader["actual_loader_chains"]) == 2 and all(chain["successful_native_presents"] == 60
            and chain["swapchain_creations"] == 2 and chain["cleaned_up"] is True
            for chain in loader["actual_loader_chains"]), "Actual Vulkan loader chains incomplete")
    integrity = evidence("installed_binary_integrity.json")
    game_path = Path(launch["game_executable"]).parent.resolve()
    pinned_chain = "942c5c314c39841a1257df6dd3a5048a84e115aa16b0c1fd4512254c53539af3"
    expected_files = {game_path / "d3d9.dll": pinned_chain, game_path / "dxvk_d3d9.dll": pinned_chain,
                      game_path / "bin/engine.dll": "8a6d468059394bf4cfce32f11dcba12ef45de88da1373084d5a2916d0bf7ba86"}
    require(len(integrity["files"]) == 3, "Binary integrity scope must stay exactly three DLLs")
    require({Path(item["path"]).resolve() for item in integrity["files"]} == set(expected_files), "Unexpected binary integrity scope")
    for item in integrity["files"]:
        path = Path(item["path"]).resolve()
        require(sha(path) == item["sha256"] == item["expected_sha256"] == expected_files[path]
                and item["matches_pinned_inspection"] is True, "Installed chain/engine DLL changed")
    registration = evidence("InputProbe/results/ProbeLayerRegistration/state.json", ROOT)
    require(registration["restored"] is True and registration["registry_view"] == 32
            and Path(registration["manifest"]).resolve() == manifest, "Temporary explicit-layer registration not restored")
    registry_path = r"SOFTWARE\Khronos\Vulkan\ExplicitLayers"
    current = {"exists": False}
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, registry_path, 0, winreg.KEY_READ | winreg.KEY_WOW64_32KEY) as key:
            try:
                data, kind = winreg.QueryValueEx(key, str(manifest))
                current = {"exists": True, "data": data, "kind": kind}
            except FileNotFoundError:
                pass
    except FileNotFoundError:
        pass
    require(current == registration["previous"], "Current explicit-layer entry differs from saved prior state")
    outcome = evidence("test_session_outcome.json")
    require(outcome["source_pid"] == pid and outcome["collection_completed"] is True
            and outcome["native_readback_evidence_verified"] is True and outcome["process_ended"] is True,
            "Session outcome source identity mismatch")
    require(outcome["exception_window_observed"] is True and outcome["exit_code_is_clean_shutdown_proof"] is False
            and outcome["game_stability_verified"] is False and outcome["clean_game_shutdown_verified"] is False
            and outcome["exception_diagnostic_pending"] is False and outcome["exception_root_cause_identified"] is False
            and outcome["shutdown_issue_resolved"] is False, "Session exception conclusions changed")
    require(len(outcome["evidence_files"]) == 3, "Session exception evidence set changed")
    exception_evidence = {}
    for relative_path, record in outcome["evidence_files"].items():
        path = (ROOT / relative_path).resolve()
        require(path.is_relative_to(ROOT / "Layer/Tests/results"), "Exception evidence escapes isolated test directory")
        require(sha(path) == record["sha256"], "Session exception evidence SHA mismatch")
        exception_evidence[relative_path] = evidence(relative_path, ROOT)
    crash = exception_evidence["Layer/Tests/results/Crash644/crash_comparison.json"]
    control = exception_evidence["Layer/Tests/results/GameShutdownControl/control_status.json"]
    diagnostic = exception_evidence["Layer/Tests/results/GameShutdownControl/Crash/diagnostic.json"]
    require(outcome["actual_exception_dumps"] == crash["actual_exception_dumps"], "Actual exception summary mismatch")
    actual = outcome["actual_exception_dumps"]
    require(actual["count"] == len(actual["files"]) == 2 and actual["exception_code"].lower() == "0xc0000005"
            and actual["faulting_module"].lower() == "vstdlib.dll" and actual["faulting_module_rva"].lower() == "0x75f0"
            and actual["original_offending_object_owner_identified"] is False, "Actual exception location/ownership claim mismatch")
    require(outcome["experimental_input_components_required_to_reproduce_shutdown_failure"] is False
            and crash["experimental_input_components_required_to_reproduce_shutdown_failure"] is False,
            "Experimental input components are not necessary for observed shutdown reproduction")
    control_summary = outcome["shutdown_control"]
    require(control_summary["process_id"] == control["created_pid"] == diagnostic["pid"], "Control evidence process mismatch")
    require(control["native_probe_module_absent"] is True and control["fg_layer_module_absent"] is True
            and control["asteria_addon_module_present"] is True and control["reshade_module_present"] is True,
            "Control module-presence evidence incomplete")
    names = [item["name"].lower() for item in control["modules"]]
    require(not any("asterianativeinputprobe" in name or "asteriaframegenerationlayer" in name for name in names),
            "Experimental input module loaded in control")
    require(all(control[key] is False for key in ("native_input_probe", "camera_probe", "pipeline_probe", "uniform_probe",
                                                 "frame_generation_layer", "exited_after_normal_quit"))
            and control["terminated_by_test"] is True and control["game_exited"] is True,
            "Control shutdown outcome mismatch")
    require(control_summary["crash_window_reproduced"] is True and control_summary["exact_exception_site_verified"] is False,
            "Control crash window does not identify its precise exception instruction")
    require(diagnostic["read_only_target"] is True and any("The game crashed." in child.get("text", "")
            for window in diagnostic["windows"] for child in window.get("children", [])), "Control exception UI not captured")
    loaded_addon = directory / "TestAddons/AsteriaNativeInputProbe.addon"
    artifact = evidence("capture_artifact_integrity.json")
    prior_path = directory / artifact["prior_validation_file"]
    require(sha(prior_path) == artifact["prior_validation_sha256"], "Original capture-validation hash changed")
    prior = read(prior_path)
    require(artifact["private_addon_file"] == "TestAddons/AsteriaNativeInputProbe.addon"
            and sha(loaded_addon) == artifact["loaded_addon_sha256"] == prior["loaded_addon_sha256"],
            "Loaded add-on differs from retained capture identity")
    result = {"author": AUTHOR, "date": "2026-10-15", "passed": True, "passed_scope": "native_readback_evidence_integrity_only",
              "schema_version": 1, "source_pid": pid,
              "collection_observed_frames": native["devices"][0]["frame_id_observed"],
              "collection_recorded_draws": native["devices"][0]["total_draws"],
              "uniform_collection_sample_attempts": uniform["sample_attempts"],
              "uniform_observer_frames": [device["observer_frame"] for device in uniform["devices"]],
              "sampling_coverage": {
                  "sample_attempts_per_device_frame": 16,
                  "uniform_scene_sampling_verified": False,
                  "complete_scene_or_view_coverage_verified": False,
                  "frame_budget_skips": sum(device["frame_budget_skips"] for device in uniform["devices"]),
                  "unverified_draw_profiles": sum(device["unverified_draw_profiles"] for device in uniform["devices"]),
                  "pipeline_shader_payload_missing": provenance["coverage"]["shader_payload_missing"],
                  "pipeline_unknown_draws": provenance["coverage"]["unknown_draw_pipelines"],
                  "semantics": "Ordered native-profile candidates capped at 16 attempts per device/observer frame; matches and mismatches cannot represent all scene views or rule out unobserved native motion inputs."},
              "retained_samples_verified": len(sample_ids), "distinct_retained_float32_matrices": len(raw_matrices),
              "independent_comparisons": recomputed, "native_profile_count": 26,
              "comparison_cumulative_matched": comparison["matched_samples"],
              "comparison_cumulative_mismatched": comparison["mismatched_samples"],
              "comparison_cumulative_unavailable": comparison["unavailable_samples"],
              "observer_interval_only": True, "native_camera_ready": False, "camera_semantics_verified": False,
              "frame_timing_verified": False, "motion_vectors_verified": False, "native_fg_ready": False,
              "exit_confirmed": True, "addon_uninit_verified": False,
              "installed_integrity_scope": "existing_d3d9_dxvk_chain_and_engine_only",
              "installed_dll_hashes_verified": 3, "temporary_registration_restored": True,
              "game_stability_verified": False, "clean_game_shutdown_verified": False,
              "exception_diagnostic_pending": False,
              "exception_root_cause_identified": False, "shutdown_issue_resolved": False,
              "session_exception_evidence_files_verified": 3,
              "actual_exception_location": "vstdlib.dll+0x75f0",
              "actual_exception_dump_count": 2,
              "control_input_addon_and_fg_layer_absent_verified": True,
              "control_shutdown_failure_reproduced": True,
              "control_exact_exception_site_verified": False,
              "finalized_auxiliary_streams": final_sidecars,
              "loaded_addon_sha256": sha(loaded_addon), "layer_binary_sha256": sha(ROOT / "Layer/bin/AsteriaFrameGenerationLayer.dll"),
              "capture_uses_current_probe_binary": sha(loaded_addon) == sha(PROBE / "bin/AsteriaNativeInputProbe.addon"),
              "capture_artifact_identity_source": "capture_artifact_integrity.json and retained original capture_validation.json",
              "scope": "Actual CPU-mapped native constant readback evidence only; session exception prevents a stability or clean-shutdown claim. Main-view, GPU-frame and motion semantics remain unverified.",
              "evidence_sha256": hashes}
    output = directory / "uniform_validation.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "evidence_sha256"}, indent=2))


if __name__ == "__main__":
    main()
