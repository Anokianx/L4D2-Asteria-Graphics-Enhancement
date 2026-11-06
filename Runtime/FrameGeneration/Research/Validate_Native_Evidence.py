import argparse
import hashlib
import json
import math
from pathlib import Path

AUTHOR = "Anokianx"
RESEARCH = Path(__file__).resolve().parent
ROOT = RESEARCH.parent
PINNED_ENGINE = "8a6d468059394bf4cfce32f11dcba12ef45de88da1373084d5a2916d0bf7ba86"


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def not_ready(report):
    for key in ("native_fg_ready", "motion_vectors_verified", "native_motion_vectors_verified",
                "native_camera_ready", "camera_semantics_verified", "frame_timing_verified",
                "native_camera_verified", "native_camera_semantics_verified"):
        if key in report:
            require(report[key] is False, "Unexpected readiness: " + key)


def zeros(values, description):
    require(all(value == 0 for value in values.values()), description)


def multiply(a, b):
    return [[sum(a[row][k] * b[k][col] for k in range(4)) for col in range(4)] for row in range(4)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=RESEARCH / "native-evidence-validation.json")
    args = parser.parse_args()
    output = args.output.resolve()
    require(output.is_relative_to(ROOT), "Output must stay in the experimental FrameGeneration tree")
    hashes = {}

    def evidence(relative):
        path = ROOT / relative
        hashes[relative] = sha(path)
        return read(path)

    abi = evidence("Research/NativeBinary/retail-camera-abi.json")
    engine = Path(abi["engine"]["path"])
    require(sha(engine) == PINNED_ENGINE == abi["engine"]["sha256"], "Retail engine SHA mismatch")
    require(abi["engine"]["source_hash_unchanged_after_analysis"] is True, "Offline engine changed")
    require(abi["engine_not_loaded_or_executed"] is True and abi["remote_process_memory_not_read"] is True,
            "Offline ABI analysis scope changed")
    profile = abi["retail_profile"]
    require(profile["extra_method"] == {"slot": 29, "name": "HideLoadingPlaque"}, "Retail extra slot mismatch")
    require((profile["world_to_screen_slot"], profile["world_to_view_slot"]) == (37, 38), "Retail matrix slots mismatch")
    require(abi["sdk_reference"]["retail_layout_matches_sdk"] is False, "SDK ABI mismatch must remain explicit")
    not_ready(profile)

    findings = evidence("Research/NativeShaders/native_shader_findings.json")
    inventory = evidence("Research/NativeShaders/native_shader_inventory.json")
    require(findings["inventory_sha256"] == hashes["Research/NativeShaders/native_shader_inventory.json"], "Inventory hash chain mismatch")
    summary = inventory["summary"]
    expected = {"files": 341, "files_complete": 341, "files_error": 0, "files_vcs4_without_ctab": 2,
                "compressed_blocks": 41402, "unique_compiled_combinations": 188679, "ctab_tables": 188676}
    require(all(summary[key] == value for key, value in expected.items()), "Native shader inventory changed")
    require(len(inventory["files"]) == 341 and len(inventory["unique_constant_names"]) == 468, "Shader coverage mismatch")
    game = Path(inventory["game"])
    for record in inventory["files"]:
        path = (game / record["path"]).resolve()
        require(path.is_relative_to(game.resolve()), "Shader path escapes installation")
        require(path.stat().st_size == record["bytes"] and sha(path) == record["sha256"], "Native shader hash mismatch: " + record["path"])
    not_ready(findings)
    not_ready(inventory)
    require(inventory["keyword_name_hits"]["history"] == [], "History-name observation changed")
    shader_test = evidence("Research/NativeShaders/native_shader_validation.json")
    require(shader_test["passed"] is True and shader_test["source_files_hash_unchanged"] == 341, "VCS validation failed")
    require(all(shader_test["malformed_inputs_rejected"].values()), "Malformed VCS acceptance")

    reflection = evidence("Research/NativeShaders/spirv-input-reflection.json")
    profiles = evidence("Research/NativeShaders/native-uniform-profiles.json")
    layout = evidence("Research/NativeShaders/dxvk-constant-layout.json")
    require(reflection["summary"] == {"reflected": 255}, "Captured shader reflection count changed")
    reflected = {record["sha256"]: record for record in reflection["records"]}
    for record in reflection["records"]:
        path = Path(record["file"])
        require(path.resolve().is_relative_to(ROOT), "Captured shader escapes experiment")
        require(path.stat().st_size == record["bytes"] and sha(path) == record["sha256"], "Captured shader hash mismatch")
    require(len(profiles["profiles"]) == 26, "Native VS whitelist count mismatch")
    require(profiles["reflection_sha256"] == hashes["Research/NativeShaders/spirv-input-reflection.json"], "Reflection hash chain mismatch")
    require(layout["actual_native_shader_profiles"]["sha256"] == hashes["Research/NativeShaders/native-uniform-profiles.json"], "Layout profile hash chain mismatch")
    require(profiles["source_dxvk_commit"] == "037d0fa1ad093065b18dad968d5898cbcd143e0e", "DXVK source version mismatch")
    file_hashes = {record["path"]: record["sha256"] for record in inventory["files"]}
    for item in profiles["profiles"]:
        require(item["shader_sha256"] in reflected, "Profile has no captured shader")
        require(sha(Path(item["spirv_file"])) == item["shader_sha256"], "Profile shader hash mismatch")
        require((item["stage"], item["descriptor_set"], item["binding"], item["relative_byte_offset"], item["byte_length"])
                == ("vertex", 2, 0, 384, 64), "Native VS constant layout mismatch")
        require(item["evidence"]["float_member_offset"] == 256 and item["evidence"]["array_stride"] == 16,
                "Float constant packing mismatch")
        require(item["evidence"]["direct_opdot_usage"] is True, "Native transform usage not established")
        require(item["native_vcs_exact_origins"], "Missing native shader source identity")
        for origin in item["native_vcs_exact_origins"]:
            require(file_hashes.get(origin["file"]) == origin["file_sha256"], "Native source hash mismatch")
            require(origin["c_viewproj_ctab"], "Missing native matrix CTAB")
            require(all((constant["name"], constant["register_set"], constant["register_index"], constant["register_count"],
                         constant["rows"], constant["columns"]) == ("cViewProj", "float4", 8, 4, 4, 4)
                        for constant in origin["c_viewproj_ctab"]), "Native matrix CTAB mismatch")
        not_ready(item)
    for item in (profiles, reflection, layout):
        not_ready(item)
    require(profiles["main_view_and_frame_semantics_verified"] is False, "Main-view semantics unproven")
    require(layout["observed_dxvk"]["release_binary_equivalence_verified"] is False,
            "Upstream DXVK layout must not imply installed binary equivalence")
    require(layout["ordinary_descriptor_claim_scope"].startswith("Matching upstream"), "Descriptor claim scope changed")
    reflector_test = evidence("Research/NativeShaders/spirv-reflector-fixture-validation.json")
    require(reflector_test["passed"] is True and reflector_test["actual_game_profiles_revalidated"] == 26, "SPIR-V profile test failed")
    require(all(reflector_test["malformed_inputs_rejected"].values()), "Malformed SPIR-V acceptance")

    mapped = evidence("Layer/Tests/results/mapped_buffer_contract.json")
    require(mapped["success"] is True and mapped["checks"] >= 255 and mapped["nondispatch_handle_bits"] == 64,
            "Mapped buffer ABI contract failed")
    require(mapped["max_copy_bytes"] == 4096 and mapped["explicit_mapping_for_copy"] is False
            and mapped["active_memory_tracking"] is False, "Mapped buffer copy scope mismatch")
    gpu = evidence("Layer/Tests/results/gpu_passthrough.json")
    require(gpu["success"] is True and gpu["saved_next_dispatch_verified"] is True
            and gpu["presented_real_frames"] == 60 and gpu["swapchain_creations"] == 2, "Layer GPU passthrough failed")
    require(gpu["frame_generation_active"] is False and gpu["generated_frames"] == 0, "Passthrough is not FG")

    host = evidence("InputProbe/results/HostPipeline/provenance_validation.json")
    require(host["passed"] is True and host["effects_excluded"] is True and host["disabled_case_passed"] is True,
            "Controlled pipeline provenance validation failed")
    not_ready(host)
    host_data = evidence("InputProbe/results/HostPipeline/Provenance/status.json")
    require(host_data["session_active"] is False and host_data["resources_are_live"] is False, "Host resources not historical")
    require(len(host_data["shaders"]) == 2 and len(host_data["associations"]) == 2, "Unexpected host shader associations")
    zeros(host_data["coverage"]["limits_hit"], "Host provenance tracking limits")
    require(host_data["coverage"]["binding_association_without_capture_losses"] is True, "Host provenance incomplete")
    for case in host["cases"]:
        path = ROOT / "InputProbe/results/HostPipeline" / case["bytecode"]
        require(sha(path) == case["sha256"] and case["recorded_draws"] > 100, "Host shader fixture mismatch")
    for shader in host_data["shaders"]:
        path = ROOT / "InputProbe/results/HostPipeline/Provenance" / shader["file"]
        require(sha(path) == shader["sha256"] and path.stat().st_size == shader["bytes"], "Host captured bytecode mismatch")

    game_snapshot = evidence("InputProbe/results/GameNativeProvenance/latest_observation.json")
    launch = evidence("InputProbe/results/GameNativeProvenance/launch_status.json")
    final = evidence("InputProbe/results/GameNativeProvenance/final_observation.json")
    finalization = evidence("InputProbe/results/GameNativeProvenance/finalization_status.json")
    require(launch["collection_complete"] is True and launch["render_observation_live"] is True
            and launch["created_pid"] == game_snapshot["pid"], "Game collection provenance mismatch")
    require(launch["game_files_modified"] is False and launch["layer_registration_changed"] is False, "Game launcher mutation")
    not_ready(game_snapshot)
    require(game_snapshot["device_limit_hits"] == 0, "Game device limit hit")
    require(len(game_snapshot["devices"]) == 1, "Unexpected game device set")
    device = game_snapshot["devices"][0]
    require(device["frame_id_observed"] >= 6000 and device["total_draws"] >= 2000000
            and len(device["resources"]) >= 500 and device["frame_progress_fresh"] is True, "Game collection too small or stale")
    zeros(device["limit_reasons"], "Game resource tracking limit")
    require(device["tracking"]["binding_observation_without_tracking_losses"] is True, "Game resource tracking loss")
    camera = device["engine_camera"]
    require(camera["available"] is True and camera["diagnostics_only"] is True, "Raw native camera unavailable")
    require(camera["retail_profile"]["mapped_binary_layout_verified"] is True
            and camera["retail_profile"]["matrix_slots"] == [37, 38]
            and camera["retail_profile"]["material_context_getters_called"] is False, "Live retail ABI guard failed")
    require(camera["raw_engine_screen_size"] is None and camera["raw_view_angles"] is None, "Unverified material-context call")
    require(camera["matrix_pair_validation"]["stable_during_sample"] is True
            and camera["matrix_pair_validation"]["paired_copy_passes"] == 2, "Unstable native matrix pair")
    for matrix in camera["matrix_validation"].values():
        require(matrix["finite"] is True and matrix["full_rank"] is True, "Invalid native matrix")
    not_ready(camera)
    require(camera["engine_thread_safety_verified"] is False, "Engine thread contract remains unverified")
    projection = camera["native_projection_diagnostic"]
    require(projection["available"] is True and projection["derived_from_native_matrices"] is True, "Native projection recovery unavailable")
    require(6.9 < projection["near_plane"] < 7.1 and 4950 < projection["far_plane"] < 5050,
            "Native clipping diagnostic mismatch")
    require(camera["render_surface_size"] == [3840, 2160] and projection["render_surface_aspect_matches_projection"] is True,
            "Projection/render surface mismatch")
    require(math.isclose(projection["projection_aspect_ratio"], 16 / 9, abs_tol=0.0001), "Native aspect ratio mismatch")
    reconstructed = multiply(projection["projection"], camera["raw_world_to_view"])
    require(max(abs(reconstructed[row][col] - camera["raw_world_to_screen"][row][col])
                for row in range(4) for col in range(4)) < 0.01, "Native projection composition mismatch")
    require(final["session_active"] is False and final["resources_are_live"] is False
            and finalization["confirmed_game_ended"] is True and finalization["addon_uninit_verified"] is False,
            "Game exit/historical classification failed")
    provenance = evidence("InputProbe/results/GameNativeProvenance/Provenance/status.json")
    coverage = provenance["coverage"]
    zeros(coverage["limits_hit"], "Game pipeline provenance limit")
    require(coverage["binding_association_without_capture_losses"] is False
            and coverage["shader_payload_missing"] > 0 and coverage["unknown_draw_pipelines"] > 0,
            "Incomplete game pipeline coverage must stay explicit")
    require(len(provenance["shaders"]) == 255, "Captured game shader count mismatch")
    not_ready(provenance)

    result = {"author": AUTHOR, "date": "2026-10-15", "passed": True, "schema_version": 1,
              "retail_matrix_slots": [37, 38], "source_shader_hashes_verified": 341,
              "captured_shader_hashes_verified": 255, "native_uniform_profiles_verified": 26,
              "mapped_buffer_contract_checks": mapped["checks"], "layer_passthrough_frames": 60,
              "native_camera_raw_matrix_capture_passed": True,
              "game_observed_frames": device["frame_id_observed"], "game_recorded_draws": device["total_draws"],
              "native_projection_near": projection["near_plane"], "native_projection_far": projection["far_plane"],
              "game_pipeline_payload_missing": coverage["shader_payload_missing"],
              "game_pipeline_unknown_draws": coverage["unknown_draw_pipelines"],
              "camera_semantics_verified": False, "frame_timing_verified": False,
              "native_motion_vectors_verified": False, "native_fg_ready": False,
              "uniform_game_sampling_result_in_scope": False,
              "scope": "Completed offline ABI/shader, controlled host/bridge, and raw camera collection evidence; no UBO game verdict.",
              "evidence_sha256": hashes}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "evidence_sha256"}, indent=2))


if __name__ == "__main__":
    main()
