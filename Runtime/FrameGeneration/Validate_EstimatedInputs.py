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
ROOT = Path(__file__).resolve().parent


class EvidenceValidator:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.checks = 0
        self.artifacts = {}
        self.summaries = {}

    def require(self, condition, message):
        self.checks += 1
        if not condition:
            raise ValueError(message)

    def path(self, relative):
        result = (self.root / str(relative).replace("\\", "/")).resolve()
        self.require(result == self.root or self.root in result.parents, f"Evidence path escapes prototype: {relative}")
        return result

    def read(self, relative):
        path = self.path(relative)
        payload = path.read_bytes()
        self.artifacts[path.relative_to(self.root).as_posix()] = {"sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload)}
        return payload

    def report(self, relative):
        result = json.loads(self.read(relative).decode("utf-8-sig"))
        self.require(isinstance(result, dict), f"Report is not an object: {relative}")
        self.require(result.get("author") == __author__, f"Unexpected report author: {relative}")
        return result

    def fields(self, report, expected):
        for key, value in expected.items():
            actual = report.get(key)
            self.require(type(actual) is type(value) and actual == value, f"Report field mismatch: {key}={actual!r}; expected {value!r}")

    def near(self, actual, expected, tolerance=1e-6):
        self.require(isinstance(actual, (int, float)) and math.isfinite(actual) and
                     abs(actual - expected) <= tolerance, f"Numeric evidence mismatch: {actual!r} versus {expected!r}")

    def floats(self, relative, components):
        payload = self.read(relative)
        self.require(len(payload) == components * 4, f"Float readback size mismatch: {relative}")
        return struct.unpack(f"<{components}f", payload)

    def validate_depth(self):
        base = "DepthCapture/"
        gpu = self.report(base + "results/gpu_depth_capture.json")
        validation = self.report(base + "results/validation.json")
        self.fields(gpu, {"passed": True, "process_bits": 32, "source_depth_written_by_gpu_fragment_shader": True,
                          "actual_gpu_depth_capture_validated": True, "cpu_generated_depth_uploaded": False,
                          "native_game_depth_validated": False, "fg_ready": False, "game_started": False,
                          "sample_count": 4, "source_format": "VK_FORMAT_D24_UNORM_S8_UINT", "output_format": "VK_FORMAT_R32_SFLOAT",
                          "copy_before_clear_preserved": True, "resolve_after_original_clear_verified": True,
                          "ordinary_z_min_samples": True, "reversed_z_max_samples": True, "subsequent_clear_control_verified": True})
        self.fields(validation, {"passed": True, "game_started": False, "fg_ready": False})
        self.require(gpu["checks"] >= 9276 and validation["checks"] == gpu["checks"], "Depth contract checks do not agree")
        width, height = gpu["width"], gpu["height"]
        count = width * height
        self.require(gpu["validated_gpu_pixels"] == count * 3 == 9216, "Depth readback pixel count changed")
        values = self.floats(base + "results/readback_r32_float.bin", count * 3)
        error = 0.0
        for y in range(height):
            for x in range(width):
                plane = 0.125 + (x % 8) * 0.0625 + (y % 4) * 0.03125
                for index, expected in enumerate((plane, 0.9375, plane + 3 * 0.0078125)):
                    value = values[index * count + y * width + x]
                    self.require(math.isfinite(value) and abs(value - expected) <= 1e-6, "GPU depth pattern/clear/sample result changed")
                    error = max(error, abs(value - expected))
        self.near(gpu["max_absolute_error"], error, 1e-12)
        hashes = validation.get("hashes")
        self.require(isinstance(hashes, dict) and len(hashes) >= 13, "Depth source/binary hash manifest missing")
        for relative, expected in hashes.items():
            self.require(hashlib.sha256(self.read(base + relative)).hexdigest() == expected, f"Depth source or binary changed: {relative}")
        semantic = self.report(base + "results/reshade_semantic_contract.json")
        self.fields(semantic, {"passed": True, "typed_reshade_sdk_adapter_compiled": True, "cpu_api_mock_contract": True,
                               "actual_game_binding_observed": False, "borrowed_resources_not_owned": True,
                               "main_view_always_unconfirmed": True, "unknown_depth_conventions_not_guessed": True,
                               "fg_ready": False, "game_started": False})
        self.require(semantic["checks"] >= 44, "ReShade DEPTH semantic contract incomplete")
        source = self.report(base + "results/reshade_semantic_source_evidence.json")
        self.fields(source, {"version": "6.7.2", "actual_game_binding_observed": False, "main_view_confirmed": False, "fg_ready": False})
        for name, evidence in source["references"].items():
            payload = self.read(base + "References/ReShade-v6.7.2/" + name)
            self.require(len(payload) == evidence["bytes"] and hashlib.sha256(payload).hexdigest() == evidence["sha256"],
                         f"Pinned ReShade source mismatch: {name}")
        host = self.root.parent / "Host/ReShade32.dll"
        self.require(hashlib.sha256(host.read_bytes()).hexdigest() == source["host"]["sha256"], "Bundled ReShade host no longer matches semantic source evidence")
        self.summaries["depth"] = {"checks": gpu["checks"], "gpu_pixels_independently_checked": count * 3,
                                   "maximum_absolute_error_recomputed": error, "native_game_depth_validated": False,
                                   "semantic_mock_checks": semantic["checks"], "actual_game_semantic_binding_observed": False,
                                   "bridge_fx_compilation_verified": False}

    def validate_camera_motion(self):
        base = "MotionReconstruction/"
        report = self.report(base + "results/gpu_motion_contract.json")
        self.fields(report, {"passed": True, "process_bits": 32, "game_started": False, "native_fg_ready": False,
                             "engine_motion_vectors_available": False, "independent_object_motion_reconstructed": False,
                             "motion_convention": "previous_uv_minus_current_uv_normalized_top_left_y_down",
                             "motion_format": "VK_FORMAT_R32G32_SFLOAT", "confidence_format": "VK_FORMAT_R32_SFLOAT"})
        self.require(report["checks"] >= 461200 and len(report["cases"]) == report["gpu_dispatches"] == 60, "Camera-motion test coverage changed")
        self.fields(report, {"parameter_bytes": 240, "original_native_matrices_preserved": True,
                             "viewport_profile_inferred": False, "main_view_verified": False})
        width, height = report["width"], report["height"]
        count = width * height
        maximum_error = 0.0
        total_pixels = 0
        for case in report["cases"]:
            self.require(case.get("passed") is True and case["pixels"] == count, f"Camera-motion case failed: {case.get('name')}")
            motion = self.floats(base + case["motion_readback"], count * 2)
            confidence = self.floats(base + case["confidence_readback"], count)
            parameters=case["parameters"]
            current_depth=self.floats(base+"results/"+case["name"]+"/current_depth_r32f.bin",count)
            previous_depth=self.floats(base+"results/"+case["name"]+"/previous_depth_r32f.bin",count)
            self.require(parameters["extent_flags"][:2]==[width,height], "Camera-motion parameter extent differs from its textures")
            parameters_valid=self.camera_parameters_valid(parameters)
            valid = 0
            for index, quality in enumerate(confidence):
                x, y = motion[index * 2:index * 2 + 2]
                self.require(math.isfinite(x) and math.isfinite(y) and quality in (0.0, 1.0), "Camera-motion output is nonfinite or confidence is invalid")
                wanted=self.camera_expected(parameters,parameters_valid,current_depth,previous_depth,index,width,height)
                self.require(quality==wanted[2], f"Independently recomputed camera confidence differs: {case['name']} pixel {index}")
                self.require(max(abs(x-wanted[0]),abs(y-wanted[1]))<2e-5,
                             f"Independently recomputed camera motion differs: {case['name']} pixel {index}")
                if not quality:
                    self.require(x == 0 and y == 0, "Rejected camera-motion pixel retained displacement")
                else:
                    valid += 1
                    if case["name"] == "stationary": self.require(abs(x) < 2e-5 and abs(y) < 2e-5, "Stationary reprojection moved")
                    if case["name"] == "camera_translation_x_four_pixels":
                        self.require(abs(x - 4 / width) < 2e-5 and abs(y) < 2e-5, "X reprojection direction/units changed")
                    if case["name"] == "camera_translation_y_up_four_pixels":
                        self.require(abs(y + 4 / height) < 2e-5 and abs(x) < 2e-5, "Y reprojection direction/units changed")
            self.require(valid == case["confidence_valid_pixels"] == case["expected_valid_pixels"], "Camera-motion confidence count mismatch")
            self.require(math.isfinite(case["max_uv_error"]) and case["max_uv_error"] < 2e-5, "Camera-motion analytic error exceeds contract")
            maximum_error = max(maximum_error, case["max_uv_error"])
            total_pixels += count
        self.summaries["camera_motion"] = {"cases": len(report["cases"]), "contract_checks": report["checks"],
                                            "readback_pixels_checked": total_pixels, "maximum_contract_uv_error": maximum_error,
                                            "native_viewport_cases":sum(case["name"].startswith("native_") for case in report["cases"]),
                                            "parameters_bytes":240,"all_motion_and_confidence_pixels_independently_recomputed":True,
                                            "independent_object_motion_reconstructed": False}

    @staticmethod
    def camera_parameters_valid(p):
        def finite(values):
            return all(isinstance(v,(int,float)) and not isinstance(v,bool) and math.isfinite(v) for v in values)
        for key in ("inverse_current_view_projection","previous_view_projection"):
            values=p[key]
            if len(values)!=16 or not finite(values): return False
            rows=[list(values[n:n+4]) for n in range(0,16,4)]
            for column in range(4):
                pivot=max(range(column,4),key=lambda n:abs(rows[n][column]))
                if abs(rows[pivot][column])<=1e-10: return False
                rows[column],rows[pivot]=rows[pivot],rows[column]
                for row in range(column+1,4):
                    factor=rows[row][column]/rows[column][column]
                    rows[row]=[a-factor*b for a,b in zip(rows[row],rows[column])]
        limits=p["depth_limits"]; controls=p["previous_depth_controls"]
        if not finite(limits+controls) or not 0<=limits[0]<limits[1]<=1 or limits[3]<0 or min(controls[:2])<0: return False
        modes=p["viewport_modes"]; ranges=p["viewport_depth_ranges"]
        for index,key in enumerate(("current_viewport_rectangle","previous_viewport_rectangle")):
            if modes[index]==0: continue
            if modes[index]!=1 or modes[index+2]!=1: return False
            rect=p[key]; endpoints=ranges[index*2:index*2+2]
            if not finite(rect+endpoints) or rect[2]<=0 or rect[3]==0 or not all(0<=v<=1 for v in endpoints) or endpoints[0]==endpoints[1]: return False
            if any(abs(v)>3.402823466e38 for v in (rect[0]+rect[2],rect[1]+rect[3],2/rect[2],2/rect[3],1/(endpoints[1]-endpoints[0]))): return False
        return True

    @staticmethod
    def camera_expected(p,valid,current,previous,index,width,height):
        rejected=(0.0,0.0,0.0)
        flags=p["extent_flags"][2]
        if not valid or not flags&1 or flags&2: return rejected
        limits=p["depth_limits"]; modes=p["viewport_modes"]; ranges=p["viewport_depth_ranges"]
        def depth_valid(value):
            return math.isfinite(value) and limits[0]<=value<=limits[1] and abs(value-limits[2])>limits[3]
        depth=current[index]
        if not depth_valid(depth): return rejected
        px=index%width+.5; py=index//width+.5
        clip=[2*px/width-1,1-2*py/height,depth,1.0]
        if modes[0]==1:
            rect=p["current_viewport_rectangle"]
            clip=[2*(px-rect[0])/rect[2]-1,2*(py-rect[1])/rect[3]-1,(depth-ranges[0])/(ranges[1]-ranges[0]),1.0]
            if not all(-1<=v<=1 for v in clip[:2]) or not 0<=clip[2]<=1: return rejected
        def transform(matrix,vector):
            return [sum(matrix[row*4+column]*vector[column] for column in range(4)) for row in range(4)]
        world=transform(p["inverse_current_view_projection"],clip)
        if not all(math.isfinite(v) for v in world) or abs(world[3])<=1e-7: return rejected
        projected=transform(p["previous_view_projection"],[v/world[3] for v in world])
        if not all(math.isfinite(v) for v in projected) or projected[3]<=1e-7: return rejected
        ndc=[v/projected[3] for v in projected[:3]]
        u=(ndc[0]+1)/2; v=(1-ndc[1])/2; predicted=ndc[2]
        if modes[1]==1:
            if not all(-1<=value<=1 for value in ndc[:2]) or not 0<=ndc[2]<=1: return rejected
            rect=p["previous_viewport_rectangle"]
            u=(rect[0]+rect[2]*(ndc[0]+1)/2)/width
            v=(rect[1]+rect[3]*(ndc[1]+1)/2)/height
            predicted=ranges[2]+(ranges[3]-ranges[2])*ndc[2]
        if not all(math.isfinite(value) for value in (u,v,predicted)) or not 0<=u<1 or not 0<=v<1 or not limits[0]<=predicted<=limits[1]: return rejected
        if flags&4:
            sampled=previous[math.floor(v*height)*width+math.floor(u*width)]
            absolute,relative=p["previous_depth_controls"][:2]
            if not depth_valid(sampled) or abs(sampled-predicted)>max(absolute,relative*max(abs(sampled),abs(predicted))): return rejected
        return u-px/width,v-py/height,1.0

    def validate_association(self):
        report = self.report("FrameAssociation/results/frame-association-contract.json")
        self.fields(report, {"passed": True, "game_started": False, "actual_gpu_inputs_validated": False,
                             "observer_interval_only": True, "gpu_frame_aligned": False, "main_view_verified": False,
                             "native_camera_ready": False, "native_fg_ready": False, "motion_vectors_verified": False})
        self.require(report["checks"] >= 222, "Frame-association contract incomplete")
        legacy = self.report("FrameAssociation/results/legacy-evidence-audit.json")
        self.fields(legacy, {"records_audited": 1040, "identity_gate_passed": 0,
                             "legacy_records_converted_to_frame_associations": False,
                             "observer_ids_promoted_to_native_frame_ids": False,
                             "gpu_frame_aligned": False, "main_view_verified": False, "native_fg_ready": False})
        source = Path(legacy["source"]).resolve()
        self.require(self.root in source.parents, "Legacy source escaped prototype")
        self.require(hashlib.sha256(self.read(source.relative_to(self.root))).hexdigest() == legacy["source_sha256"], "Legacy capture hash changed")
        self.summaries["frame_association"] = {"checks": report["checks"], "legacy_records_audited": 1040,
                                               "legacy_records_upgraded": 0, "observer_interval_only": True,
                                               "gpu_frame_aligned": False, "main_view_verified": False}

    def check_estimated_region(self, motion_path, confidence_path, width, height, x0, y0, dx, dy):
        motion_bytes = self.read(motion_path)
        self.require(len(motion_bytes) == width * height * 4, f"Estimated motion byte size mismatch: {motion_path}")
        motion = tuple(struct.iter_unpack("<ee", motion_bytes))
        confidence = self.floats(confidence_path, width * height)
        self.require(all(math.isfinite(x) and math.isfinite(y) for x, y in motion), "Estimated motion is nonfinite")
        self.require(all(math.isfinite(q) and 0 <= q <= 1 for q in confidence), "Estimated confidence outside [0,1]")
        accepted = tested = 0
        error = 0.0
        for y in range(y0, y0 + 96):
            for x in range(x0, x0 + 72):
                tested += 1
                index = y * width + x
                if confidence[index] >= 0.25:
                    accepted += 1
                    u, v = motion[index]
                    error += math.hypot(u * width - dx, v * height - dy)
        self.require(accepted > 0, "Estimated motion region has no accepted pixels")
        return {"accepted": accepted, "tested": tested, "coverage": accepted / tested, "error": error / accepted}

    def validate_flow(self):
        base = "EstimatedMotion/"
        report = self.report(base + "results/flow_contract.json")
        self.fields(report, {"passed": True, "process_bits": 32, "game_started": False, "game_frame_generation": False,
                             "real_game_inputs": False, "synthetic_scene": True, "motion_source": "Estimated",
                             "optical_flow_vectors_manufactured": False, "scene_cut_rejection": True, "history_reset_rejection": True})
        self.require(report["verified_motion_cases"] == len(report["cases"]) == 4 and report["rejection_cases"] >= 44,
                     "Estimated motion case coverage incomplete")
        self.require(report["actual_scene_cut_history"] & 15 and report["sdk_warmup_static_fixture_frames_per_direction"] >= 6,
                     "Actual scene-cut or SDK warm-up evidence missing")
        width, height = report["shader_outputs"]["width"], report["shader_outputs"]["height"]
        for index, case in enumerate(report["cases"], 1):
            self.require(case.get("passed") is True, "Estimated motion case failed")
            dx, dy = case["object_delta_pixels"]
            measured = self.check_estimated_region(base + f"results/motion_{index}.rg16f", base + f"results/confidence_{index}.r32f",
                                                    width, height, 240 + dx, 128 + dy, -dx, -dy)
            self.require(measured["accepted"] == case["accepted_pixels"] and measured["tested"] == case["tested_pixels"], "Estimated motion pixel counts changed")
            self.near(measured["coverage"], case["coverage"])
            self.near(measured["error"], case["mean_endpoint_error_pixels"])
            self.require(measured["coverage"] > 0.30 and measured["error"] < 1.25, "Estimated motion accuracy/coverage failed")
        self.summaries["estimated_motion"] = {"motion_cases": 4, "rejection_cases": report["rejection_cases"],
                                               "actual_scene_cut_history": report["actual_scene_cut_history"],
                                               "static_fixture_recovery_dispatches": report["scene_cut_static_fixture_recovery_dispatches"],
                                               "synthetic_scene": True, "real_game_inputs": False}

    def ppm(self, relative):
        payload = self.read(relative)
        match = re.match(rb"P6\n([0-9]+) ([0-9]+)\n255\n", payload)
        self.require(match is not None, f"Invalid fixture PPM header: {relative}")
        width, height = map(int, match.groups())
        pixels = payload[match.end():]
        self.require(len(pixels) == width * height * 3, f"Incomplete PPM pixels: {relative}")
        return width, height, pixels

    def centroid(self, width, height, pixels):
        total = count = 0
        self.require(height >= 260, "Fixture centroid range exceeds image")
        for y in range(100, 260):
            for x in range(width):
                offset = (y * width + x) * 3
                red, green, blue = pixels[offset:offset + 3]
                if red > 180 and 60 < green < 180 and blue < 100:
                    total += x
                    count += 1
        self.require(count > 0, "Fixture object disappeared")
        return total / count

    def validate_interpolation(self):
        base = "EstimatedMotion/"
        report = self.report(base + "results/EstimatedInterpolation/estimated_interpolation.json")
        self.fields(report, {"passed": True, "stage": "complete", "process_bits": 32, "synthetic_scene": True,
                             "real_game_inputs": False, "game_started": False, "game_frame_generation": False,
                             "motion_source": "Estimated", "ground_truth_motion_uploaded": False,
                             "generated_frames_presented": 0, "hud_composition_validated": False,
                             "warmup_frames_are_real_game_frames": False,
                             "synthetic_estimated_motion_gpu_interpolation_verified": True,
                             "active_optical_flow_dispatches_per_input_frame": 3})
        self.require(report["input_frames"] == len(report["frames"]) == report["official_interpolation_dispatches"] == 8 and
                     report["verified_estimated_motion_midpoints"] == 7, "Estimated interpolation frame counts disagree")
        self.require(report["forward_of_dispatches_total"] == 14 and report["reverse_of_dispatches_total"] == 22 and
                     report["all_of_dispatches_including_synthetic_warmup"] == 36,
                     "Forward/reverse warmup and continuous-pair dispatch counts disagree")
        previous_current = None
        frames = []
        for frame in report["frames"]:
            number = frame["frame_id"]
            self.require(number == len(frames) and frame["reset"] is (number == 0), "Estimated interpolation frame order/reset changed")
            self.require(frame["optical_flow_dispatches"] == 3, "Actual pair requires one forward plus two reverse dispatches per actual frame")
            self.require(frame["forward_context_frame_index"] > 5 and frame["reverse_context_frame_index"] > 5,
                         "Uninitialized SDK optical-flow warmup vectors were accepted")
            directory = base + str(frame["readback_directory"]).replace("\\", "/").removeprefix("./") + "/"
            images = [self.ppm(directory + name + ".ppm") for name in ("previous", "current", "generated")]
            width, height = images[0][:2]
            self.require(all(image[:2] == (width, height) for image in images), "Interpolation image dimensions disagree")
            before, current, generated = (image[2] for image in images)
            if previous_current is not None:
                self.require(before == previous_current, "Consecutive interpolation inputs do not share the same retained frame")
            previous_current = current
            centers = [self.centroid(*image) for image in images]
            differences = [sum(generated[offset:offset + 3] != source[offset:offset + 3]
                               for offset in range(0, len(source), 3)) for source in (before, current)]
            for key, value in zip(("previous_centroid_x", "current_centroid_x", "generated_centroid_x"), centers): self.near(frame[key], value)
            self.require(differences == [frame["pixels_different_from_previous"], frame["pixels_different_from_current"]], "Generated-pixel differences disagree")
            expected = (centers[0] + centers[1]) / 2
            error = abs(centers[2] - expected)
            self.near(frame["expected_centroid_x"], expected)
            self.near(frame["midpoint_centroid_error_pixels"], error)
            if number:
                self.require(frame["midpoint_verified"] is True and error < 1 and min(differences) > 500 and
                             centers[0] + 1 < centers[2] < centers[1] - 1, "Estimated midpoint is a copied input or wrong position")
            else:
                self.require(frame["midpoint_verified"] is False, "Reset copy was counted as a midpoint")
            measured = self.check_estimated_region(directory + "estimated_motion.rg16f", directory + "confidence.r32f",
                                                    width, height, 240 + number * 12, 128, -12 if number else 0, 0)
            self.require(measured["accepted"] == frame["accepted_object_pixels"] and measured["tested"] == frame["tested_object_pixels"], "Continuous estimated motion count mismatch")
            self.near(measured["error"], frame["mean_motion_endpoint_error_pixels"])
            self.near(measured["coverage"], frame["coverage"])
            flow_extent = ((width + 7) // 8, (height + 7) // 8)
            for name, wanted, x0 in (("forward", -12 if number else 0, 240 + number * 12),
                                      ("reverse", 12 if number else 0, 240 + max(0, number - 1) * 12)):
                raw = self.read(directory + name + "_flow.rg16i")
                self.require(len(raw) == flow_extent[0] * flow_extent[1] * 4, "Block-flow readback size mismatch")
                values = [struct.unpack_from("<hh", raw, ((y // 8) * flow_extent[0] + x // 8) * 4)
                          for y in range(144, 208, 8) for x in range(x0 + 16, x0 + 56, 8)]
                self.require(all(abs(x - wanted) <= 1 and abs(y) <= 1 for x, y in values), f"{name} optical flow does not match the actual pair direction")
            frames.append({"frame_id": number, "reset": number == 0, "midpoint_error_pixels_recomputed": error,
                           "distinct_pixel_counts_recomputed": differences, "active_optical_flow_dispatches": frame["optical_flow_dispatches"]})
        lifetime = report["independent_descriptor_gpu_case"]
        self.fields(lifetime, {"passed": True, "dispatches_in_same_command_buffer": 2, "different_outputs_and_history_flags": True,
                               "distinct_descriptor_pools_and_sets": True, "pipeline_owner_destroyed_before_submit": True,
                               "pipeline_retained_by_tokens_until_gpu_completion": True, "last_token_released_pipeline": True,
                               "second_dispatch_all_motion_and_confidence_zero": True, "sentinel_outputs_overwritten_by_actual_gpu": True})
        self.require(report["motion_filter_dispatches_including_lifetime_case"] == 10, "Independent descriptor dispatch case was not run")
        lifetime_base = base + "results/EstimatedInterpolation/"
        first_quality = self.floats(lifetime_base + "independent_first_confidence.r32f", width * height)
        reset_quality = self.floats(lifetime_base + "independent_reset_confidence.r32f", width * height)
        reset_motion = self.read(lifetime_base + "independent_reset_motion.rg16f")
        self.require(all(value == 0 for value in reset_quality) and len(reset_motion) == width * height * 4 and not any(reset_motion),
                     "Second same-command dispatch did not independently reset GPU outputs")
        valid = sum(first_quality[y * width + x] >= 0.25 for y in range(128, 224) for x in range(324, 396))
        self.require(valid == lifetime["first_dispatch_valid_object_pixels"] == 6912, "First same-command dispatch was overwritten by second descriptor update")
        timing = self.validate_gpu_timing(report)
        self.summaries["estimated_interpolation"] = {"input_frames": 8, "verified_midpoints": 7,
                "active_optical_flow_dispatches_per_frame": 3, "forward_dispatches_per_frame": 1, "reverse_dispatches_per_frame": 2,
                "all_of_dispatches_including_synthetic_warmup": 36, "frames": frames,
                "independent_descriptor_gpu_case": lifetime, "gpu_timing": timing,
                "fixture_wall_time_is_not_game_performance": True, "generated_frames_presented": 0,
                "hud_composition_validated": False, "game_frame_generation": False}

    def validate_gpu_timing(self, report):
        metadata = report["gpu_timing"]
        summary = report["gpu_timing_summary"]
        names = ("forward_of", "reverse_seed_of", "reverse_pair_of", "filter", "prepare_and_fi")
        self.fields(metadata, {"counter_difference": "masked_modulo_queue_timestampValidBits", "query_slots": 10,
                               "game_fps_measured": False, "four_k_performance_measured": False,
                               "scope": "640x360_synchronous_synthetic_fixture_GPU_commands_only"})
        self.require(type(metadata["available"]) is bool, "GPU timing availability is not explicit")
        available = metadata["available"]
        bits = metadata["queue_timestamp_valid_bits"]
        period = metadata["timestamp_period_nanoseconds"]
        if available:
            self.require(type(bits) is int and 1 <= bits <= 64 and isinstance(period, (int, float)) and
                         math.isfinite(period) and period > 0 and metadata["unavailable_reason"] is None,
                         "Available GPU timestamp metadata has invalid counter width/period/reason")
        else:
            self.require(isinstance(metadata["unavailable_reason"], str) and bool(metadata["unavailable_reason"]),
                         "Unavailable GPU timing has no reason")
        mask = (1 << bits) - 1 if available else 0
        durations = {name: [] for name in (*names, "sum")}
        timed_frames = 0
        timed_segments = 0
        unavailable_frames = []
        for frame in report["frames"]:
            is_timed = frame["gpu_timing_available"]
            self.require(type(is_timed) is bool, "Per-frame GPU timing availability is not explicit")
            work = frame["gpu_work_ms"]
            raw = frame["gpu_timestamp_ticks"]
            if not is_timed:
                self.require(isinstance(frame["gpu_timing_unavailable_reason"], str) and bool(frame["gpu_timing_unavailable_reason"]) and
                             all(work.get(name) is None for name in durations) and not raw,
                             "Unavailable GPU timing must retain null durations and a reason, not zeros")
                unavailable_frames.append({"frame_id": frame["frame_id"], "reason": frame["gpu_timing_unavailable_reason"]})
                continue
            self.require(available and frame["gpu_timing_unavailable_reason"] is None and set(raw) == set(names),
                         "Timed GPU frame has no valid query metadata or incomplete timestamp segments")
            total = 0.0
            for name in names:
                ticks = raw[name]
                first, last, declared_delta = ticks["begin"], ticks["end"], ticks["masked_delta"]
                self.require(all(type(value) is int and 0 <= value <= (1 << 64) - 1 for value in (first, last, declared_delta)),
                             "Raw GPU timestamp value is not uint64")
                delta = ((last & mask) - (first & mask)) & mask
                self.require(delta == declared_delta, "GPU timestamp wrap-mask delta disagrees")
                duration = delta * period / 1000000.0
                self.near(work[name], duration, 1e-9)
                durations[name].append(duration)
                total += duration
                timed_segments += 1
            self.near(work["sum"], total, 1e-9)
            durations["sum"].append(total)
            timed_frames += 1
        all_available = timed_frames == len(report["frames"])
        self.fields(summary, {"available": all_available, "frame_count": len(report["frames"]),
                              "scope": "640x360_synchronous_synthetic_fixture_not_game_or_4k"})
        self.require(summary["timed_frame_count"] == (timed_frames if all_available else 0), "GPU timing summary count disagrees")
        recomputed = {}
        for name, values in durations.items():
            if all_available:
                median, maximum = statistics.median(values), max(values)
                self.near(summary["gpu_work_ms"][name]["median"], median, 1e-9)
                self.near(summary["gpu_work_ms"][name]["max"], maximum, 1e-9)
                recomputed[name] = {"median": median, "max": maximum}
            else:
                self.require(summary["gpu_work_ms"][name]["median"] is None and summary["gpu_work_ms"][name]["max"] is None,
                             "Incomplete GPU query history was summarized as measured zero time")
                recomputed[name] = {"median": None, "max": None}
        return {"available": all_available, "counter_valid_bits": bits, "timestamp_period_nanoseconds": period,
                "timed_frames": timed_frames, "segments_independently_recomputed": timed_segments,
                "gpu_work_ms_recomputed": recomputed, "unavailable_frames": unavailable_frames,
                "scope": "640x360_synchronous_synthetic_fixture_GPU_segments_only_not_game_FPS_or_4K"}

    def hash_sources(self):
        extensions = {".h", ".hpp", ".cpp", ".py", ".bat", ".comp", ".vert", ".frag", ".fx", ".spv", ".exe"}
        for directory in ("DepthCapture", "MotionReconstruction", "FrameAssociation", "EstimatedMotion"):
            for path in sorted(self.path(directory).rglob("*")):
                if path.is_file() and path.suffix.lower() in extensions:
                    self.read(path.relative_to(self.root))
        self.read("Port/InterpolationProbe.cpp")
        self.read("Research/sdk-v1.1.4/sdk/include/FidelityFX/gpu/opticalflow/ffx_opticalflow_callbacks_glsl.h")
        self.read(Path(__file__).relative_to(self.root))
        for relative, expected in self.artifacts.copy().items():
            payload = self.path(relative).read_bytes()
            self.require(len(payload) == expected["bytes"] and hashlib.sha256(payload).hexdigest() == expected["sha256"],
                         f"Source/report/artifact changed while validating: {relative}")

    def validate_build_freshness(self):
        jobs = [
            ("DepthCapture/bin/AsteriaDepthCaptureHost.exe", "DepthCapture/results/gpu_depth_capture.json",
             ["DepthCapture/HostTest.cpp", "DepthCapture/DepthCapture.cpp", "DepthCapture/DepthCapture.h", "DepthCapture/CandidateSelector.h",
              "DepthCapture/VulkanFunctions.h", "DepthCapture/bin/ResolveDepth.spv", "DepthCapture/bin/Fixture.frag.spv", "DepthCapture/bin/Fixture.vert.spv"]),
            ("DepthCapture/bin/AsteriaReShadeDepthSemanticContract.exe", "DepthCapture/results/reshade_semantic_contract.json",
             ["DepthCapture/ReShadeDepthSemanticBridge.h", "DepthCapture/ReShadeDepthSemanticContract.cpp"]),
            ("MotionReconstruction/build/AsteriaGpuMotionContract.exe", "MotionReconstruction/results/gpu_motion_contract.json",
             ["MotionReconstruction/GpuMotionContract.cpp", "MotionReconstruction/CameraMotionReconstruction.h", "MotionReconstruction/NativeViewportTransform.h",
              "MotionReconstruction/build/ReconstructMotion.spv", "Port/InterpolationProbe.cpp"]),
            ("FrameAssociation/bin/AsteriaFrameAssociationContract.exe", "FrameAssociation/results/frame-association-contract.json",
             ["FrameAssociation/ObservedFrameAssociation.h", "FrameAssociation/FrameAssociationContract.cpp"]),
            ("EstimatedMotion/bin/AsteriaGpuFlowContract.exe", "EstimatedMotion/results/flow_contract.json",
             ["EstimatedMotion/GpuFlowContract.cpp", "EstimatedMotion/BidirectionalFlowMotion.h", "EstimatedMotion/FilterFlow.spv", "Port/InterpolationProbe.cpp"]),
            ("EstimatedMotion/bin/AsteriaEstimatedInterpolationProbe.exe", "EstimatedMotion/results/EstimatedInterpolation/estimated_interpolation.json",
             ["EstimatedMotion/EstimatedInterpolationProbe.cpp", "EstimatedMotion/GpuFlowContract.cpp",
              "EstimatedMotion/BidirectionalFlowMotion.h", "EstimatedMotion/FilterFlow.spv", "Port/InterpolationProbe.cpp"]),
        ]
        for executable, report, sources in jobs:
            built = self.path(executable).stat().st_mtime_ns
            self.require(self.path(report).stat().st_mtime_ns >= built, f"Report predates its executable: {report}")
            for source in sources:
                boundary = self.path(report).stat().st_mtime_ns if source.endswith(".spv") else built
                self.require(self.path(source).stat().st_mtime_ns <= boundary, f"Executable/report predates compiled source or runtime shader: {source}")
        for source, compiled in [("DepthCapture/Shaders/ResolveDepth.comp", "DepthCapture/bin/ResolveDepth.spv"),
                                 ("DepthCapture/Shaders/Fixture.frag", "DepthCapture/bin/Fixture.frag.spv"),
                                 ("DepthCapture/Shaders/Fixture.vert", "DepthCapture/bin/Fixture.vert.spv"),
                                 ("MotionReconstruction/ReconstructMotion.comp", "MotionReconstruction/build/ReconstructMotion.spv"),
                                 ("EstimatedMotion/FilterFlow.comp", "EstimatedMotion/FilterFlow.spv")]:
            self.require(self.path(source).stat().st_mtime_ns <= self.path(compiled).stat().st_mtime_ns,
                         f"Compiled shader predates its source: {source}")
        self.summaries["build_freshness"] = {"executable_report_chains_checked": len(jobs),
                                               "shader_source_chains_checked": 5,
                                               "timestamp_check_is_not_reproducible_build_proof": True}

    def run(self):
        self.validate_depth()
        self.validate_camera_motion()
        self.validate_association()
        self.validate_flow()
        self.validate_interpolation()
        self.validate_build_freshness()
        self.hash_sources()
        return self.summaries


def main() -> int:
    validator = EvidenceValidator(ROOT)
    report = {"author": __author__, "schema_version": 1, "passed": False,
              "scope": "isolated_gpu_depth_reconstruction_estimation_and_interpolation_evidence",
              "native_game_depth_validated": False, "native_motion_vectors_available": False,
              "same_gpu_frame_association_validated": False, "main_view_verified": False,
              "hud_composition_validated": False, "real_game_input_producer_available": False,
              "game_frame_generation": False, "native_fg_ready": False,
              "generated_frames_presented": 0, "performance_benchmark": False, "game_started": False,
              "synthetic_fixture_inputs": True}
    try:
        report["modules"] = validator.run()
        report["passed"] = True
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
    report["modules"] = validator.summaries
    report["checks"] = validator.checks
    report["artifacts"] = dict(sorted(validator.artifacts.items()))
    (ROOT / "estimated-input-validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "artifacts"}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
