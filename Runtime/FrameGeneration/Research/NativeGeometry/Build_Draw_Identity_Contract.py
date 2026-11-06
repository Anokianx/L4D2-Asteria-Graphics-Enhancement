import hashlib
import json
import pathlib
import re


AUTHOR = "Anokianx"
DIRECTORY = pathlib.Path(__file__).resolve().parent
SDK = DIRECTORY.parents[2] / "sdk/reshade"
EVENTS = ["bind_vertex_buffers", "bind_index_buffer", "draw", "draw_indexed", "draw_or_dispatch_indirect", "init_pipeline", "destroy_pipeline", "bind_pipeline", "bind_pipeline_states", "init_resource", "destroy_resource", "reset_command_list", "destroy_command_list", "execute_command_list", "execute_secondary_command_list", "update_buffer_region", "map_buffer_region", "unmap_buffer_region", "copy_buffer_region", "copy_resource", "update_buffer_region_command"]


def main():
    event_path = SDK / "reshade_events.hpp"
    pipeline_path = SDK / "reshade_api_pipeline.hpp"
    event_bytes = event_path.read_bytes()
    pipeline_bytes = pipeline_path.read_bytes()
    text = event_bytes.decode("utf-8-sig")
    evidence = []
    for name in EVENTS:
        match = re.search(r"^[ \t]*RESHADE_DEFINE_ADDON_EVENT_TRAITS\(addon_event::" + name + r",.*$", text, re.M)
        if not match:
            raise ValueError("SDK event trait missing: " + name)
        evidence.append({"event": name, "line": text.count("\n", 0, match.start()) + 1, "declaration": match[0].strip(), "path": str(event_path)})
    pipeline_text = pipeline_bytes.decode("utf-8-sig")
    start = pipeline_text.index("\tstruct input_element\n")
    stop = pipeline_text.index("\n\t};", start) + len("\n\t};")
    fields = [line.strip() for line in pipeline_text[start:stop].splitlines() if line.strip().endswith(";") and not line.lstrip().startswith("///")]
    required = ["location", "semantic", "semantic_index", "format", "buffer_binding", "offset", "stride", "instance_step_rate"]
    if any(not any(re.search(r"\b" + member + r"\s*=", line) for line in fields) for member in required):
        raise ValueError("SDK input layout fields changed")
    report = {
        "schema_version": 1,
        "author": AUTHOR,
        "date": "2026-10-15",
        "sdk_files": [{"path": str(path), "sha256": hashlib.sha256(data).hexdigest()} for path, data in [(event_path, event_bytes), (pipeline_path, pipeline_bytes)]],
        "event_traits": evidence,
        "input_element": {"line": pipeline_text.count("\n", 0, start) + 1, "path": str(pipeline_path), "declarations": fields},
        "draw_snapshot_fields": ["device lifetime generation", "command-list lifetime generation and reset serial", "observer frame and per-command draw ordinal", "pipeline lifetime generation and exact shader profile SHA-256", "copied input layout including semantic string content", "each VB resource generation, byte offset, known stride or explicit unknown", "IB resource generation, byte offset and 16/32-bit index size", "indexed versus non-indexed draw parameters", "all instance parameters", "native RT/depth/view scope and viewport", "same-draw native camera/model/bone input snapshot", "buffer content/write epoch or explicit unknown"],
        "not_engine_entity_identity": ["VB/IB handles are reused and can be shared by many entities", "draw order changes with culling/material sorting/LOD and is not an entity id", "buffer offsets identify streaming slices, not persistent actors", "pipeline/layout are shared across models and actors", "bone matrices can match for different actors and must not serve as unique ids", "same structural signature does not prove the same mesh contents after writes", "first_instance and instance count describe a draw, not an engine lifetime", "draws recorded before present do not alone establish the final main-view frame"],
        "required_fail_closed_boundaries": ["generation change or destruction", "command reset/secondary execution not propagated", "unknown layout or null optional stride", "buffer write or content epoch not tracked", "indirect parameter buffer not safely captured", "unresolved instancing/skinning/flex state", "camera/view/main-depth ambiguity", "duplicate signature in one frame or cross-frame collision", "tracking capacity loss", "map/scene/device reset"],
        "native_previous_transform_api_found_in_public_reshade_events": False,
        "native_entity_lifetime_api_found_in_public_reshade_events": False,
        "cross_frame_entity_identity_verified": False,
        "native_motion_vectors_available": False,
        "scope": "Local installed public ReShade API event declarations only; no entity/previous-transform contract is present in the listed draw interfaces.",
    }
    output = DIRECTORY / "draw-identity-contract.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "sdk_event_traits": len(evidence), "input_layout_fields": len(fields), "sha256": hashlib.sha256(output.read_bytes()).hexdigest()}))


if __name__ == "__main__":
    main()
