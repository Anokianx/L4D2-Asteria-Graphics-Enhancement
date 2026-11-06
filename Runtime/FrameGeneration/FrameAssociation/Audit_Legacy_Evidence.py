from __future__ import annotations

import argparse
import hashlib
import json
from collections import Counter
from pathlib import Path

__author__ = "Anokianx"


def lookup(record: dict, path: str):
    current = record
    for item in path.split("."):
        if not isinstance(current, dict) or item not in current:
            return None
        current = current[item]
    return current


def main() -> None:
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=root.parent / "InputProbe/results/GameNativeUniform/NativeUniform/collection_observation.json")
    parser.add_argument("--output", type=Path, default=root / "results/legacy-evidence-audit.json")
    args = parser.parse_args()
    payload = args.source.read_bytes()
    capture = json.loads(payload.decode("utf-8-sig"))
    records = capture.get("first_successful_samples", []) + capture.get("samples", [])
    required_identity = ["command_native_handle_u64", "command_generation", "draw_ordinal", "recording_sequence",
                         "recording_tick_nanoseconds", "pipeline_generation", "bound_depth.generation",
                         "viewport.width", "viewport.height"]
    missing = Counter()
    accepted = 0
    for sample in records:
        issues = [field for field in required_identity if lookup(sample, field) is None]
        if not any(target.get("generation") for target in sample.get("bound_targets", [])):
            issues.append("bound_color.generation")
        if not lookup(sample, "descriptor.native_binding_offset_verified"):
            issues.append("descriptor.native_binding_offset_verified")
        if issues:
            missing.update(issues)
        else:
            accepted += 1
    report = {
        "author": __author__, "schema_version": 1,
        "source": str(args.source.resolve()), "source_sha256": hashlib.sha256(payload).hexdigest(),
        "records_audited": len(records), "identity_gate_passed": accepted,
        "missing_identity_counts": dict(sorted(missing.items())),
        "audit_only": True, "read_only_source": True,
        "legacy_records_converted_to_frame_associations": False,
        "observer_ids_promoted_to_native_frame_ids": False,
        "gpu_frame_aligned": False, "main_view_verified": False,
        "native_camera_ready": False, "native_fg_ready": False,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
