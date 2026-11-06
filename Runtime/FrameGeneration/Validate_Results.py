from pathlib import Path
import argparse
import ast
import hashlib
import json
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parents[2]


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def verify_recording_pipeline_diagnostics(directory=None):
    result = {"checked_this_run": False, "evidence_available": False,
              "reason": "recording_evidence_not_requested"}
    if directory is None:
        return result
    directory = Path(directory).resolve()
    collection = read(directory / "mapped_layer_collection_observation.json")
    if "graphics_pipeline_diagnostics" not in collection:
        result["reason"] = "archived_recording_has_no_pipeline_diagnostics"
        return result
    completed = subprocess.run([sys.executable, str(ROOT / "InputProbe/Validate_PipelineDiagnostics.py"),
                                "--directory", str(directory)], capture_output=True, text=True, encoding="utf-8")
    require(completed.returncode == 0, f"Game pipeline diagnostic join failed:\n{completed.stdout}\n{completed.stderr}")
    observed = read(directory / "pipeline_diagnostic_validation.json")
    require(observed["passed"] and observed["scope"] == "native_pipeline_diagnostic_join_observer_only"
            and not observed["game_started_by_validator"] and not observed["registry_modified_by_validator"]
            and not observed["gpu_frame_aligned"] and not observed["main_view_verified"] and not observed["game_fg_ready"],
            "Game pipeline diagnostic join overstated input qualification")
    native_submit = observed.get("native_submit_observer")
    if native_submit is not None:
        for name in ("collection", "final_historical"):
            evidence = native_submit.get(name)
            require(isinstance(evidence, dict) and evidence.get("checked_this_run") and evidence.get("available")
                    and evidence.get("passed") and not evidence.get("gpu_completion_verified")
                    and not evidence.get("gpu_frame_aligned") and not evidence.get("main_view_verified")
                    and not evidence.get("game_fg_ready"), f"Native submit observer {name} overstated completion")
    provider_registration = None
    provider_path = directory / "fg_provider_registration_validation.json"
    if provider_path.is_file():
        provider_registration = read(provider_path)
        require(provider_registration["passed"] and provider_registration["provider_registration_observed"]
                and provider_registration["proxy_swapchain_observed"] and not provider_registration["frame_inputs_submitted"]
                and not provider_registration["gpu_frame_aligned"] and not provider_registration["game_fg_ready"],
                "Game provider registration observer overstated frame generation")
    return {"checked_this_run": True, "evidence_available": True, "evidence": observed,
            "native_submit_observer_checked_this_run": native_submit is not None,
            "provider_registration_checked_this_run": provider_registration is not None,
            "provider_registration": provider_registration}


def verify_sources():
    manifest = read(ROOT / "Research/source-manifest.json")
    for item in manifest["files"]:
        path = ROOT / "Research/sdk-v1.1.4" / item["path"]
        require(hashlib.sha256(path.read_bytes()).hexdigest() == item["sha256"], f"Pinned AMD source changed: {item['path']}")
    l4d2_manifest = read(ROOT / "Research/l4d2-sdk-reference/source-manifest.json")
    require(len(l4d2_manifest["files"]) == 12, "L4D2 SDK reference coverage changed")
    for item in l4d2_manifest["files"]:
        path = ROOT / "Research/l4d2-sdk-reference" / item["path"]
        data = path.read_bytes()
        require(len(data) == item["bytes"] and hashlib.sha256(data).hexdigest() == item["sha256"], f"Pinned L4D2 reference changed: {item['path']}")
    viewport_reference = ROOT / "InputProbe/References/ViewportHooks"
    viewport_manifest = read(viewport_reference / "source-manifest.json")
    require(len(viewport_manifest["files"]) == 5, "Viewport hook reference coverage changed")
    for item in viewport_manifest["files"]:
        data = (viewport_reference / item["path"]).read_bytes()
        require(len(data) == item["bytes"] and hashlib.sha256(data).hexdigest() == item["sha256"], f"Pinned viewport reference changed: {item['path']}")
    shaders = read(ROOT / "Port/Shaders/generated-manifest.json")
    for item in shaders["bytecode"]:
        path = ROOT / "Port/Shaders/SPIRV" / Path(item["header"]).with_suffix(".spv")
        data = path.read_bytes()
        require(len(data) == item["bytes"] and hashlib.sha256(data).hexdigest() == item["sha256"], f"Shader hash mismatch: {path.name}")
        require(len(data) % 4 == 0 and struct.unpack_from("<I", data)[0] == 0x07230203, f"Invalid SPIR-V: {path.name}")
    return {"pinned_source_files": len(manifest["files"]), "l4d2_reference_files": len(l4d2_manifest["files"]), "viewport_reference_files": len(viewport_manifest["files"]),
            "l4d2_reference_commit": l4d2_manifest["commit"], "spirv_files": len(shaders["bytecode"]), "spirv_bytes": shaders["bytecode_bytes"]}


def verify_packaging():
    sys.path.insert(0, str(PROJECT))
    from PyInstaller.utils.hooks import collect_submodules

    tree = ast.parse((PROJECT / "Danyria.spec").read_text(encoding="utf-8-sig"))
    analysis = next(node for node in ast.walk(tree) if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "Analysis")
    datas = ast.literal_eval(next(argument.value for argument in analysis.keywords if argument.arg == "datas"))
    for source, destination in datas:
        source = Path(source).resolve()
        require(source.exists(), f"Missing packaged resource: {source}")
        require(not ROOT.is_relative_to(source), f"Spec includes prototype through {source}")
    build = (PROJECT / "Build.bat").read_text(encoding="utf-8-sig")
    require('--add-data "%~dp0Asteria;Asteria"' not in build, "Broad Asteria PyInstaller data tree")
    staging = (PROJECT / "Build_Secure_Nuitka_Fixed.bat").read_text(encoding="utf-8-sig")
    require('low == "framegeneration"' in staging, "Missing Nuitka prototype staging exclusion")
    modules = collect_submodules("Asteria")
    require(not any("framegeneration" in name.lower() for name in modules), "Experimental Python module collected")
    return {"production_packaging_excludes_prototype": True, "asteria_python_modules": modules, "spec_data_paths_verified": len(datas)}


def verify_lower_dispatch():
    lower = read(ROOT / "Port/results/lower-dispatch.json")
    gpu = read(ROOT / "Port/results/layer-routing/gpu-probe.json")
    layer = read(ROOT / "Port/results/layer-routing/layer-probe.json")
    require(lower["success"] and lower["process_bits"] == 32 and not lower["game_frame_generation"], "Lower-dispatch probe failed or mislabeled")
    simulated = lower["simulated"]
    require(simulated["devices"] == 2 and simulated["threaded_present_calls"] == 2000, "Multidevice dispatch coverage changed")
    require(all(simulated[name] for name in ("cascade_cleanup", "foreign_owner_rejected", "recursive_self_rejected", "unknown_owner_rejected")), "Dispatch ownership, recursion or cleanup rejection failed")
    require(simulated["upper_trap_calls"] == 0, "Simulated dispatch reentered the upper layer")
    native = lower["gpu"]
    require(native["saved_lower_present_calls"] == 1 and native["upper_trap_calls"] == 0, "Native lower Present count or upper-layer recursion failed")
    require(native["standard_present_success"] and native["strict_mode"] and native["tracked_command_buffer"], "Native strict lower-dispatch proof failed")
    reuse = lower["address_reuse"]
    require(reuse["native_callbacks"] == 4 and all(reuse[name] for name in ("free_command_preserved", "destroy_pool_preserved", "destroy_device_preserved", "destroy_instance_preserved", "old_owner_retired_before_native")), "Native destruction recycled an address and lost its new owner")
    require(gpu["success"] and gpu["layer_loaded"] and gpu["saved_next_dispatch_verified"], "Vulkan layer did not verify its saved next dispatch")
    require(gpu["process_bits"] == 32 and gpu["nondispatchable_handle_bits"] == 64, "Layer handle ABI changed")
    require(gpu["presented_real_frames"] == 60 and gpu["swapchain_creations"] == 2, "Layer presentation or resize coverage changed")
    require(gpu["graphics_queue_compute"] and gpu["subgroup_compute"] and gpu["queue_capacity"] > 0, "Layer GPU capabilities unavailable")
    require(not gpu["frame_generation_active"] and gpu["generated_frames"] == 0, "Passthrough layer mislabeled as game FG")
    require(layer["layer"] == "VK_LAYER_Asteria_FrameGeneration" and layer["mode"] == "layer-probe" and layer["passthrough"] and not layer["frame_generation"], "Layer result mode changed")
    calls = layer["calls"]
    expected = {"create_instance": 1, "destroy_instance": 1, "create_device": 1, "destroy_device": 1,
                "create_swapchain": 2, "destroy_swapchain": 2, "recreate_swapchain": 1,
                "allocate_command_buffers": 1, "destroy_command_pool": 1, "get_device_queue": 1,
                "acquire_next_image": 30, "acquire_next_image2": 30, "successful_acquires": 60,
                "queue_present": 60, "successful_present_calls": 60, "successful_presented_swapchains": 60}
    require(all(calls.get(name) == count for name, count in expected.items()), "Layer lifecycle or successful-call counts changed")
    require(all(layer[name] == 0 for name in ("last_acquire_result", "last_create_device_result", "last_create_instance_result", "last_create_swapchain_result", "last_present_result")), "Layer reported a Vulkan error")
    require(len(layer["instances"]) == 1 and len(layer["devices"]) == 1 and len(layer["swapchains"]) == 2, "Layer resource counts changed")
    require(all(not item["live"] for category in ("instances", "devices", "swapchains") for item in layer[category].values()), "Layer retained live native objects after teardown")
    require(all(item["saved_next_dispatch_registered"] for item in layer["instances"].values()), "Layer instance dispatch was not registered")
    require(all(item["saved_next_dispatch_registered"] and item["saved_next_dispatch_verified"] and item["dispatch_probe_result"] == 0 and not item["native_frame_inputs_bound"] for item in layer["devices"].values()), "Layer device dispatch or input-readiness result invalid")
    require(all(item["last_present_result"] == 0 and item["acquires"] == item["presents"] == 30 for item in layer["swapchains"].values()), "Per-swapchain presentation failed")
    require({(item["width"], item["height"]) for item in layer["swapchains"].values()} == {(640, 360), (512, 288)}, "Layer resize dimensions changed")
    require(native["device"] == gpu["gpu_name"] and all(item["dispatch_probe_gpu"] == gpu["gpu_name"] for item in layer["devices"].values()), "Dispatch probes used different GPUs")
    return {"lower_dispatch_probe": lower, "layer_routing_probe": {"gpu_probe": gpu, "layer_probe": layer},
            "gpu_probe": gpu, "layer_counts": calls, "prototype_lower_dispatch_verified": True,
            "layer_saved_next_dispatch_verified": True}


def verify_native_input_host():
    enabled_root = ROOT / "InputProbe/results/HostTest"
    disabled_root = ROOT / "InputProbe/results/HostTestDisabled"
    enabled = read(enabled_root / "validation.json")
    disabled = read(disabled_root / "validation.json")
    active = read(enabled_root / "active_status.json")
    final = read(enabled_root / "native_input_status.json")
    effects = read(enabled_root / "LocalAppData/Danyria/AsteriaRuntime/asteria_host_test_status.json")
    for result in (enabled, disabled):
        require(result["passed"] and result["process_bits"] == 32 and result["frames_presented"] > 0, "Native-input isolated host test failed")
        require(result["author"] == "Anokianx" and not result["native_fg_ready"], "Host test attribution or FG readiness incorrect")
    require(not enabled["probe_disabled"] and disabled["probe_disabled"], "Native-input enable/disable coverage missing")
    require(not (disabled_root / "native_input_status.json").exists(), "Disabled probe wrote a status file")
    require(enabled["active_observation"] == active, "Host active evidence differs from tested observation")
    require(active["enabled"] and active["read_only"] and active["session_active"] and active["resources_are_live"], "Active input observation lifecycle incorrect")
    require(final["enabled"] and final["read_only"] and not final["session_active"] and not final["resources_are_live"], "Input add-on cleanup lifecycle failed")
    require(active["pid"] == final["pid"] == effects["pid"], "Host evidence has inconsistent process identity")
    require(active["device_limit_hits"] == final["device_limit_hits"] == 0, "Input probe device limits truncated observation")
    require(not active["native_fg_ready"] and not active["motion_vectors_verified"] and not final["native_fg_ready"] and not final["motion_vectors_verified"], "Resource discovery incorrectly claimed native FG or motion verification")
    require(active["architecture_bits"] == final["architecture_bits"] == 32, "Input probe architecture is not x86")
    states = active["devices"]
    require(states and all(state["total_draws"] == state["frame_id_observed"] * 2 and state["last_frame_draws"] == 2 for state in states), "Input discovery counted effect draws as application work")
    require(all(state["limit_hits"] == 0 and state["tracking"]["binding_observation_without_tracking_losses"] for state in states), "Input binding tracking lost resources")
    require(all(state["engine_camera"]["status"] == "unavailable" and not state["engine_camera"]["available"] and not state["engine_camera"]["camera_semantics_verified"] for state in states), "Isolated host invented engine camera semantics")
    require(any(state["effect_scopes"] > 0 for state in states), "Effect exclusion scope was not exercised")
    resources = [resource for state in states for resource in state["resources"]]
    require(resources and all(resource["provenance"] == "application_event_outside_reshade_effects" and not resource["motion_candidate"]["verified"] for resource in resources), "Input provenance or motion semantics mislabeled")
    require(all(any(resource["roles"][role] for resource in resources) for role in ("render_target", "depth_view", "sampled_texture")), "Host fixture resource roles missing")
    candidates = [resource for resource in resources if resource["motion_candidate"]["format_only"]]
    require(len(candidates) == 1 and candidates[0]["native_handle_hex"] == enabled["known_fixture_handles"]["rg_texture"] and candidates[0]["format_name"] == "R16G16_FLOAT", "Format-only candidate was not the known non-motion fixture")
    require(effects["render_result"] == 0 and all(effects["effects_rendered"][name] for name in ("smaa", "cas", "bloom", "color", "tonemap", "vignette", "deband", "marker")), "Existing Asteria effects did not render alongside the input probe")
    return {"native_input_host_probe": {"enabled": enabled, "disabled": disabled, "final_observation": final},
            "native_input_host_discovery_verified": True, "native_input_host_disabled_verified": True,
            "native_input_host_cleanup_verified": True}


def verify_native_input_game():
    fixture = ROOT / "InputProbe/results/GameProbe"
    launch = read(fixture / "launch_status.json")
    active = read(fixture / "latest_observation.json")
    final = read(fixture / "final_observation.json")
    ended = read(fixture / "finalization_status.json")
    pid = launch["created_pid"]
    require(pid > 0 and pid == launch["probe_pid"] == active["pid"] == final["pid"] == ended["source_pid"], "Game capture process identity mismatch")
    require(launch["mode"] == "native-input-game-probe" and all(launch[name] for name in ("prepared", "started", "collection_complete", "probe_observed", "probe_process_identity_matches", "probe_heartbeat_fresh", "render_observation_live", "application_resources_observed", "probe_session_active", "probe_resources_are_live")), "Game capture did not observe fresh real rendering")
    require(not launch["game_files_modified"] and not launch["layer_registration_changed"], "Game probe modified installed files or layer registration")
    environment = launch["child_environment"]
    require(environment["ASTERIA_NATIVE_ENGINE_CAMERA"] == "0" and environment["ASTERIA_NATIVE_INPUT_PROBE"] == "1", "Game probe camera diagnostics were not disabled")
    require(environment["SteamAppId"] == environment["SteamGameId"] == "550", "Game probe used an incorrect Steam identity")
    require(active["architecture_bits"] == final["architecture_bits"] == 32 and active["enabled"] and active["read_only"] and active["session_active"] and active["resources_are_live"], "Game active snapshot mode or lifecycle invalid")
    require(active["device_limit_hits"] == final["device_limit_hits"] == 0, "Game resource discovery hit a device limit")
    require(not active["native_fg_ready"] and not final["native_fg_ready"] and not active["motion_vectors_verified"] and not final["motion_vectors_verified"] and not launch["native_fg_ready"], "Game discovery incorrectly claimed native FG readiness")
    states = active["devices"]
    require(states and all(state["api"] == "Vulkan" for state in states), "Game probe did not observe Vulkan rendering")
    require(sum(state["frame_id_observed"] for state in states) >= 1000 and sum(state["total_draws"] for state in states) >= 100000, "Game observation duration or application work was insufficient")
    require(all(state["frame_progress_fresh"] and state["limit_hits"] == 0 and all(value == 0 for value in state["limit_reasons"].values()) for state in states), "Game rendering was stale or tracking reached a limit")
    require(all(state["tracking"]["binding_observation_without_tracking_losses"] and all(state["tracking"][name] == 0 for name in ("missing_table_bindings", "unknown_layout_table_binds", "unclassified_copy_sources")) for state in states), "Game descriptor observation lost tracked bindings")
    camera_flags = ("enabled", "available", "camera_semantics_verified", "engine_thread_safety_verified", "frame_timing_verified", "native_camera_ready", "native_motion_vectors_available")
    require(all(not state["engine_camera"][name] for state in states for name in camera_flags), "Disabled camera diagnostic claimed native engine semantics")
    require(all(not state["camera_callback"]["in_flight"] and state["camera_callback"]["samples_started"] == state["camera_callback"]["samples_completed"] for state in states), "Game capture stopped inside a camera callback")
    resources = [resource for state in states for resource in state["resources"]]
    require(len(resources) >= 100 and all(resource["provenance"] == "application_event_outside_reshade_effects" and not resource["motion_candidate"]["verified"] for resource in resources), "Game resource coverage or motion labeling invalid")
    depth = [{name: resource[name] for name in ("format_name", "width", "height", "samples")} for resource in resources if resource["roles"]["depth_view"]]
    require(any(item["format_name"] == "D24_UNORM_S8_UINT" and item["width"] == 3840 and item["height"] == 2160 and item["samples"] == 4 for item in depth), "Observed 4K MSAA depth descriptor missing")
    require(ended["confirmed_game_ended"] and not ended["process_live"] and not ended["telemetry_live"] and not ended["addon_uninit_verified"] and not ended["native_fg_ready"], "External game finalization identity or semantics invalid")
    require(ended["method"] == "external_process_identity_and_heartbeat_check" and Path(ended["expected_game_path"]) == Path(launch["game_executable"]), "Game finalization used a different process path")
    require(not final["session_active"] and not final["resources_are_live"] and final["finalization"]["resource_liveness"] == "historical" and final["finalization"]["confirmed_game_ended"] and not final["finalization"]["addon_uninit_verified"], "Game final snapshot mislabeled historical resources as live or add-on cleanup as verified")
    for original in states:
        subsequent = next((state for state in final["devices"] if state["native_handle_hex"] == original["native_handle_hex"]), None)
        require(subsequent is not None and subsequent["frame_id_observed"] >= original["frame_id_observed"] and subsequent["total_draws"] >= original["total_draws"], "Final game counters regressed or changed device identity")
        require(not subsequent["frame_progress_fresh"] and subsequent["limit_hits"] == 0 and all(value == 0 for value in subsequent["limit_reasons"].values()), "Final game freshness or tracking limits invalid")
        require(all(not subsequent["engine_camera"][name] for name in camera_flags), "Final game camera readiness changed")
    artifact = read(fixture / "artifact_integrity.json")
    prior_path = fixture / artifact["prior_validation_file"]
    require(hashlib.sha256(prior_path.read_bytes()).hexdigest() == artifact["prior_validation_sha256"], "Historical validation record changed")
    prior = read(prior_path)
    expected_artifact = prior["files"]["InputProbe/bin/AsteriaNativeInputProbe.addon"]
    private = fixture / artifact["private_probe_file"]
    require(artifact["matches_retained_prior_validation"] and artifact["capture_artifact"] == expected_artifact
            and len(private.read_bytes()) == expected_artifact["bytes"]
            and hashlib.sha256(private.read_bytes()).hexdigest() == expected_artifact["sha256"], "Historical game capture artifact mismatch")
    source, private = ROOT.parent / "Addons/Asteria.addon", fixture / "TestAddons/Asteria.addon"
    require(hashlib.sha256(source.read_bytes()).digest() == hashlib.sha256(private.read_bytes()).digest(), "Production add-on changed since resource-only capture")
    historical = read(fixture / "installed_dll_integrity.json")
    require(historical["scope"] == "Only the existing D3D9/DXVK DLL chain; not a whole-installation immutability claim", "Installed DLL comparison scope changed")
    require({Path(item["path"]).name.lower() for item in historical["files"]} == {"d3d9.dll", "dxvk_d3d9.dll"} and all(item["matches_prior_installed_inspection"] and item["sha256"] == historical["prior_installed_sha256"] for item in historical["files"]), "Historical D3D9/DXVK comparison failed")
    summary = {"pid": pid, "api": "Vulkan", "source": "InputProbe/results/GameProbe/latest_observation.json",
               "observed_frames": sum(state["frame_id_observed"] for state in states), "observed_draws": sum(state["total_draws"] for state in states),
               "observed_resources": len(resources), "observed_views": sum(len(state["views"]) for state in states),
               "binding_tracking_without_losses": True, "limit_hits": 0, "camera_diagnostics_enabled": False,
               "camera_semantics_verified": False, "native_fg_ready": False, "motion_vectors_verified": False,
               "float_rg_format_candidates": sum(bool(resource["motion_candidate"]["format_only"]) for resource in resources),
               "candidate_search_scope": "Observed application texture resources and bound view formats outside ReShade effects; R16G16_FLOAT/R32G32_FLOAT are format-only candidates.",
               "motion_absence_inferred": False, "depth_descriptors": depth, "tracking": [state["tracking"] for state in states],
               "process_identity_matched": True, "render_observation_live_at_capture": True,
               "game_files_modified": False, "layer_registration_changed": False,
               "final_observed_frames": sum(state["frame_id_observed"] for state in final["devices"]),
               "final_observed_draws": sum(state["total_draws"] for state in final["devices"]),
               "confirmed_game_ended": True, "final_resource_liveness": "historical", "addon_uninit_verified": False,
               "external_finalization": ended, "installed_dll_historical_comparison": historical,
               "capture_artifact_validation": artifact, "capture_uses_current_rebuilt_probe": False}
    return {"native_input_game_probe": summary, "native_input_game_discovery_verified": True,
            "game_resource_only_probe_verified": True}


def validate(dxvk=None, recording_evidence=None):
    report = read(ROOT / "validation.json") if (ROOT / "validation.json").exists() else {}
    interpolation = read(ROOT / "Port/results/interpolation/interpolation-probe.json")
    presentation = read(ROOT / "Port/results/presentation/presentation-probe.json")
    handles = read(ROOT / "Port/results/handle-probe.json")
    shaders = read(ROOT / "Port/results/shader-probe.json")
    shim = read(ROOT / "Port/results/vulkan-shim-probe.json")
    require(interpolation["success"] and interpolation["gpu_context_created"] and interpolation["verified_intermediate_frames"] == 17, "GPU interpolation proof failed")
    require(all(sample["intermediate_position_verified"] for sample in interpolation["frames"][1:]), "A non-reset offline frame failed interpolation")
    require(presentation["success"] and presentation["complete_cleanup"] and presentation["verified_intermediate_frames"] == 17, "Actual presentation proof failed")
    require(presentation["presenter_cache_cleanup_verified"] and presentation["retired_proxy_rejection_verified"], "SDK cache/proxy cleanup failed")
    require(presentation["native_present_failed_calls"] == 0 and presentation["native_present_successful_calls"] == 42, "Native presents not successful")
    require(presentation["game_present_calls"] == 22 and presentation["scheduled_generated_presents"] == 20, "Presentation counts changed")
    require(presentation["fg_disable_enable_verified"] and presentation["resize_verified"], "Lifecycle scenarios failed")
    enabled = [sample for sample in presentation["frames"] if sample["fg_enabled"]]
    require(all(sample["intermediate_position_verified"] for sample in enabled if not sample["reset"]), "A non-reset presented image failed interpolation")
    require(sum(sample["reset"] for sample in enabled) == 3, "Reset-frame accounting changed")
    disabled = [sample for sample in presentation["frames"] if not sample["fg_enabled"]]
    require(len(disabled) == 2 and all(sample["callback_count"] == 0 and sample["native_successful_presents"] == 1 for sample in disabled), "FG disabled behavior failed")
    require(not interpolation["game_frame_generation"] and not presentation["game_frame_generation"], "Synthetic probe mislabeled as game FG")
    require(handles["high_bit_round_trip"] and handles["stale_device_type_rejection"] and handles["threaded_interning"], "Handle ABI proof failed")
    require(shaders["failed"] == 0 and shaders["permutations_tested"] == 408 and shaders["bindings_checked"] == 3728, "Shader reflection proof failed")
    require(shim["real_vulkan_device"] and shim["non_dispatchable_handle_bytes"] == 8, "Vulkan shim proof failed")
    require(interpolation["process_bits"] == presentation["process_bits"] == handles["process_bits"] == 32, "Probe architecture is not x86")
    report.pop("actual_frame_generation", None)
    report.update({"author": "Anokianx", "schema_version": 9, "verified_date": "2026-10-27", "game_frame_generation": False,
                   "prototype_gpu_interpolation_verified": True, "prototype_presentation_verified": True,
                   "cpu_probe": read(ROOT / "Research/cpu-probe-result.json"),
                   "handle_probe": handles, "shader_probe": shaders, "vulkan_shim_probe": shim,
                   "interpolation_probe": interpolation, "presentation_probe": presentation})
    report["source_integrity"] = verify_sources()
    report.update(verify_lower_dispatch())
    report.update(verify_native_input_host())
    report.update(verify_native_input_game())
    for script in ("Research/Validate_Native_Evidence.py", "Layer/Tests/Validate_Loader_Chain.py",
                   "InputProbe/Validate_Provenance_Host.py", "InputProbe/Validate_Uniform_Game.py",
                   "Research/ImageInterpolation/Verify_Evidence.py", "Validate_EstimatedInputs.py",
                   "SceneInputs/Validate_SceneInterpolation.py", "DepthCapture/Validate_Semantic_Live.py"):
        completed = subprocess.run([sys.executable, str(ROOT / script)], capture_output=True, text=True, encoding="utf-8")
        require(completed.returncode == 0, f"Native evidence check failed: {script}\n{completed.stdout}\n{completed.stderr}")
    report["native_input_evidence"] = read(ROOT / "Research/native-evidence-validation.json")
    report["system_loader_chain"] = read(ROOT / "Layer/Tests/results/loader_chain_validation.json")
    lifetime = read(ROOT / "InputProbe/results/NativeUniformLifetimeContract.json")
    require(lifetime["passed"], "Native uniform callback lifetime contract failed")
    report["native_uniform_lifetime"] = lifetime
    comparison = read(ROOT / "InputProbe/results/NativeUniformComparisonContract.json")
    require(comparison["passed"] and comparison["checks"] >= 26, "Native getter comparison contract failed")
    report["native_uniform_comparison_contract"] = {key: comparison[key] for key in ("author", "passed", "checks", "game_started")}
    descriptor = read(ROOT / "InputProbe/results/NativeUniformDescriptorContract.json")
    require(descriptor["passed"] and descriptor["checks"] >= 31 and not descriptor["unverified_offset_read_allowed"]
            and descriptor["native_device_and_command_full_width"] and descriptor["wrapper_address_differs_from_native_handle"]
            and descriptor["descriptor_stage_type_range_offset_validated"], "Native command and descriptor contract failed")
    report["native_uniform_descriptor_contract"] = descriptor
    viewport = read(ROOT / "Layer/Tests/results/viewport_evidence_contract.json")
    require(viewport["passed"] and viewport["checks"] >= 25 and viewport["cpu_mock_only"] and viewport["abi"]["size_bytes"] == 80
            and viewport["legacy_exact_next_forwarding_verified"] and viewport["core_with_count_exact_next_forwarding_verified"]
            and viewport["ext_with_count_exact_next_forwarding_verified"] and viewport["observed_only"]
            and not viewport["pipeline_dynamic_state_verified"] and not viewport["gpu_execution_verified"]
            and not viewport["game_started"] and not viewport["gpu_frame_aligned"], "Native viewport observation contract failed or overstated qualification")
    report["native_viewport_observation_contract"] = viewport
    graphics = read(ROOT / "Layer/Tests/results/graphics_viewport_state_contract.json")
    require(graphics["passed"] and graphics["checks"] >= 537 and graphics["cpu_mock_only"] and graphics["bounded_hash_maps"]
            and graphics["capacity_fail_closed"] and graphics["dynamic_scissor_forms_checked"] and graphics["dynamic_rendering_chain_checked"]
            and graphics["pipeline_library"] == "bounded_subset_transfer" and graphics["linked_contract_survives_library_retirement"]
            and graphics["pipeline_mutation_epoch_guard"] and graphics["unknown_and_overlapping_subsets_fail_closed"] and graphics["library_binding_rejected"]
            and graphics["vertex_input_library_ignores_rendering_state"] and graphics["pure_link_inherits_rendering_state"]
            and graphics["linked_render_subpass_view_mask_match_required"] and graphics["shader_identifier_stage_supported"]
            and graphics["shader_identifier_requires_fail_on_compile"] and graphics["identifier_link_requires_fail_on_compile"]
            and graphics["pipeline_epoch_guard_only_link_dependencies"]
            and not graphics["frame_generation"] and not graphics["gpu_frame_aligned"] and not graphics["mainview_certified"],
            "Graphics pipeline viewport contract failed or overstated qualification")
    draw_viewport = read(ROOT / "Layer/Tests/results/draw_viewport_evidence_contract.json")
    require(draw_viewport["passed"] and draw_viewport["checks"] >= 189 and draw_viewport["abi_bytes"] == 112 and draw_viewport["cpu_mock_only"]
            and draw_viewport["native_generations_shared_with_descriptor_tracker"] and draw_viewport["unsupported_shader_objects_rejected"]
            and draw_viewport["unsupported_pipeline_binding_remains_rejected"]
            and draw_viewport["native_creation_epoch_checked_without_locking_next"]
            and draw_viewport["partial_native_batches_preserve_indices_and_results"]
            and draw_viewport["diagnostic_abi_bytes"] == 144 and draw_viewport["poison_origin_distinct_from_current_creation"]
            and draw_viewport["historical_creation_identity_selected"] and draw_viewport["unrelated_monolithic_mutation_does_not_poison"]
            and draw_viewport["linked_dependency_reuse_rejected"] and draw_viewport["unknown_payloads_fail_closed"]
            and not draw_viewport["game_started"] and not draw_viewport["gpu_execution_verified"] and not draw_viewport["frame_generation_ready"],
            "Draw viewport C ABI contract failed or overstated qualification")
    report["graphics_pipeline_viewport_contract"] = graphics
    report["draw_viewport_c_abi_contract"] = draw_viewport
    native_submit = read(ROOT / "Layer/Tests/results/native_submit_observer_contract.json")
    require(native_submit["passed"] and native_submit["checks"] >= 546 and native_submit["cpu_mock_only"]
            and native_submit["raw_submit_observer_only"] and native_submit["queue_submit_forms_observed"]
            and native_submit["queue_and_device_idle_observed_without_completion_claim"]
            and native_submit["bounded_submit_history"] and native_submit["reentrant_return_serials_checked"]
            and native_submit["native_failure_retained"] and not native_submit["game_started"]
            and not native_submit["gpu_execution_verified"] and not native_submit["frame_generation_ready"],
            "Raw native submit observer contract failed or overstated completion")
    report["native_submit_observer_contract"] = native_submit
    diagnostic_shape = read(ROOT / "Layer/Tests/results/pipeline_diagnostic_shape_contract.json")
    require(diagnostic_shape["passed"] and diagnostic_shape["checks"] >= 58 and diagnostic_shape["cpu_mock_only"]
            and diagnostic_shape["bounded_arrays_and_chains"] and diagnostic_shape["inline_shader_and_identifier_forms_observed"]
            and diagnostic_shape["nested_depth_clip_observed"] and not diagnostic_shape["changes_qualification"]
            and not diagnostic_shape["gpu_execution_verified"] and not diagnostic_shape["game_started"],
            "Pipeline diagnostic shape contract failed or overstated qualification")
    diagnostic_cache = read(ROOT / "Layer/Tests/results/pipeline_diagnostic_cache_contract.json")
    require(diagnostic_cache["passed"] and diagnostic_cache["checks"] >= 252 and diagnostic_cache["cpu_mock_only"]
            and diagnostic_cache["missing_identity_never_uses_current_handle"] and diagnostic_cache["selected_records_are_immutable_copies"]
            and not diagnostic_cache["changes_qualification"] and not diagnostic_cache["gpu_execution_verified"] and not diagnostic_cache["game_started"],
            "Pipeline diagnostic cache contract failed or overstated qualification")
    report["pipeline_diagnostic_shape_contract"] = diagnostic_shape
    report["pipeline_diagnostic_cache_contract"] = diagnostic_cache
    metadata = read(ROOT / "InputProbe/results/PipelineViewportValidatorContract.json")
    require(metadata["passed"] and metadata["checks"] >= 47 and metadata["cpu_metadata_only"]
            and not metadata["game_started"] and not metadata["gpu_execution_verified"] and not metadata["frame_generation_ready"],
            "Pipeline viewport metadata validator contract failed")
    for relative, expected in metadata["artifacts"].items():
        require(hashlib.sha256((ROOT / "InputProbe" / relative).read_bytes()).hexdigest() == expected,
                "Pipeline viewport metadata contract source changed after execution")
    report["pipeline_viewport_metadata_contract"] = metadata
    diagnostic_join = read(ROOT / "InputProbe/results/PipelineDiagnosticJoinContract.json")
    require(diagnostic_join["passed"] and diagnostic_join["checks"] >= 47 and diagnostic_join["cpu_mock_only"]
            and diagnostic_join["scope"] == "pipeline_diagnostic_identity_join_contract_only"
            and not diagnostic_join["game_started"] and not diagnostic_join["gpu_execution_verified"]
            and not diagnostic_join["frame_generation_ready"], "Pipeline diagnostic identity join contract failed")
    for relative, expected in diagnostic_join["artifacts"].items():
        require(hashlib.sha256((ROOT / "InputProbe" / relative).read_bytes()).hexdigest() == expected,
                "Pipeline diagnostic identity join source changed after execution")
    report["pipeline_diagnostic_identity_join_contract"] = diagnostic_join
    coordinator = read(ROOT / "LiveInputs/results/producer-frame-coordinator-contract.json")
    require(coordinator["passed"] and coordinator["checks"] >= 2424 and coordinator["cpu_mock_only"]
            and coordinator["completed_capture_separate_from_fg_qualification"] and coordinator["runtime_close_keeps_inflight_owner"]
            and coordinator["consumer_native_submission_required_before_retirement"] and coordinator["callback_stage_not_gpu_proof"]
            and coordinator["failed_or_abandoned_consumer_retirement_requires_native_drain"]
            and coordinator["consumer_abort_retirement_requires_final_lease_release"]
            and coordinator["successful_consumer_submission_blocks_abort_retirement"]
            and coordinator["consumer_abort_after_runtime_close_supported"] and coordinator["consumer_abort_drain_not_normal_consumer_completion"]
            and coordinator["same_ticket_success_recorded_before_qualification"] and coordinator["foreign_queue_success_blocks_scope_abort_retirement"]
            and coordinator["distinct_successes_require_device_drain"] and coordinator["invalid_success_metadata_persists_as_hazard"]
            and coordinator["device_drain_requires_all_native_devices_and_serials_known"] and coordinator["device_drain_follows_latest_attempt_and_abort"]
            and coordinator["unconsumed_release_follows_capture_completion"] and coordinator["begin_present_rejects_invalidated_publication"]
            and coordinator["native_pre_submit_lifetime_handshake"] and coordinator["pending_native_dispatch_blocks_retirement"]
            and coordinator["native_return_requires_exact_dispatch_identity"] and coordinator["abandoned_dispatch_retains_owner"]
            and coordinator["dispatch_tokens_move_only_and_bounded"] and coordinator["native_dispatch_close_return_races_checked"]
            and coordinator["native_return_serial_excludes_earlier_idle_proofs"]
            and coordinator["strict_sync_metadata_optional_for_legacy_observations"]
            and coordinator["bounded_wait_stage_values_validated"] and coordinator["signal_semaphore_identity_set_validated"]
            and coordinator["submit_batch_identity_validated"] and coordinator["queue_family_and_ownership_epoch_validated"]
            and coordinator["completion_sync_identity_rechecked"]
            and not coordinator["native_gpu_submission_observed"] and not coordinator["actual_gpu_completion_verified"]
            and not coordinator["real_game_producer_connected"] and not coordinator["game_started"] and not coordinator["game_frame_generation"],
            "Producer coordination contract failed or overstated native evidence")
    report["producer_frame_coordination_contract"] = coordinator
    lifecycle = read(ROOT / "Layer/Tests/results/swapchain_lifecycle_contract.json")
    require(lifecycle["passed"] and lifecycle["checks"] >= 151 and not lifecycle["game_started"]
            and lifecycle["native_calls_outside_state_lock"] and lifecycle["old_proxy_retained_until_explicit_destroy"]
            and lifecycle["no_provider_initialization_rejected_before_gpu_allocation"]
            and lifecycle["wrong_frame_callback_rejected_before_gpu_commands"], "Swapchain recreation or input fallback contract failed")
    report["swapchain_lifecycle_contract"] = lifecycle
    inputs = read(ROOT / "Layer/Tests/results/frame_input_contract.json")
    require(inputs["passed"] and inputs["checks"] >= 130 and inputs["engine_and_estimated_motion"]
            and inputs["zero_motion_accepted"] and inputs["owner_lifetime_verified"]
            and inputs["frame_retirement"]["cpu_contract_only"] and not inputs["frame_retirement"]["gpu_completion_validated"]
            and not inputs["actual_gpu_inputs_validated"] and not inputs["game_started"], "Frame input contract failed or overstated qualification")
    report["frame_input_contract"] = inputs
    bridge_path = ROOT / "Layer/Tests/results/CrossDllProviderFallback/validation.json"
    bridge = read(bridge_path)
    require(bridge["passed"] and bridge["cross_dll_registry_and_lease_contract_verified"]
            and bridge["post_device_provider_registration_verified"]
            and bridge["successful_acquire_tickets"] == 60 and bridge["swapchain_generations"] == 2
            and bridge["generated_frames"] == 0 and not bridge["actual_gpu_input_lease_completion_verified"]
            and not bridge["game_frame_generation"] and not bridge["game_started"]
            and all(value["passed"] for value in bridge["injected_native_failures"].values()),
            "Cross-DLL input registry, acquire ticket, lease or error propagation failed")
    for relative, digest in bridge["hashes"].items():
        require(hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() == digest, f"Input bridge used a different artifact: {relative}")
    report["cross_dll_input_bridge"] = bridge
    features = read(ROOT / "Layer/Tests/results/device_feature_chain_contract.json")
    require(features["passed"] and features["checks"] >= 86 and features["native_create_callbacks"] == 4
            and features["caller_feature_values_and_chain_preserved"] and features["loader_link_advanced_only"]
            and features["unknown_chain_forwards_original_device_info"] and features["native_calls_outside_state_lock"]
            and not features["actual_gpu_device_created"] and not features["game_started"], "FG device setup mutated caller input")
    report["device_feature_chain_contract"] = features
    alternative = read(ROOT / "Research/ImageInterpolation/verification.json")
    require(alternative["status"] == "pass" and alternative["source_files_verified"] == 13
            and all(alternative["facts"].values()) and not alternative["game_frame_generation_qualified"], "Image interpolation source evidence failed")
    report["image_interpolation_research"] = alternative
    report["accepted_motion_input_sources"] = ["Engine", "Estimated"]
    report["game_frame_input_producer_implemented"] = False
    estimated = read(ROOT / "estimated-input-validation.json")
    require(estimated["passed"] and not estimated["game_frame_generation"]
            and not estimated["real_game_input_producer_available"] and not estimated["same_gpu_frame_association_validated"]
            and not estimated["hud_composition_validated"] and not estimated["native_fg_ready"], "Estimated input prototype evidence failed or overstated game readiness")
    report["estimated_input_gpu_prototype"] = estimated
    scene = read(ROOT / "SceneInputs/results/SceneInterpolation/validation.json")
    require(scene["passed"] and not scene["real_game_inputs"] and not scene["game_frame_generation"]
            and not scene["real_game_input_producer_available"] and not scene["main_view_verified"]
            and not scene["hud_composition_validated"] and scene["generated_frames_presented"] == 0,
            "Controlled GPU scene chain failed or overstated game readiness")
    require(scene["modules"]["continuous_gpu_scene"]["input_frames"] == 8
            and scene["modules"]["continuous_gpu_scene"]["verified_midpoints"] == 7,
            "Controlled GPU scene did not verify seven actual intermediate images")
    report["controlled_gpu_scene_input_chain"] = scene
    semantic = read(ROOT / "DepthCapture/results/semantic-live-validation.json")
    require(semantic["passed"] and not semantic["game_started"] and not semantic["native_fg_ready"],
            "Live ReShade semantic host failed or overstated game readiness")
    report["reshade_depth_semantic_live_host"] = semantic
    report["reshade_depth_semantic_vulkan_checked_this_run"] = bool(dxvk)
    report.pop("reshade_depth_semantic_vulkan_live_host", None)
    association = read(ROOT / "FrameAssociation/results/LiveAssociationHost/validation.json")
    require(association["passed"] and association["cpu_contract_checks"] >= 116 and association["present_ticket_contract_checks"] >= 818 and not association["game_started"]
            and not association["actual_vulkan_native_draw_association_validated"] and not association["gpu_frame_aligned"]
            and not association["main_view_verified"] and not association["native_camera_ready"] and not association["game_fg_ready"],
            "Live recording observer contract/host failed or overstated readiness")
    ticket_contract = read(ROOT / "FrameAssociation/results/present-ticket-contract.json")
    require(ticket_contract["passed"] and ticket_contract["checks"] >= 826 and ticket_contract["cpu_mock_contract_only"]
            and ticket_contract["bounded_ticket_history"] and ticket_contract["exact_owner_generation_image_serial_match"]
            and ticket_contract["stale_reuse_resize_foreign_duplicate_fail_closed"] and ticket_contract["legacy_no_ticket_path_preserved"]
            and ticket_contract["ticket_never_upgrades_gpu_or_game_readiness"] and not ticket_contract["gpu_frame_aligned"]
            and not ticket_contract["main_view_verified"] and not ticket_contract["game_fg_ready"],
            "Present ticket evidence contract failed or overstated readiness")
    for relative, digest in association["hashes"].items():
        require(hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() == digest, f"Live association used a different artifact: {relative}")
    report["live_recording_association_observer"] = association
    report["present_ticket_evidence_contract"] = ticket_contract
    report["recording_association_game_evidence_checked_this_run"] = bool(recording_evidence)
    report.pop("recording_association_game_observer_evidence", None)
    if recording_evidence:
        completed = subprocess.run([sys.executable, str(ROOT / "InputProbe/Validate_RecordingAssociation_Game.py"),
                                    "--directory", str(recording_evidence.resolve())],
                                   capture_output=True, text=True, encoding="utf-8")
        require(completed.returncode == 0, f"Game recording observer evidence failed:\n{completed.stdout}\n{completed.stderr}")
        observed = read(recording_evidence / "recording_association_validation.json")
        require(observed["passed"] and observed["scope"] == "actual_vulkan_native_observer_metadata_only"
                and not observed["gpu_frame_aligned"] and not observed["main_view_verified"]
                and not observed["native_camera_ready"] and not observed["game_fg_ready"],
                "Game observer evidence overstated input qualification")
        report["recording_association_game_observer_evidence"] = observed
    pipeline_diagnostics = verify_recording_pipeline_diagnostics(recording_evidence)
    report["pipeline_diagnostic_game_evidence_checked_this_run"] = pipeline_diagnostics["checked_this_run"]
    report["pipeline_diagnostic_game_observer"] = pipeline_diagnostics
    report["owned_reshade_gpu_snapshot_checked_this_run"] = bool(dxvk)
    report.pop("owned_reshade_gpu_snapshot", None)
    if dxvk:
        completed = subprocess.run([sys.executable, str(ROOT / "DepthCapture/Validate_Semantic_Live.py"), "--vulkan"],
                                   capture_output=True, text=True, encoding="utf-8")
        require(completed.returncode == 0, f"Vulkan semantic evidence failed:\n{completed.stdout}\n{completed.stderr}")
        vulkan = read(ROOT / "DepthCapture/results/semantic-vulkan-live-validation.json")
        require(vulkan["passed"] and vulkan["dxvk_vulkan"] and not vulkan["native_fg_ready"] and not vulkan["game_started"],
                "Vulkan semantic host failed or overstated readiness")
        fixture = ROOT / "DepthCapture/results/SemanticLiveVulkan"
        source = read(fixture / "validation.json")["dxvk_source"]
        require(Path(source).resolve() == Path(dxvk).resolve(), "Vulkan evidence belongs to a different requested DXVK source")
        require(hashlib.sha256(Path(source).read_bytes()).digest() == hashlib.sha256((fixture / "AsteriaFixtureDXVK.dll").read_bytes()).digest(),
                "The Vulkan host tested a different DXVK binary")
        report["reshade_depth_semantic_vulkan_live_host"] = vulkan
        completed = subprocess.run([sys.executable, str(ROOT / "LiveInputs/Validate_InputSnapshot.py")],
                                   capture_output=True, text=True, encoding="utf-8")
        require(completed.returncode == 0, f"Owned snapshot evidence failed:\n{completed.stdout}\n{completed.stderr}")
        snapshot = read(ROOT / "LiveInputs/results/input-snapshot-validation.json")
        require(snapshot["passed"] and snapshot["captures_independently_checked"] == 30 and not snapshot["game_started"]
                and not snapshot["game_frame_generation"] and not snapshot["main_view_verified"]
                and not snapshot["camera_association_verified"] and not snapshot["hud_absence_verified"]
                and not snapshot["asynchronous_resource_ring"] and not snapshot["real_game_input_producer_available"]
                and not snapshot["frame_input_provider_registered"], "Owned snapshot overstated game readiness")
        for suffix in ("", "NoDepth", "Disabled", "Invalidated", "Resize", "Unqualified"):
            fixture = ROOT / "LiveInputs/results" / ("InputSnapshotVulkan" + suffix)
            require(Path(read(fixture / "validation.json")["dxvk_source"]).resolve() == Path(dxvk).resolve(),
                    "Owned snapshot belongs to a different requested DXVK source")
        report["owned_reshade_gpu_snapshot"] = snapshot
    fallback = read(ROOT / "Layer/Tests/results/QualifiedInputFallback/validation.json")
    require(fallback["passed"] and fallback["fg_requested"] and not fallback["provider_available"]
            and fallback["native_frames"] == 60 and fallback["swapchain_creations"] == 2 and fallback["generated_frames"] == 0
            and fallback["fg_device_prepared_without_provider"] and fallback["application_device_request_recorded_unchanged"]
            and fallback["swapchain_flags_unchanged"] and fallback["fg_swapchain_rejected_without_provider"]
            and fallback["pnext_types_unchanged"] and fallback["all_resources_destroyed"]
            and fallback["native_submit_observer_only"] and fallback["native_submit_records_observed"] > 0
            and fallback["native_idle_records_observed"] > 0 and not fallback["gpu_completion_verified"]
            and not fallback["game_started"] and not fallback["registry_changed"], "Unqualified FG request changed native presentation")
    for relative, digest in fallback["hashes"].items():
        require(hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() == digest, f"Input fallback used a different artifact: {relative}")
    report["unqualified_input_fallback"] = fallback
    report["native_uniform_game_readback"] = read(ROOT / "InputProbe/results/GameNativeUniform/uniform_validation.json")
    report["native_input_session_outcome"] = read(ROOT / "InputProbe/results/GameNativeUniform/test_session_outcome.json")
    report["game_native_camera_ready"] = False
    report["game_native_motion_vectors_verified"] = False
    report["game_native_input_stability_verified"] = False
    report.update(verify_packaging())
    files = report.setdefault("files", {})
    for relative in ("Port/build/x86/AsteriaInterpolationProbe.exe", "Port/build/x86/AsteriaPresentationProbe.exe",
                     "Port/build/x86/AsteriaHandleProbe.exe", "Port/build/x86/AsteriaFGBackend.lib", "Port/build/x86/AsteriaFGPresenter.lib",
                     "Port/Shaders/build/x86/AsteriaFGShaders.lib", "Port/results/interpolation/generated.ppm",
                     "Port/results/presentation/640x360_generated.ppm", "Port/results/presentation/512x288_generated.ppm",
                     "Port/build/x86/AsteriaLowerDispatchProbe.exe", "Port/results/lower-dispatch.json",
                     "Layer/bin/AsteriaFrameGenerationLayer.dll", "Port/results/layer-routing/gpu-probe.json",
                     "Port/results/layer-routing/layer-probe.json", "InputProbe/results/HostTest/validation.json",
                     "InputProbe/results/HostTest/active_status.json", "InputProbe/results/HostTest/native_input_status.json",
                     "InputProbe/results/HostTestDisabled/validation.json", "InputProbe/bin/AsteriaNativeInputProbe.addon",
                     "InputProbe/results/GameProbe/launch_status.json", "InputProbe/results/GameProbe/latest_observation.json",
                     "InputProbe/results/GameProbe/final_observation.json", "InputProbe/results/GameProbe/finalization_status.json",
                     "InputProbe/results/GameProbe/installed_dll_integrity.json", "Research/l4d2-sdk-reference/source-manifest.json",
                     "InputProbe/results/GameProbe/artifact_integrity.json", "Research/native-evidence-validation.json",
                     "Layer/AsteriaFrameGenerationLayer.json", "Layer/Tests/results/loader_chain_validation.json",
                     "InputProbe/results/NativeUniformLifetimeContract.json", "InputProbe/results/NativeUniformComparisonContract.json",
                     "InputProbe/results/NativeUniformDescriptorContract.json", "Layer/Tests/results/swapchain_lifecycle_contract.json",
                     "Layer/Tests/results/frame_input_contract.json", "Layer/FrameGenerationInputs.h", "Layer/FrameGenerationRuntime.h",
                     "Layer/AsteriaFrameInputApi.h", "Layer/FrameInputBridge.h", "Layer/FrameInputExports.h",
                     "Layer/AsteriaViewportEvidenceApi.h", "Layer/DescriptorEvidenceTracker.h",
                     "Layer/AsteriaDrawViewportEvidenceApi.h", "Layer/GraphicsViewportState.h",
                     "Layer/AsteriaDrawViewportDiagnosticApi.h", "Layer/GraphicsPipelineDiagnosticShape.h", "Layer/PipelineDiagnosticCache.h",
                     "Layer/AsteriaFrameGenerationLayer.def",
                     "Layer/Tests/PipelineDiagnosticShapeContract.cpp", "Layer/Tests/Build_PipelineDiagnostic_Tests.bat",
                     "Layer/Tests/PipelineDiagnosticCacheContract.cpp", "Layer/Tests/Build_PipelineDiagnosticCache_Tests.bat",
                     "Layer/Tests/results/pipeline_diagnostic_shape_contract.json", "Layer/Tests/results/pipeline_diagnostic_cache_contract.json",
                     "Layer/Tests/NativeSubmitObserverContract.cpp", "Layer/Tests/Build_NativeSubmitObserver_Tests.bat",
                     "Layer/Tests/results/native_submit_observer_contract.json", "Layer/Tests/Run_InputFallback.py",
                     "Layer/Tests/GraphicsViewportStateContract.cpp", "Layer/Tests/Build_GraphicsViewport_Tests.bat",
                     "Layer/Tests/DrawViewportEvidenceContract.cpp", "Layer/Tests/Build_DrawViewport_Tests.bat",
                     "Layer/Tests/results/graphics_viewport_state_contract.json", "Layer/Tests/results/draw_viewport_evidence_contract.json",
                     "Layer/Tests/ViewportEvidenceContract.cpp", "Layer/Tests/Build_Viewport_Tests.bat",
                     "Layer/Tests/results/viewport_evidence_contract.json", "Layer/Tests/bin/AsteriaViewportEvidenceContract.exe",
                     "InputProbe/References/ViewportHooks/source-manifest.json",
                     "Layer/Tests/FrameInputBridgeContract.cpp", "Layer/Tests/FrameInputProducerFixture.cpp",
                     "Layer/Tests/FrameInputProducerFixture.h", "Layer/Tests/Run_InputBridge_Tests.py",
                     "Layer/Tests/results/frame_input_bridge_contract.json", "Layer/Tests/results/CrossDllProviderFallback/validation.json",
                     "Port/include/AsteriaSwapchainOwnership.h", "SceneInputs/CrossDllSceneSession.h",
                     "Layer/Tests/results/device_feature_chain_contract.json", "Layer/DeviceFeatureChain.h",
                     "Layer/AsteriaFrameGenerationLayer.cpp", "Research/ImageInterpolation/upstream-lock.json",
                     "Research/ImageInterpolation/verification.json",
                     "estimated-input-validation.json", "Validate_EstimatedInputs.py", "Run_EstimatedInput_Tests.py",
                     "SceneInputs/SceneInterpolationProbe.cpp", "SceneInputs/GpuScene.h",
                     "SceneInputs/Build_SceneInterpolation.bat", "SceneInputs/Build_SceneShaders.bat",
                     "SceneInputs/Validate_SceneInterpolation.py", "SceneInputs/README.md",
                     "SceneInputs/Shaders/Scene.vert", "SceneInputs/Shaders/Scene.frag",
                     "SceneInputs/bin/Scene.vert.spv", "SceneInputs/bin/Scene.frag.spv",
                     "SceneInputs/bin/AsteriaSceneInterpolationProbe.exe",
                     "SceneInputs/results/SceneInterpolation/scene_interpolation.json",
                     "SceneInputs/results/SceneInterpolation/validation.json",
                     "DepthCapture/Run_DepthCapture_Tests.py", "DepthCapture/Build_Semantic_Live.bat",
                     "DepthCapture/Run_Semantic_Live_Tests.py", "DepthCapture/Validate_Semantic_Live.py",
                     "DepthCapture/SemanticDepthHostTest.cpp", "DepthCapture/SemanticDepthLiveProbe.cpp",
                     "DepthCapture/bin/AsteriaDepthSemanticHost.exe", "DepthCapture/bin/AsteriaDepthSemanticLiveProbe.addon",
                     "DepthCapture/results/semantic-live-validation.json",
                     "FrameAssociation/ObservedFrameAssociation.h", "FrameAssociation/LiveObservedAssociation.h", "FrameAssociation/LiveAssociationContract.cpp",
                     "FrameAssociation/Run_LiveAssociation_Tests.py", "FrameAssociation/Build_Contract.bat", "FrameAssociation/Build_Live_Contract.bat",
                     "FrameAssociation/PresentTicketContract.cpp", "FrameAssociation/Build_PresentTicket_Contract.bat",
                     "FrameAssociation/bin/AsteriaPresentTicketContract.exe", "FrameAssociation/results/present-ticket-contract.json",
                     "FrameAssociation/results/live-association-contract.json", "FrameAssociation/results/LiveAssociationHost/validation.json",
                     "FrameAssociation/results/LiveAssociationHost/active_uniform_observation.json",
                     "InputProbe/NativeInputGameProbe.cpp", "InputProbe/NativeUniformSampler.h",
                     "InputProbe/Validate_RecordingAssociation_Game.py", "InputProbe/Prepare_Probe_Layer.py",
                     "InputProbe/Tests/PipelineViewportMetadataContract.py", "InputProbe/results/PipelineViewportValidatorContract.json",
                     "InputProbe/Validate_PipelineDiagnostics.py", "InputProbe/Tests/PipelineDiagnosticJoinContract.py",
                     "InputProbe/results/PipelineDiagnosticJoinContract.json",
                     "LiveInputs/ReShadeInputSnapshot.cpp", "LiveInputs/InputSnapshotHost.cpp", "LiveInputs/Shaders/AsteriaInputSnapshot.fx",
                     "LiveInputs/Build_InputSnapshot.bat", "LiveInputs/Run_InputSnapshot_Tests.py", "LiveInputs/Validate_InputSnapshot.py",
                     "LiveInputs/VulkanTextureInterop.h", "LiveInputs/ProducerFrameCoordinator.h", "LiveInputs/ProducerFrameCoordinatorContract.cpp",
                     "LiveInputs/Build_ProducerCoordinator_Tests.bat", "LiveInputs/results/producer-frame-coordinator-contract.json",
                     "MotionReconstruction/NativeViewportTransform.h",
                     "Layer/Tests/results/QualifiedInputFallback/validation.json",
                     "Layer/Tests/results/QualifiedInputFallback/gpu_probe.json",
                     "Layer/Tests/results/QualifiedInputFallback/layer-probe.json",
                     "InputProbe/results/GameNativeUniform/uniform_validation.json", "InputProbe/results/GameNativeUniform/launch_status.json",
                     "InputProbe/results/GameNativeUniform/latest_observation.json", "InputProbe/results/GameNativeUniform/finalization_status.json",
                     "InputProbe/results/GameNativeUniform/NativeUniform/collection_observation.json",
                     "InputProbe/results/GameNativeUniform/Provenance/collection_observation.json",
                     "InputProbe/results/GameNativeUniform/mapped_layer_collection_observation.json",
                     "InputProbe/results/GameNativeUniform/installed_binary_integrity.json",
                     "InputProbe/results/GameNativeUniform/test_session_outcome.json",
                     "Layer/Tests/results/Crash644/crash_comparison.json",
                     "Layer/Tests/results/GameShutdownControl/control_status.json"):
        data = (ROOT / relative).read_bytes()
        files[relative] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    output = ROOT / "validation.json"
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(output)
    print("Validated prototype GPU output, dispatch, source integrity, packaging isolation and native readback evidence; game camera/motion readiness and input-chain stability remain unverified.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxvk", type=Path)
    parser.add_argument("--recording-evidence", type=Path)
    options = parser.parse_args()
    validate(options.dxvk, options.recording_evidence)
