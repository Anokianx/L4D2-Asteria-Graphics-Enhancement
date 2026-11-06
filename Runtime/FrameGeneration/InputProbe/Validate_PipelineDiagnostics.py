from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
from typing import Any

__author__ = "Anokianx"
ROOT = Path(__file__).resolve().parent
RESULTS = ROOT / "results"
MAX_SAMPLES = 2048
MAX_RECORDS = 256
MAX_SUBMIT_ELEMENTS = 32
MAX_DEPENDENCIES = 4
MAX_CHAIN = 8
MAX_STAGES = 8
MAX_DYNAMIC = 128
MAX_FORMATS = 8
MODULE_CREATE = 16
MODULE_IDENTIFIER = 1000462002
DEPTH_CLIP = 1000102001
LIBRARIES = 1000290000
RENDERING = 1000044002
IDENTITY_FIELDS = ("native_device", "device_generation", "pipeline", "pipeline_generation")
STATE_FIELDS = ("vertex_input_state", "input_assembly_state", "tessellation_state", "viewport_state",
                "rasterization_state", "multisample_state", "depth_stencil_state", "color_blend_state", "dynamic_state")


def uint(value: Any, positive: bool = False) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value <= 0xFFFFFFFFFFFFFFFF and (value > 0 if positive else value >= 0)


def identity_key(device: int, device_generation: int, pipeline: int, pipeline_generation: int) -> str:
    return ":".join(str(item) for item in (device, device_generation, pipeline, pipeline_generation))


def record_identity(record: dict[str, Any]) -> tuple[int, int, int, int]:
    return tuple(record.get(field) for field in IDENTITY_FIELDS)


def normalized_hint(value: Any) -> Any:
    return None if value is None or value == 0xFFFFFFFF else value


def computed_recording_match(sample: dict[str, Any], diagnostic: dict[str, Any]) -> bool:
    draw = sample.get("draw_recording", {})
    viewport = draw.get("pipeline_viewport", {})
    flags = diagnostic.get("flags")
    values = (sample.get("device_handle_u64"), sample.get("command_native_handle_u64"),
              sample.get("vertex_pipeline_handle_u64"), draw.get("native_command"),
              draw.get("native_device_generation"), draw.get("native_command_allocation_generation"),
              draw.get("native_command_recording_generation"), diagnostic.get("native_device"),
              diagnostic.get("device_generation"), diagnostic.get("command_generation"),
              diagnostic.get("recording_generation"), diagnostic.get("pipeline"), diagnostic.get("pipeline_generation"),
              viewport.get("device_generation"), viewport.get("command_generation"),
              viewport.get("recording_generation"), viewport.get("pipeline"), viewport.get("pipeline_generation"))
    return (all(uint(value, True) for value in values) and uint(flags) and flags & 3 == 3 and flags & ~15 == 0
            and type(diagnostic.get("status")) is int and diagnostic["status"] == 0
            and diagnostic["native_device"] == sample["device_handle_u64"]
            and sample["command_native_handle_u64"] == draw["native_command"]
            and diagnostic["device_generation"] == draw["native_device_generation"] == viewport["device_generation"]
            and diagnostic["command_generation"] == draw["native_command_allocation_generation"] == viewport["command_generation"]
            and diagnostic["recording_generation"] == draw["native_command_recording_generation"] == viewport["recording_generation"]
            and diagnostic["pipeline"] == sample["vertex_pipeline_handle_u64"] == viewport["pipeline"]
            and diagnostic["pipeline_generation"] == viewport["pipeline_generation"])


class SnapshotValidator:
    def __init__(self) -> None:
        self.checks = 0
        self.counts: Counter[str] = Counter()
        self.distributions: dict[str, Counter[str]] = {}
        self.coverage: Counter[str] = Counter()
        self.joined: dict[str, dict[str, Any]] = {}
        self.dependency_joins: dict[str, list[dict[str, Any]]] = {}

    def require(self, value: bool, message: str) -> None:
        self.checks += 1
        if not value:
            raise ValueError(message)

    def distribution(self, name: str, value: Any) -> None:
        self.distributions.setdefault(name, Counter())[str(value)] += 1

    def finite_json(self, value: Any) -> bool:
        if isinstance(value, float):
            return math.isfinite(value)
        if isinstance(value, dict):
            return all(self.finite_json(item) for item in value.values())
        if isinstance(value, list):
            return all(self.finite_json(item) for item in value)
        return True

    def array(self, value: dict[str, Any], items: str, count: str, present: str, truncated: str, limit: int, path: str) -> list[Any]:
        array = value.get(items)
        size = value.get(count)
        pointer = value.get(present)
        cut = value.get(truncated)
        self.require(isinstance(array, list) and uint(size) and type(pointer) is bool and type(cut) is bool,
                     f"{path}: invalid bounded array metadata")
        self.require(len(array) == (min(size, limit) if pointer else 0), f"{path}: array length disagrees with declared count/pointer")
        self.require(cut == (size > limit or bool(size and not pointer)), f"{path}: incorrect truncated declaration")
        if cut:
            self.coverage["bounded_array_truncated"] += 1
        return array

    def chain(self, value: Any, path: str) -> list[dict[str, Any]]:
        self.require(isinstance(value, dict), f"{path}: missing chain")
        nodes, types, cut = value.get("nodes"), value.get("types"), value.get("truncated")
        self.require(isinstance(nodes, list) and isinstance(types, list) and type(cut) is bool and len(nodes) == len(types) <= MAX_CHAIN,
                     f"{path}: invalid chain boundary")
        self.require(not cut or len(nodes) == MAX_CHAIN, f"{path}: truncated chain did not reach bound")
        if cut:
            self.coverage["chain_truncated"] += 1
        for index, node in enumerate(nodes):
            self.require(isinstance(node, dict) and uint(node.get("sType")) and node["sType"] == types[index], f"{path}: chain type mismatch")
            if node["sType"] == LIBRARIES:
                self.array(node, "libraries", "library_count", "libraries_present", "libraries_truncated", MAX_DEPENDENCIES, path)
            elif node["sType"] == RENDERING:
                self.array(node, "color_formats", "color_attachment_count", "color_formats_present", "color_formats_truncated", MAX_FORMATS, path)
        return nodes

    def shape(self, value: Any, path: str) -> None:
        self.require(isinstance(value, dict) and value.get("scope") == "raw_native_create_info_only"
                     and value.get("changes_qualification") is False, f"{path}: incorrect raw diagnostic scope")
        self.require(self.finite_json(value), f"{path}: nonfinite number was not serialized as null")
        self.chain(value.get("pnext"), path + ".pnext")
        stages = self.array(value, "stages", "stage_count", "stages_present", "stages_truncated", MAX_STAGES, path)
        for index, stage in enumerate(stages):
            self.require(isinstance(stage, dict), f"{path}: malformed stage")
            self.chain(stage.get("pnext"), f"{path}.stages[{index}]")
        for state_name in STATE_FIELDS:
            state = value.get(state_name)
            self.require(isinstance(state, dict) and type(state.get("present")) is bool, f"{path}.{state_name}: missing state presence")
            if not state["present"]:
                continue
            self.chain(state.get("pnext"), f"{path}.{state_name}.pnext")
            if state_name == "viewport_state":
                self.array(state, "viewports", "viewport_count", "viewports_present", "viewports_truncated", 1, path)
                self.array(state, "scissors", "scissor_count", "scissors_present", "scissors_truncated", 1, path)
            elif state_name == "dynamic_state":
                self.array(state, "states", "dynamic_state_count", "states_present", "states_truncated", MAX_DYNAMIC, path)

    def records(self, cache: Any) -> dict[str, Any]:
        self.require(isinstance(cache, dict) and cache.get("scope") == "native_create_observer_only", "layer diagnostic cache missing or incorrectly scoped")
        records = cache.get("records")
        self.require(isinstance(records, dict) and len(records) <= MAX_RECORDS, "selected record cache exceeds bound or is missing")
        for field in ("capacity", "selected_capacity", "history_retained", "selected_retained", "created", "evictions", "selected_evictions", "missing"):
            self.require(uint(cache.get(field)), f"cache.{field}: expected nonnegative observed integer")
        self.require(cache["history_retained"] <= cache["capacity"] and len(records) == cache["selected_retained"] <= cache["selected_capacity"], "cache retained counts disagree with records/capacities")
        for field in ("evictions", "selected_evictions", "missing"):
            if cache[field]:
                self.coverage["cache_" + field] += cache[field]
        for key, record in records.items():
            self.require(isinstance(record, dict) and all(uint(item, True) for item in record_identity(record)), f"record {key}: malformed identity")
            self.require(key == identity_key(*record_identity(record)), f"record {key}: key and full identity differ")
            self.require(record.get("scope") == "raw_native_create_and_tracker_observation_only", f"record {key}: invalid observation scope")
            self.require(type(record.get("creation_accepted")) is bool and type(record.get("is_library")) is bool and uint(record.get("creation_rejection")), f"record {key}: malformed creation declaration")
            self.require(record["creation_accepted"] == (record["creation_rejection"] == 0), f"record {key}: accepted/rejected declarations conflict")
            self.require(record.get("payload_hint_scope") == "post_validation_scan_not_exact_rejection_branch", f"record {key}: payload hint misdeclares an exact rejection branch")
            self.shape(record.get("create_info"), f"record {key}.create_info")
            dependencies, count, cut = record.get("dependencies_at_tracker_commit"), record.get("dependency_count"), record.get("dependencies_truncated")
            self.require(isinstance(dependencies, list) and uint(count) and type(cut) is bool and len(dependencies) == min(count, MAX_DEPENDENCIES), f"record {key}: dependency snapshot boundary invalid")
            self.require(cut == (count > MAX_DEPENDENCIES), f"record {key}: dependency truncation declaration invalid")
            if cut:
                self.coverage["dependencies_truncated"] += 1
        return records

    def join(self, key: Any, expected: tuple[int, int, int, int], records: dict[str, Any], role: str) -> dict[str, Any] | None:
        expected_key = identity_key(*expected) if all(uint(item, True) for item in expected) else None
        self.require(key == expected_key, f"{role}: declared record key disagrees with complete identity")
        if expected_key is None:
            self.coverage[role + "_identity_unavailable"] += 1
            return None
        record = records.get(expected_key)
        if record is None:
            self.coverage[role + "_record_missing"] += 1
            return None
        self.require(record_identity(record) == expected, f"{role}: joined record identity mismatch")
        self.joined[expected_key] = record
        return record

    def dependencies(self, records: dict[str, Any]) -> None:
        pending = list(self.joined)
        visited: set[str] = set()
        while pending:
            key = pending.pop()
            if key in visited:
                continue
            visited.add(key)
            self.require(len(visited) <= MAX_RECORDS, "dependency closure exceeded selected-record bound")
            source = self.joined[key]
            summary = []
            for dependency in source["dependencies_at_tracker_commit"]:
                self.require(isinstance(dependency, dict) and type(dependency.get("observed")) is bool
                             and type(dependency.get("valid")) is bool and type(dependency.get("is_library")) is bool,
                             f"record {key}: malformed dependency observation")
                pipeline, generation = dependency.get("pipeline"), dependency.get("pipeline_generation")
                self.require(uint(pipeline) and uint(generation), f"record {key}: malformed dependency identity")
                entry = {"pipeline": pipeline, "pipeline_generation": generation, "observed_at_tracker_commit": dependency["observed"]}
                if not dependency["observed"]:
                    self.require(generation == 0, f"record {key}: unobserved dependency claims a generation")
                    entry["join"] = "unobserved_at_commit"
                    self.coverage["dependency_unobserved_at_commit"] += 1
                else:
                    self.require(pipeline > 0 and generation > 0, f"record {key}: observed dependency lacks full identity")
                    identity = (source["native_device"], source["device_generation"], pipeline, generation)
                    target_key = identity_key(*identity)
                    target = self.join(target_key, identity, records, "dependency")
                    entry.update({"creation_record_key": target_key, "join": "known_missing" if target is None else "joined"})
                    if target is not None:
                        self.require(target["creation_accepted"] == dependency["valid"] and target["is_library"] == dependency["is_library"]
                                     and target.get("library_subset_flags") == dependency.get("subset_flags")
                                     and target["creation_rejection"] == dependency.get("creation_rejection"), f"record {key}: dependency snapshot disagrees with joined immutable generation")
                        pending.append(target_key)
                    self.counts["dependency_snapshots_observed"] += 1
                summary.append(entry)
            self.dependency_joins[key] = summary

    def run(self, uniform: Any, layer: Any, require_coverage: bool) -> None:
        self.require(isinstance(uniform, dict) and isinstance(layer, dict), "snapshot documents must be objects")
        self.require(uint(uniform.get("pid"), True) and uniform["pid"] == layer.get("pid"), "snapshot PID identities differ or are missing")
        records = self.records(layer.get("graphics_pipeline_diagnostics"))
        samples = uniform.get("samples")
        self.require(isinstance(samples, list) and 0 < len(samples) <= MAX_SAMPLES, "native sample list missing, empty or exceeds retained bound")
        self.counts["samples"] = len(samples)
        for index, sample in enumerate(samples):
            self.require(isinstance(sample, dict), f"sample {index}: expected object")
            draw = sample.get("draw_recording", {})
            viewport = draw.get("pipeline_viewport", {})
            diagnostic = viewport.get("diagnostic")
            if diagnostic is None:
                self.coverage["sample_diagnostic_absent"] += 1
                continue
            self.require(isinstance(diagnostic, dict) and type(diagnostic.get("matched_descriptor_recording")) is bool
                         and diagnostic.get("observer_only") is True, f"sample {index}: malformed diagnostic declarations")
            self.require(all(uint(diagnostic.get(field)) for field in (*IDENTITY_FIELDS, "command_generation", "recording_generation", "status", "current_creation_rejection")),
                         f"sample {index}: diagnostic native fields must be observed integers")
            matched = computed_recording_match(sample, diagnostic)
            self.require(diagnostic["matched_descriptor_recording"] == matched, f"sample {index}: false matched_descriptor_recording declaration")
            flags = diagnostic.get("flags")
            self.require(uint(flags) and flags & ~15 == 0 and type(diagnostic.get("current_creation_observed")) is bool
                         and diagnostic["current_creation_observed"] == bool(flags & 8), f"sample {index}: diagnostic flags/current creation declaration conflict")
            self.counts["diagnostic_samples"] += 1
            self.counts["matched_descriptor_recordings" if matched else "unmatched_descriptor_recordings"] += 1
            identity = tuple(diagnostic.get(field, 0) for field in IDENTITY_FIELDS)
            current = self.join(diagnostic.get("creation_record_key"), identity, records, "current")
            if current is not None:
                if diagnostic["current_creation_observed"]:
                    self.require(current["creation_rejection"] == diagnostic.get("current_creation_rejection")
                                 and normalized_hint(current.get("payload_hint_index")) == normalized_hint(diagnostic.get("payload_hint_index")), f"sample {index}: current creation metadata differs from immutable record")
                if matched:
                    self.counts["current_joined_samples"] += 1
                    self.distribution("current_creation_rejection_samples", current["creation_rejection"])
                    self.distribution("current_payload_issue_hint_samples", current.get("payload_issue_hint"))
            any_poison = False
            poison_values = []
            for role in ("first_poison", "last_poison"):
                poison = diagnostic.get(role)
                self.require(isinstance(poison, dict) and uint(poison.get("event")) and poison["event"] <= 6
                             and uint(poison.get("sequence")) and uint(poison.get("rejection_reason")), f"sample {index}: malformed {role} event")
                poison_values.append(poison)
                if poison["event"] == 0:
                    self.require(poison.get("creation_record_key") is None and poison.get("pipeline") == 0
                                 and poison.get("pipeline_generation") == 0 and poison["sequence"] == 0
                                 and poison["rejection_reason"] == 0 and normalized_hint(poison.get("payload_hint_index")) is None,
                                 f"sample {index}: empty poison declares source metadata")
                    continue
                self.counts["poison_events"] += 1
                self.require(poison["sequence"] > 0, f"sample {index}: observed poison has zero sequence")
                poison_identity = (identity[0], identity[1], poison.get("pipeline", 0), poison.get("pipeline_generation", 0))
                joined = self.join(poison.get("creation_record_key"), poison_identity, records, role)
                if joined is not None and poison["event"] == 3:
                    self.require(poison.get("rejection_reason") == joined["creation_rejection"], f"sample {index}: rejected-pipeline poison disagrees with its source creation")
                if matched:
                    self.distribution(role + "_event_reason_samples", f"{poison['event']}:{poison.get('rejection_reason')}")
                    if joined is not None:
                        any_poison = True
                        self.counts[role + "_joined_samples"] += 1
                        self.distribution(role + "_source_creation_rejection_samples", joined["creation_rejection"])
                        self.distribution(role + "_source_payload_issue_hint_samples", joined.get("payload_issue_hint"))
                        if poison_identity != identity:
                            self.counts[role + "_differs_from_current_samples"] += 1
            self.require(bool(poison_values[0]["event"]) == bool(poison_values[1]["event"])
                         and poison_values[0]["sequence"] <= poison_values[1]["sequence"], f"sample {index}: poison event chronology invalid")
            if any_poison:
                self.counts["poison_joined_samples"] += 1
        self.dependencies(records)
        for record in self.joined.values():
            shape = record["create_info"]
            for stage in shape["stages"]:
                for node in stage["pnext"]["nodes"]:
                    if node["sType"] == MODULE_CREATE:
                        self.counts["joined_inline_shader_module_nodes"] += 1
                    elif node["sType"] == MODULE_IDENTIFIER:
                        self.counts["joined_module_identifier_nodes"] += 1
            raster = shape["rasterization_state"]
            if raster["present"]:
                self.counts["joined_raster_depth_clip_nodes"] += sum(node["sType"] == DEPTH_CLIP for node in raster["pnext"]["nodes"])
        self.counts["joined_unique_records"] = len(self.joined)
        self.require(self.counts["diagnostic_samples"] > 0, "no native pipeline diagnostic objects retained")
        if require_coverage:
            self.require(self.counts["matched_descriptor_recordings"] > 0, "no matched native descriptor recording diagnostic")
            self.require(self.counts["current_joined_samples"] > 0, "no current pipeline creation joined by full identity")
            if self.counts["poison_events"]:
                self.require(self.counts["poison_joined_samples"] > 0, "no poison source pipeline creation joined by full identity")


def validate_native_submit_observer(layer: Any) -> dict[str, Any]:
    observer = layer.get("native_submit_observer") if isinstance(layer, dict) else None
    if observer is None:
        return {"checked_this_run": False, "available": False, "reason": "recording_has_no_native_submit_observer"}
    checks = 0
    failures: list[str] = []
    counts: Counter[str] = Counter()

    def require(value: bool, message: str) -> None:
        nonlocal checks
        checks += 1
        if not value:
            raise ValueError(message)

    try:
        require(isinstance(observer, dict) and observer.get("scope") == "raw_native_submit_observer_only", "native submit observer scope is invalid")
        require(observer.get("capacity") == MAX_RECORDS, "native submit observer capacity changed")
        records = observer.get("records")
        idle = observer.get("idle_records")
        require(isinstance(records, list) and isinstance(idle, list) and len(records) <= MAX_RECORDS and len(idle) <= MAX_RECORDS,
                "native submit observer history exceeds bound")
        require(uint(observer.get("evictions")) and uint(observer.get("latest_attempt_serial")) and uint(observer.get("latest_return_serial")),
                "native submit observer counters are malformed")
        serials: list[tuple[int, int]] = []
        devices: set[int] = set()
        for index, record in enumerate(records):
            require(isinstance(record, dict), f"native submit record {index} is not an object")
            require(record.get("kind") in ("vkQueueSubmit", "vkQueueSubmit2", "vkQueueSubmit2KHR"), f"native submit record {index} has unknown kind")
            require(uint(record.get("device"), True) and uint(record.get("queue"), True) and uint(record.get("fence")), f"native submit record {index} identity is invalid")
            require(uint(record.get("attempt_serial"), True) and uint(record.get("return_serial"), True) and record["attempt_serial"] < record["return_serial"],
                    f"native submit record {index} serial order is invalid")
            require(isinstance(record.get("native_result"), int) and not isinstance(record.get("native_result"), bool), f"native submit record {index} result is invalid")
            require(type(record.get("input_valid")) is bool and type(record.get("truncated")) is bool and uint(record.get("submit_count")), f"native submit record {index} metadata is invalid")
            submits = record.get("submits")
            require(isinstance(submits, list) and len(submits) == min(record["submit_count"], MAX_SUBMIT_ELEMENTS), f"native submit record {index} batch bound is invalid")
            for batch, submit in enumerate(submits):
                require(isinstance(submit, dict), f"native submit record {index} batch {batch} is not an object")
                for count_name, values_name in (("command_buffer_count", "command_buffers"), ("wait_semaphore_count", "wait_semaphores"), ("signal_semaphore_count", "signal_semaphores")):
                    require(uint(submit.get(count_name)) and isinstance(submit.get(values_name), list) and len(submit[values_name]) == min(submit[count_name], MAX_SUBMIT_ELEMENTS),
                            f"native submit record {index} batch {batch} {values_name} bound is invalid")
                    require(all(uint(item) for item in submit[values_name]), f"native submit record {index} batch {batch} {values_name} identity is invalid")
                if submit["command_buffer_count"] > MAX_SUBMIT_ELEMENTS or submit["wait_semaphore_count"] > MAX_SUBMIT_ELEMENTS or submit["signal_semaphore_count"] > MAX_SUBMIT_ELEMENTS:
                    require(record["truncated"], f"native submit record {index} omitted truncation declaration")
            if not record["input_valid"]:
                require(record["submit_count"] == 0 or not submits, f"native submit record {index} invalid input retained elements")
            serials.append((record["attempt_serial"], record["return_serial"]))
            devices.add(record["device"])
        for index, record in enumerate(idle):
            require(isinstance(record, dict) and record.get("kind") in ("vkQueueWaitIdle", "vkDeviceWaitIdle"), f"native idle record {index} kind is invalid")
            require(uint(record.get("object"), True) and uint(record.get("attempt_serial"), True) and uint(record.get("return_serial"), True)
                    and record["attempt_serial"] < record["return_serial"] and isinstance(record.get("native_result"), int)
                    and not isinstance(record.get("native_result"), bool), f"native idle record {index} is malformed")
            serials.append((record["attempt_serial"], record["return_serial"]))
        require(all(attempt < returned for attempt, returned in serials), "native submit observer contains an invalid serial pair")
        layer_devices_value = layer.get("devices", [])
        layer_devices_iter = layer_devices_value.values() if isinstance(layer_devices_value, dict) else layer_devices_value
        layer_devices = {device.get("native_device") for device in layer_devices_iter if isinstance(device, dict) and uint(device.get("native_device"), True)}
        matching = devices & layer_devices
        require(bool(records) and bool(matching), "native submit observer has no device identity overlap with layer state")
        counts.update({"submit_records": len(records), "idle_records": len(idle), "matching_devices": len(matching), "serial_pairs": len(serials), "evictions": observer["evictions"]})
    except (ValueError, KeyError, TypeError, AttributeError) as error:
        failures.append(str(error))
    return {"checked_this_run": True, "available": True, "passed": not failures, "checks": checks, "failures": failures,
            "counts": dict(counts), "gpu_completion_verified": False, "gpu_frame_aligned": False, "main_view_verified": False, "game_fg_ready": False}


def validate_snapshot(uniform: Any, layer: Any, require_coverage: bool = True) -> dict[str, Any]:
    validator = SnapshotValidator()
    failures = []
    try:
        validator.run(uniform, layer, require_coverage)
    except (ValueError, KeyError, TypeError, AttributeError) as error:
        failures.append(str(error))
    return {"passed": not failures, "checks": validator.checks, "failures": failures,
            "counts": dict(validator.counts), "coverage_lost": bool(validator.coverage), "coverage_losses": dict(validator.coverage),
            "distributions": {name: dict(values) for name, values in validator.distributions.items()},
            "payload_hint_semantics": "post_validation_scan_not_exact_rejection_branch",
            "dependency_snapshot_joins": validator.dependency_joins,
            "joined_record_keys": sorted(validator.joined), "samples_are_observer_records_not_gpu_frames": True,
            "gpu_frame_aligned": False, "main_view_verified": False, "game_fg_ready": False}


def main() -> int:
    parser = argparse.ArgumentParser(description="Join archived pipeline diagnostics by complete native generation identities.")
    parser.add_argument("--directory", required=True, type=Path)
    options = parser.parse_args()
    directory = options.directory.resolve() if options.directory.is_absolute() else (ROOT / options.directory).resolve()
    if not directory.is_relative_to(RESULTS.resolve()) or not directory.is_dir():
        parser.error("Evidence directory must exist under InputProbe/results")
    hashes = {}
    report = {"author": __author__, "schema_version": 1, "scope": "native_pipeline_diagnostic_join_observer_only",
              "passed": False, "game_started_by_validator": False, "registry_modified_by_validator": False,
              "gpu_frame_aligned": False, "main_view_verified": False, "game_fg_ready": False,
              "directory": str(directory), "input_sha256": hashes, "failures": []}

    def read(relative: str) -> Any:
        path = directory / relative
        data = path.read_bytes()
        hashes[relative] = hashlib.sha256(data).hexdigest()
        return json.loads(data.decode("utf-8-sig"))

    try:
        collection_layer = read("mapped_layer_collection_observation.json")
        report["collection"] = validate_snapshot(read("NativeUniform/collection_observation.json"), collection_layer)
        report["passed"] = report["collection"]["passed"]
        report["failures"].extend(report["collection"]["failures"])
        report["native_submit_observer"] = {"collection": validate_native_submit_observer(collection_layer)}
        if report["native_submit_observer"]["collection"].get("available"):
            report["passed"] &= report["native_submit_observer"]["collection"]["passed"]
            report["failures"].extend(report["native_submit_observer"]["collection"]["failures"])
        final_files = ("NativeUniform/final_observation.json", "mapped_layer_final_observation.json")
        present = [name for name in final_files if (directory / name).is_file()]
        if len(present) == 2:
            final_layer = read(final_files[1])
            report["final_historical"] = validate_snapshot(read(final_files[0]), final_layer, require_coverage=False)
            report["native_submit_observer"]["final_historical"] = validate_native_submit_observer(final_layer)
            if report["native_submit_observer"]["final_historical"].get("available"):
                report["passed"] &= report["native_submit_observer"]["final_historical"]["passed"]
                report["failures"].extend(report["native_submit_observer"]["final_historical"]["failures"])
            report["passed"] &= report["final_historical"]["passed"]
            report["failures"].extend(report["final_historical"]["failures"])
        else:
            report["final_historical"] = {"available": False, "partial_files": present, "combined_with_collection": False}
        for relative, expected in hashes.items():
            if hashlib.sha256((directory / relative).read_bytes()).hexdigest() != expected:
                raise ValueError(f"Input changed during validation: {relative}")
    except (OSError, ValueError, KeyError, TypeError) as error:
        report["passed"] = False
        report["failures"].append(str(error))
    output = directory / "pipeline_diagnostic_validation.json"
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(output)
    print(json.dumps({"passed": report["passed"], "collection_counts": report.get("collection", {}).get("counts", {}),
                      "failures": report["failures"]}, ensure_ascii=False))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
