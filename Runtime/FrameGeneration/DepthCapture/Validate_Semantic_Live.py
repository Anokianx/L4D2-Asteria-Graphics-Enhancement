import argparse
import hashlib
import json
import math
from pathlib import Path


__author__ = "Anokianx"
ROOT = Path(__file__).resolve().parent


def validate(vulkan=False):
    checks = 0
    artifacts = {}

    def require(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            raise ValueError(message)

    def artifact(path):
        path = Path(path)
        data = path.read_bytes()
        artifacts[str(path.relative_to(ROOT)).replace("\\", "/")] = hashlib.sha256(data).hexdigest()
        return data

    for source, binary in (("SemanticDepthLiveProbe.cpp", "bin/AsteriaDepthSemanticLiveProbe.addon"),
                           ("ReShadeDepthSemanticBridge.h", "bin/AsteriaDepthSemanticLiveProbe.addon"),
                           ("SemanticDepthHostTest.cpp", "bin/AsteriaDepthSemanticHost.exe")):
        require((ROOT / binary).stat().st_mtime_ns >= (ROOT / source).stat().st_mtime_ns,
                f"Stale live semantic binary: {binary}")

    phase_observations = []
    pixels_checked = 0
    prefix = "SemanticLiveVulkan" if vulkan else "SemanticLive"
    for suffix, no_depth, disabled in (("", False, False), ("NoDepth", True, False), ("Disabled", False, True)):
        name = prefix + suffix
        fixture = ROOT / "results" / name
        summary = json.loads(artifact(fixture / "validation.json"))
        require(summary["passed"] is True and summary["timing_and_bindings_passed"] is True, f"{name}: host failed")
        require(summary["author"] == "Anokianx" and summary["process_bits"] == 32, f"{name}: author or ABI differs")
        require(summary["native_fg_ready"] is False and summary["main_view_confirmed"] is False and summary["game_started"] is False,
                f"{name}: isolated observation promoted game readiness")
        require(summary["no_depth_control"] == no_depth and summary["disabled_control"] == disabled, f"{name}: control mode differs")
        require(summary["dxvk_vulkan"] == vulkan and summary["layer_registration_changed"] is False and
                summary["game_files_modified"] is False, f"{name}: backend or isolation claims differ")
        frames = summary["rendered_depth_frames"]
        require(len(frames) == summary["frames_presented"] and len(frames) >= 100, f"{name}: host frame manifest incomplete")
        for index, frame in enumerate(frames, 1):
            alternate = (index // 10) % 2 != 0
            require(frame["present_id"] == index and
                    math.isclose(frame["left_device_depth"], .4 if alternate else .25, abs_tol=1e-7) and
                    math.isclose(frame["right_device_depth"], .8 if alternate else .65, abs_tol=1e-7) and
                    frame["background_device_depth"] == 1, f"{name}: depth render sequence changed")
        require(summary["input_observation_saved_before_teardown"] is True, f"{name}: no active InputProbe snapshot")
        input_status = json.loads(artifact(fixture / "active_input_status.json"))
        require(input_status["native_fg_ready"] is False and len(input_status["devices"]) == 1, f"{name}: InputProbe observation absent")
        require(input_status["devices"][0]["last_frame_draws"] == (13 if vulkan else 12) and input_status["devices"][0]["limit_hits"] == 0,
                f"{name}: application draw count or InputProbe tracking changed")
        require(input_status.get("session_active") is True and input_status.get("resources_are_live") is True,
                f"{name}: InputProbe evidence came from teardown")
        require(input_status["devices"][0]["api"] == ("Vulkan" if vulkan else "D3D9") and
                any(resource["roles"]["depth_view"] for resource in input_status["devices"][0]["resources"]) == (not no_depth),
                f"{name}: independent active depth attachment observation differs")
        if vulkan:
            artifact(fixture / "AsteriaFixtureDXVK.dll")
            artifact(fixture / "Layers/ReShade32.json")
        log = artifact(fixture / "ReShadeHost" / "ReShade.log").decode("utf-8", "replace")
        require("Successfully compiled '" in log and "AsteriaDepthSemanticBridge.fx' in" in log and "Failed to compile" not in log,
                f"{name}: real ReShade effect compile missing")
        require(artifact(fixture / "Effects" / "AsteriaDepthSemanticBridge.fx") == (ROOT / "Shaders/AsteriaDepthSemanticBridge.fx").read_bytes(),
                f"{name}: compiled effect differs from current source")
        require(artifact(fixture / "TestAddons" / "AsteriaDepthSemanticLiveProbe.addon") == (ROOT / "bin/AsteriaDepthSemanticLiveProbe.addon").read_bytes(),
                f"{name}: deployed diagnostic addon differs from current binary")
        artifact(fixture / "TestAddons" / "AsteriaNativeInputProbe.addon")
        if disabled:
            require(not (fixture / "semantic_status.json").exists(), "Disabled probe unexpectedly recorded semantic observations")
            continue
        status = json.loads(artifact(fixture / "active_semantic_status.json"))
        require(status["backend"] == (131072 if vulkan else 36864), f"{name}: actual semantic backend differs")
        require(status == summary["observation"], f"{name}: summarized observation is stale")
        require(status["sequence_valid"] is True and status["begin_effects"] == status["finish_effects"] and
                0 <= len(frames) - status["present_callbacks"] <= 2 and
                summary["submitted_present_callback_lag"] == len(frames) - status["present_callbacks"],
                f"{name}: begin/finish/present sequence mismatch")
        require(status["last_borrow_retired_after_callback"] is True and
                status["native_fg_ready"] is False and status["main_view_confirmed"] is False and
                status["gpu_completion_confirmed_for_depth"] is False and status["depth_resource_owned"] is False,
                f"{name}: borrow or readiness claims are unsafe")
        require(len(status["observations"]) >= 60, f"{name}: too few real callback observations")
        for index, observation in enumerate(status["observations"], 1):
            require(observation["frame_id"] == index and 0 < observation["present_id"] <= len(frames), f"{name}: callback identity differs")
            require(observation["conventions_explicitly_known"] is False and all(value is None for value in observation["definitions"].values()),
                    f"{name}: missing preprocessor conventions were guessed")
            if no_depth:
                require(observation["valid_during_callback"] is False and observation["generic_depth_ready"] is False,
                        "No-depth control exposed a valid binding")
            else:
                require(observation["valid_during_callback"] is True and observation["generic_depth_ready"] is True and
                        observation["status"] == "borrowed_generic_depth_main_view_unconfirmed" and
                        observation["width"] == 640 and observation["height"] == 360 and observation["samples"] == 1 and
                        int(observation["resource"], 16) != 0 and int(observation["view"], 16) != 0,
                        "Live single-sample DEPTH binding was not observed")
        if no_depth:
            require(status["ready_observations"] == 0 and not status["screenshots"], "No-depth control created depth evidence")
            continue
        require(status["ready_observations"] == status["begin_effects"] and len(status["screenshots"]) == 4,
                "Active fixture lacks complete ready observations or four screenshots")
        for image in status["screenshots"]:
            require(image["width"] == 640 and image["height"] == 360 and image["capture_screenshot_completed"] is True and
                    image["channel_order"] == "RGBA8", "Screenshot metadata differs")
            data = artifact(fixture / image["file"])
            require(len(data) == 640 * 360 * 4, "Depth semantic screenshot length differs")
            expected = frames[image["present_id"] - 1]
            phase_observations.append(round(expected["left_device_depth"], 2))
            for x0, x1, y0, y1, depth in ((55, 265, 170, 285, expected["left_device_depth"]),
                                          (375, 585, 170, 285, expected["right_device_depth"]),
                                          (290, 350, 310, 350, 1.0)):
                expected_byte = int(math.floor(depth * 255 + .5))
                for y in range(y0, y1):
                    for x in range(x0, x1):
                        offset = (y * 640 + x) * 4
                        require(all(abs(data[offset + channel] - expected_byte) <= 2 for channel in range(3)),
                                "GPU DEPTH readback differs from same-present draw manifest")
                        pixels_checked += 1
    require(set(phase_observations) == {.25, .4}, "GPU readbacks did not follow both depth phases")
    require(all(a != b for a, b in zip(phase_observations, phase_observations[1:])), "GPU readback depth phases stopped alternating")
    for relative in ("SemanticDepthLiveProbe.cpp", "SemanticDepthHostTest.cpp", "ReShadeDepthSemanticBridge.h",
                     "Shaders/AsteriaDepthSemanticBridge.fx", "Build_Semantic_Live.bat", "Run_Semantic_Live_Tests.py",
                     "Validate_Semantic_Live.py", "bin/AsteriaDepthSemanticHost.exe", "bin/AsteriaDepthSemanticLiveProbe.addon"):
        artifact(ROOT / relative)
    result = {"author": "Anokianx", "passed": True, "checks": checks, "depth_pixels_checked": pixels_checked, "dxvk_vulkan": vulkan,
              "control_cases": 3, "actual_reshade_shader_compiled": True, "actual_gpu_depth_binding_observed": True,
              "same_present_depth_changes_verified": True, "game_started": False, "main_view_confirmed": False,
              "depth_resource_owned": False, "native_fg_ready": False, "hashes": artifacts}
    report_name = "semantic-vulkan-live-validation.json" if vulkan else "semantic-live-validation.json"
    (ROOT / "results" / report_name).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--vulkan", action="store_true")
    result = validate(parser.parse_args().vulkan)
    print(f"Semantic live validation passed: {result['checks']} checks, {result['depth_pixels_checked']} actual GPU depth pixels")
