import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import re
import struct

from Native_View_Math import UnsupportedNativeView, close_components, recover_native_perspective

__author__ = "Anokianx"
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE_REFERENCE = {
    "repository": "ValveSoftware/source-sdk-2013",
    "commit": "be57e3d01ac7d7aa6a2cacbfef187c0b46616db0",
    "url": "https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/game/client/viewrender.cpp#L4477-L4509",
    "function": "CSkyboxView::DrawInternal",
    "scope": "Public Source reference, not proof of identical retail L4D2 implementation",
    "relationship": "sky_origin = main_origin / sky_camera.scale + sky_camera.origin",
    "near_plane": 2.0,
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            digest.update(block)
    return digest.hexdigest()


def native_key(values):
    return struct.pack("<16f", *values).hex()


def diagnostic(values):
    try:
        return recover_native_perspective(values)
    except UnsupportedNativeView as error:
        return {"available": False, "reason": str(error), "native_camera_ready": False,
                "main_view_verified": False, "frame_timing_verified": False, "motion_vectors_verified": False}


def read_sky_cameras(path):
    length = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(1036)
        if len(header) != 1036 or header[:4] != b"VBSP" or struct.unpack_from("<I", header, 4)[0] != 21:
            raise ValueError("Expected local retail VBSP version 21 map")
        version, offset, size, compression = struct.unpack_from("<4I", header, 8)
        if version != 0 or compression != 0 or not (1036 <= offset <= length and 0 < size <= 16777216 and offset + size <= length):
            raise ValueError("Unsupported L4D2 entity lump descriptor")
        stream.seek(offset)
        entities = stream.read(size)
    if not entities.startswith(b"{\n") or b'"classname" "worldspawn"' not in entities:
        raise ValueError("Entity lump structure does not match the guarded profile")
    found = []
    for match in re.finditer(rb"\{[^{}]*\}", entities):
        pairs = dict((key.decode("ascii"), value.decode("ascii")) for key, value in re.findall(rb'"([^"\r\n]+)"\s+"([^"\r\n]*)"', match.group()))
        if pairs.get("classname") != "sky_camera":
            continue
        origin = [float(value) for value in pairs["origin"].split()]
        scale = float(pairs["scale"])
        if len(origin) != 3 or not all(math.isfinite(value) for value in origin) or not math.isfinite(scale) or scale <= 0:
            raise ValueError("Invalid sky camera transform")
        found.append({"origin": origin, "scale": scale, "entity_properties": pairs,
                      "entity_lump_byte_offset": match.start(), "raw_entity_sha256": hashlib.sha256(match.group()).hexdigest()})
    return {"path": str(path), "sha256": sha256(path), "size_bytes": length,
            "vbsp_version": 21, "entity_lump": {"version": version, "offset": offset, "length": size, "fourcc": compression},
            "read_only": True, "sky_cameras": found}


def analyze(source, map_evidence):
    data = json.loads(source.read_text(encoding="utf-8"))
    samples = data["samples"]
    presentations = data["presentation_comparison"]["presentations"]
    absolute = data["presentation_comparison"]["absolute_tolerance"]
    relative = data["presentation_comparison"]["relative_tolerance"]
    available_getters = [p for p in presentations if p.get("native_getter", {}).get("available") and isinstance(p["native_getter"].get("raw_world_to_screen"), list)]
    getter_by_interval = {(p["device_owner_u64"], p["draw_observer_frame"]): p for p in available_getters}
    unique_getters = {}
    for presentation in available_getters:
        raw = presentation["native_getter"]["raw_world_to_screen"]
        unique_getters.setdefault(native_key(raw), {"raw_world_to_screen": raw, "intervals": [], "diagnostic": diagnostic(raw)})["intervals"].append(presentation["draw_observer_frame"])
    groups = {}
    frame_groups = defaultdict(Counter)
    match_counts = Counter()
    exact_counts = Counter()
    for sample in samples:
        raw = sample["raw_register_floats"]
        key = native_key(raw)
        if key not in groups:
            groups[key] = {"raw_register_floats": raw, "matrix_float32_sha256": hashlib.sha256(bytes.fromhex(key)).hexdigest(),
                           "sample_count": 0, "observer_frames": [], "profiles": Counter(), "diagnostic": diagnostic(raw),
                           "getter_match_distribution": Counter(), "getter_bitwise_match_distribution": Counter()}
        group = groups[key]
        group["sample_count"] += 1
        frame = sample["observer_frame"]
        group["observer_frames"].append(frame)
        group["profiles"][sample["profile_shader_sha256"]] += 1
        frame_groups[(sample["device_owner_u64"], frame)][key] += 1
        same = getter_by_interval.get((sample["device_owner_u64"], frame))
        cross_matches = [p for p in available_getters if p["device_owner_u64"] == sample["device_owner_u64"] and close_components(raw, p["native_getter"]["raw_world_to_screen"], absolute, relative)]
        same_match = same is not None and close_components(raw, same["native_getter"]["raw_world_to_screen"], absolute, relative)
        category = "same_observer_interval" if same_match else ("other_retained_interval_only" if cross_matches and same else ("own_interval_not_retained_other_interval_match" if cross_matches else "no_retained_getter_match"))
        match_counts[category] += 1
        group["getter_match_distribution"][category] += 1
        exact_same = same is not None and key == native_key(same["native_getter"]["raw_world_to_screen"])
        exact_any = any(key == native_key(p["native_getter"]["raw_world_to_screen"]) for p in available_getters if p["device_owner_u64"] == sample["device_owner_u64"])
        exact_category = "same_observer_interval" if exact_same else ("other_retained_interval_only" if exact_any and same else ("own_interval_not_retained_other_interval_match" if exact_any else "no_retained_getter_match"))
        exact_counts[exact_category] += 1
        group["getter_bitwise_match_distribution"][exact_category] += 1
    ordered = sorted(groups.items(), key=lambda item: (-item[1]["sample_count"], item[1]["matrix_float32_sha256"]))
    id_by_key = {key: index + 1 for index, (key, _) in enumerate(ordered)}
    output_groups = []
    for key, group in ordered:
        frames = sorted(set(group.pop("observer_frames")))
        group["group_id"] = id_by_key[key]
        group["observer_frames"] = frames
        group["distinct_observer_interval_count"] = len(frames)
        group["profiles"] = dict(sorted(group["profiles"].items()))
        group["getter_match_distribution"] = dict(group["getter_match_distribution"])
        group["getter_bitwise_match_distribution"] = dict(group["getter_bitwise_match_distribution"])
        group["sky_camera_relationships"] = []
        if group["diagnostic"]["available"]:
            view = group["diagnostic"]
            for getter_key, getter in unique_getters.items():
                other = getter["diagnostic"]
                if not other["available"]:
                    continue
                for sky in map_evidence["sky_cameras"]:
                    expected = [sky["origin"][i] + other["origin"][i] / sky["scale"] for i in range(3)]
                    residual = [view["origin"][i] - expected[i] for i in range(3)]
                    rotation_error = max(abs(view["world_to_view"][r][c] - other["world_to_view"][r][c]) for r in range(3) for c in range(3))
                    fov_error = max(abs(view[name] - other[name]) for name in ("horizontal_fov_degrees", "vertical_fov_degrees"))
                    group["sky_camera_relationships"].append({"map_entity_hammerid": sky["entity_properties"].get("hammerid"),
                        "scale": sky["scale"], "expected_origin": expected, "origin_residual": residual,
                        "maximum_origin_residual": max(abs(value) for value in residual),
                        "maximum_rotation_component_residual": rotation_error, "maximum_fov_residual_degrees": fov_error,
                        "sky_relationship_consistent": max(abs(value) for value in residual) <= 0.002 and rotation_error <= 0.0001 and fov_error <= 0.0001 and abs(view["near_plane"] - 2.0) <= 0.005,
                        "semantic_label_verified": False, "getter_matrix_float32_sha256": hashlib.sha256(bytes.fromhex(getter_key)).hexdigest()})
        output_groups.append(group)
    early = data.get("first_successful_samples", [])
    early_outcomes = Counter(diagnostic(sample["raw_register_floats"]).get("reason", "standard_perspective") for sample in early)
    return {"schema_version": 1, "author": __author__, "analysis_passed": True, "read_only": True,
            "source": {"path": str(source), "sha256": sha256(source), "size_bytes": source.stat().st_size,
                       "pid": data["pid"], "sample_attempts": data["sample_attempts"], "successful_samples_recorded": sum(data["outcomes"].values()),
                       "retained_samples": len(samples), "retained_getter_presentations": len(presentations), "available_getter_presentations": len(available_getters)},
            "native_math_source": {"path": str(ROOT / "InputProbe" / "EngineCameraProbe.h"), "sha256": sha256(ROOT / "InputProbe" / "EngineCameraProbe.h"),
                                   "retail_abi_report_sha256": sha256(ROOT / "Research" / "NativeBinary" / "retail-camera-abi.json"),
                                   "matrix_composition": "WorldToScreen = native standard D3D perspective * rigid WorldToView"},
            "primary_sky_reference": SOURCE_REFERENCE, "map_evidence": map_evidence,
            "matching": {"rule": "Each retained draw is compared directly to the raw getter matrix from its own retained observer interval and to every other retained getter, without transpose, rescale, regression, alignment optimization or matrix fitting",
                         "absolute_tolerance": absolute, "relative_tolerance": relative,
                         "tolerance_distribution": dict(match_counts), "float32_bitwise_distribution": dict(exact_counts),
                         "retained_unique_getter_matrices": len(unique_getters),
                         "time_shift_cannot_be_identified_from_static_getter": len(unique_getters) == 1,
                         "cross_interval_match_does_not_prove_same_frame": True,
                         "getter_diagnostics": [{**value, "matrix_float32_sha256": hashlib.sha256(bytes.fromhex(key)).hexdigest()} for key, value in unique_getters.items()]},
            "groups": output_groups,
            "observer_intervals": [{"device_owner_u64": owner, "observer_frame": frame,
                                    "has_retained_getter": (owner, frame) in getter_by_interval,
                                    "group_sample_counts": {str(id_by_key[key]): count for key, count in counts.items()},
                                    "matched_samples_in_original_presentation": getter_by_interval.get((owner, frame), {}).get("matched_samples")}
                                   for (owner, frame), counts in sorted(frame_groups.items())],
            "early_retained_samples": {"count": len(early), "decomposition_outcomes": dict(early_outcomes), "counted_in_retained_window": False},
            "limits": ["Only the final 1024 samples and 64 getter intervals are retained; neither the complete session matrix distribution nor all application views can be inferred",
                       "Sampling allows 16 distinct buffer ranges per interval; multiple UBOs can carry the same secondary view and exhaust that budget before main-view draws",
                       "Observer intervals are CPU callback ordering, not GPU submission, execution or swapchain-image ownership",
                       "Matrix recovery requires the proven native profile; a mathematically compatible matrix alone cannot establish engine identity or distinguish every possible projection convention",
                       "The map entity plus origin/rotation/FOV/near-plane agreement supports a skybox-view explanation but does not replace render-view or draw-execution provenance",
                       "No weapon-specific projection appears in the retained window; its absence here is not proof that weapon rendering shares the main camera",
                       "A single static getter matrix makes cross-interval lag unidentifiable; a later changing-camera run must record bounded execution provenance and camera changes",
                       "Near and especially far clipping reconstructed from float32 can be ill-conditioned; reported values are approximate diagnostics"],
            "native_camera_ready": False, "main_view_verified": False, "frame_timing_verified": False,
            "motion_vectors_verified": False, "native_fg_ready": False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=ROOT / "InputProbe" / "results" / "GameNativeUniform" / "NativeUniform" / "collection_observation.json")
    parser.add_argument("--map", type=Path, default=Path(r"H:\SteamLibrary\steamapps\common\Left 4 Dead 2\left4dead2\maps\c1m1_hotel.bsp"))
    parser.add_argument("--map-evidence", type=Path)
    parser.add_argument("--output", type=Path, default=HERE / "native-view-analysis.json")
    args = parser.parse_args()
    evidence = json.loads(args.map_evidence.read_text(encoding="utf-8")) if args.map_evidence else read_sky_cameras(args.map.resolve())
    result = analyze(args.source.resolve(), evidence)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    if not args.map_evidence:
        (HERE / "map-sky-camera-evidence.json").write_text(json.dumps(evidence, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"analysis_passed": result["analysis_passed"], "matching": result["matching"]["tolerance_distribution"],
                      "groups": [{"id": group["group_id"], "samples": group["sample_count"], "origin": group["diagnostic"].get("origin"),
                                  "sky_relationship_consistent": any(value["sky_relationship_consistent"] for value in group["sky_camera_relationships"])} for group in result["groups"]],
                      "native_camera_ready": False, "motion_vectors_verified": False}, ensure_ascii=False))


if __name__ == "__main__":
    main()
