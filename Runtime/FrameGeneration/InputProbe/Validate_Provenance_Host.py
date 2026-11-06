from pathlib import Path
import hashlib
import json
import struct


ROOT = Path(__file__).resolve().parent


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def validate():
    fixture = ROOT / "results/HostPipeline"
    host = read(fixture / "validation.json")
    report = read(fixture / "Provenance/status.json")
    require(host["passed"] and host["pipeline_probe_enabled"] and not host["probe_disabled"], "Provenance fixture failed")
    require(report["read_only"] and report["enabled"] and not report["native_fg_ready"] and not report["motion_vectors_verified"], "Diagnostic readiness or mode changed")
    require(not report["session_active"] and not report["resources_are_live"], "Fixture provenance retained live state after unload")
    coverage = report["coverage"]
    require(coverage["binding_association_without_capture_losses"] and not any(coverage["limits_hit"].values()), "Pipeline tracking exceeded its coverage bounds")
    require(all(coverage[name] == 0 for name in ("unknown_pipeline_binds", "unknown_draw_pipelines", "missing_target_resources", "shader_payload_missing")), "Application shader binding was missed")
    require(report["writer"]["hash_failures_this_snapshot"] == report["writer"]["file_failures_this_snapshot"] == 0, "Shader capture did not reach disk")
    require(len(report["shaders"]) == len(report["pipelines"]) == len(report["associations"]) == 2, "Effect shaders entered the application fixture capture")
    shader_hashes = {}
    for item in report["shaders"]:
        relative = Path(item["file"])
        require(not relative.is_absolute() and ".." not in relative.parts, "Unsafe shader artifact path")
        data = (fixture / "Provenance" / relative).read_bytes()
        require(len(data) == item["bytes"] and hashlib.sha256(data).hexdigest() == item["sha256"], "Shader bytecode differs from its recorded hash")
        require(struct.unpack_from("<I", data)[0] == 0xFFFF0200, "Fixture payload is not the compiled D3D9 pixel shader")
        shader_hashes[item["id"]] = item["sha256"]
    resources = {item["id"]: item for item in report["resources"]}
    pipelines = {item["id"]: item for item in report["pipelines"]}
    cases = (
        ("fixture_first_ps.bin", host["known_fixture_handles"]["rg_texture"]),
        ("fixture_second_ps.bin", host["known_fixture_handles"]["backbuffer_surface"]),
    )
    verified = []
    for file_name, target_handle in cases:
        expected_hash = hashlib.sha256((fixture / file_name).read_bytes()).hexdigest()
        found = []
        for association in report["associations"]:
            bound_targets = [resources[value]["handle_hex"] for value in association["bound_render_target_resource_ids"]]
            if target_handle not in bound_targets:
                continue
            for binding in association["pipeline_stage_bindings"]:
                pipeline = pipelines[binding["pipeline_record_id"]]
                if any(shader_hashes[shader["blob_id"]] == expected_hash and shader["pipeline_stage_bits"] == 128 for shader in pipeline["shaders"]):
                    found.append(association)
        require(len(found) == 1 and found[0]["recorded_draws"] == host["frames_presented"] and not found[0]["recorded_dispatches"], "Fixture shader was associated with the wrong target or draw count")
        verified.append({"bytecode": file_name, "sha256": expected_hash, "target_handle": target_handle, "recorded_draws": found[0]["recorded_draws"]})
    disabled = ROOT / "results/HostPipelineDisabled"
    disabled_host = read(disabled / "validation.json")
    require(disabled_host["passed"] and disabled_host["probe_disabled"] and not (disabled / "native_input_status.json").exists() and not (disabled / "Provenance/status.json").exists(), "Disabled addon emitted discovery data")
    result = {"author": "Anokianx", "passed": True, "mode": "known_pixel_shader_target_provenance", "native_fg_ready": False,
              "motion_vectors_verified": False, "effects_excluded": True, "disabled_case_passed": True, "cases": verified}
    (fixture / "provenance_validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    validate()
