import collections
import copy
import hashlib
import json
import pathlib
import struct
import sys


AUTHOR = "Anokianx"
DIRECTORY = pathlib.Path(__file__).resolve().parent
SHADERS = DIRECTORY.parent / "NativeShaders"
sys.path.insert(0, str(SHADERS))
from Build_Native_Uniform_Profiles import raw_combinations


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def raw_metadata_and_reads(bytecode):
    require(len(bytecode) >= 8 and len(bytecode) % 4 == 0, "raw shader length")
    words = struct.unpack(f"<{len(bytecode) // 4}I", bytecode)
    require(words[0] == 0xFFFE0300 and words[-1] == 0xFFFF, "raw VS3 version/END")
    constants = []
    reads = []
    projection = []
    position = 1
    while position < len(words):
        token = words[position]
        opcode = token & 0xFFFF
        if opcode == 0xFFFF:
            require(position + 1 == len(words), "trailing shader words")
            break
        if opcode == 0xFFFE:
            count = token >> 16 & 0x7FFF
            require(count > 0 and position + count + 1 <= len(words), "raw comment length")
            comment = bytecode[(position + 1) * 4:(position + 1 + count) * 4]
            if comment[:4] == b"CTAB":
                table = comment[4:]
                require(len(table) >= 28, "raw CTAB header")
                size, creator, version, count_entries, offset, flags, target = struct.unpack_from("<7I", table)
                require(size == 28 and version == words[0] and offset + count_entries * 20 <= len(table), "raw CTAB directory")
                for index in range(count_entries):
                    name, register_set, first, count_registers, reserved, type_offset, default = struct.unpack_from("<IHHHHII", table, offset + index * 20)
                    require(name < len(table) and type_offset + 16 <= len(table), "raw CTAB entry offsets")
                    end = table.find(b"\0", name)
                    require(end >= name, "raw CTAB name")
                    parameter_class, parameter_type, rows, columns, elements, members, member_info = struct.unpack_from("<6HI", table, type_offset)
                    constants.append({"name": table[name:end].decode("ascii"), "register_set": {0: "bool", 1: "int4", 2: "float4", 3: "sampler"}[register_set], "register_index": first, "register_count": count_registers, "rows": rows, "columns": columns, "elements": elements, "parameter_class": parameter_class, "parameter_type": parameter_type, "struct_members": members})
            position += count + 1
            continue
        count = token >> 24 & 15
        require(position + count + 1 <= len(words), "raw instruction interval")
        operands = words[position + 1:position + count + 1]
        if opcode not in (31, 47, 48, 81) and operands:
            for ordinal, parameter in enumerate(operands[1:], 1):
                register_type = (parameter >> 28 & 7) | (parameter >> 8 & 24)
                if parameter & 0x80000000 and register_type == 2:
                    register = parameter & 0x7FF
                    relative = bool(parameter & 0x2000)
                    if 58 <= register <= 216:
                        if relative:
                            require(ordinal + 1 < len(operands), "missing relative-address operand")
                            address = operands[ordinal + 1]
                            require((address & 0x7FF) == 0 and ((address >> 28 & 7) | (address >> 8 & 24)) == 3, "unsupported model address register")
                        reads.append({"base_register": register, "relative": relative, "opcode": opcode, "shader_word_offset": position})
                    if opcode == 9 and register in (8, 9, 10, 11):
                        projection.append(register)
        position += count + 1
    return constants, reads, projection


def context():
    source_bytes = (SHADERS / "native-uniform-profiles.json").read_bytes()
    source = json.loads(source_bytes)
    inventory_bytes = (SHADERS / "native_shader_inventory.json").read_bytes()
    inventory = json.loads(inventory_bytes)
    game = pathlib.Path(inventory["game"])
    files = {origin["file"] for profile in source["profiles"] for origin in profile["native_vcs_exact_origins"]}
    bytecodes = {}
    for filename in files:
        data = (game / filename).read_bytes()
        expected = next(item["sha256"] for item in inventory["files"] if item["path"] == filename)
        require(sha(data) == expected, "source VCS hash changed")
        for static_id, dynamic_id, bytecode in raw_combinations(data):
            bytecodes[(filename, static_id, dynamic_id)] = bytecode
    return {"source": source, "source_hash": sha(source_bytes), "inventory_hash": sha(inventory_bytes), "bytecodes": bytecodes}


def validate(report, inputs, check_files=True):
    source = inputs["source"]
    require(report["author"] == AUTHOR and report["schema_version"] == 1, "report author/schema")
    require(report["source_profile_sha256"] == inputs["source_hash"] and report["source_inventory_sha256"] == inputs["inventory_hash"], "report source identity")
    require(report["source_reflection_sha256"] == source["reflection_sha256"], "report reflection identity")
    require(not report["native_motion_vectors_available"] and not report["game_frame_generation_ready"], "unsupported native MV claim")
    source_profiles = {item["shader_sha256"]: item for item in source["profiles"]}
    require(len(report["profiles"]) == len(source_profiles) == 26, "profile coverage")
    require({item["shader_sha256"] for item in report["profiles"]} == set(source_profiles), "profile identities")
    checked_origins = 0
    checked_reads = 0
    for profile in report["profiles"]:
        original = source_profiles[profile["shader_sha256"]]
        for flag in ("native_previous_model_or_bones_observed", "native_per_pixel_motion_vectors_available", "entity_identity_verified", "main_view_and_frame_semantics_verified"):
            require(profile[flag] is False, "unsupported geometry readiness claim")
        require(profile["descriptor_set"] == original["descriptor_set"] == 2 and profile["binding"] == original["binding"] == 0 and profile["stage"] == "vertex", "descriptor/stage")
        require(profile["camera"] == {key: original[key] for key in ("relative_byte_offset", "byte_length", "evidence")}, "camera source data")
        require(profile["camera"]["relative_byte_offset"] == 384 and profile["camera"]["byte_length"] == 64, "camera byte range")
        model = profile["model"]
        ctab = model["ctab"]
        count = ctab["register_count"]
        require(ctab["register_index"] == model["register_first"] == 58 and count == model["register_count"] and count in (3, 159), "model register bounds")
        require((ctab["rows"], ctab["columns"], ctab["elements"], ctab["parameter_class"], ctab["parameter_type"]) == (4, 3, 53, 3, 3), "model CTAB layout")
        require(model["relative_byte_offset"] == 256 + 58 * 16 == 1184 and model["byte_length"] == count * 16, "model byte range")
        require(model["matrix_count_in_referenced_range"] == count // 3 and model["matrix_stride_bytes"] == 48 and model["storage_vectors_per_matrix"] == 3 and model["storage_vector_components"] == 4, "model matrix extent")
        require(model["register_last_inclusive"] == 58 + count - 1 and 58 + count <= 256, "model bank bounds")
        require(profile["classification"] == ("skinned_current_palette" if count == 159 else "single_current_model_matrix"), "model classification")
        require(len(profile["native_vcs_exact_origins"]) == len(original["native_vcs_exact_origins"]), "origin coverage")
        assembly_path = DIRECTORY / profile["assembly_file"]
        if check_files:
            require(sha(pathlib.Path(profile["spirv_file"]).read_bytes()) == profile["shader_sha256"], "SPIR-V payload hash")
            require(sha(assembly_path.read_bytes()) == profile["assembly_sha256"], "assembly evidence hash")
        for observed, expected in zip(profile["native_vcs_exact_origins"], original["native_vcs_exact_origins"]):
            require({key: observed[key] for key in expected} == expected, "native origin identity")
            bytecode = inputs["bytecodes"][(expected["file"], expected["static_id"], expected["dynamic_id"])]
            require(sha(bytecode) == expected["bytecode_sha256"] and hashlib.sha1(bytecode).hexdigest() == expected["raw_vcs_sha1"], "raw native bytecode hash")
            constants, reads, projection = raw_metadata_and_reads(bytecode)
            model_entries = [entry for entry in constants if entry["name"] == "cModel"]
            camera_entries = [entry for entry in constants if entry["name"] == "cViewProj"]
            require(model_entries == observed["c_model_ctab"] == [ctab] and camera_entries == expected["c_viewproj_ctab"], "independent raw CTAB model/camera")
            observed_reads = [(entry["base_register"], bool(entry["relative_address"])) for entry in profile["assembly_evidence"]["executable_model_reads"]]
            require(observed_reads == [(entry["base_register"], entry["relative"]) for entry in reads], "independent executable register operands")
            require(all(entry["relative"] == (count == 159) for entry in reads) and {entry["base_register"] for entry in reads} == {58, 59, 60}, "native dynamic palette classification")
            require(sorted(set(projection)) == [8, 9, 10, 11], "native projection DP4 operands")
            require(all(entry["opcode"] in (1, 4, 5, 8, 9) for entry in reads), "native model arithmetic operations")
            checked_reads += len(reads)
            checked_origins += 1
    summary = dict(collections.Counter(profile["classification"] for profile in report["profiles"]))
    require(report["summary"] == summary == {"skinned_current_palette": 9, "single_current_model_matrix": 17}, "profile summary")
    return {"profiles": 26, "native_origins": checked_origins, "raw_executable_model_operands": checked_reads, "summary": summary}


def main():
    path = DIRECTORY / "geometry-profiles.json"
    payload = path.read_bytes()
    report = json.loads(payload)
    inputs = context()
    checks = validate(report, inputs)
    mutations = {
        "descriptor_offset_shift": lambda value: value["profiles"][0]["model"].__setitem__("relative_byte_offset", 928),
        "declared_elements_overread": lambda value: value["profiles"][2]["model"].__setitem__("byte_length", 53 * 48),
        "truncated_bone_palette": lambda value: value["profiles"][0]["model"].__setitem__("register_count", 3),
        "camera_matrix_shift": lambda value: value["profiles"][0]["camera"].__setitem__("relative_byte_offset", 128),
        "source_hash_mutation": lambda value: value.__setitem__("source_profile_sha256", "0" * 64),
        "ctab_only_model_claim": lambda value: value["profiles"][0]["assembly_evidence"].__setitem__("executable_model_reads", []),
        "native_motion_claim": lambda value: value.__setitem__("native_motion_vectors_available", True),
        "entity_identity_claim": lambda value: value["profiles"][0].__setitem__("entity_identity_verified", True),
    }
    rejected = []
    for name, mutate in mutations.items():
        changed = copy.deepcopy(report)
        mutate(changed)
        try:
            validate(changed, inputs, check_files=False)
        except ValueError:
            rejected.append(name)
        else:
            raise ValueError("corrupt fixture was accepted: " + name)
    result = {"schema_version": 1, "author": AUTHOR, "status": "passed", "profile_file": str(path), "profile_sha256": sha(payload), **checks, "rejected_corruption_cases": rejected, "source_files_modified": False, "native_motion_vectors_available": False, "game_frame_generation_ready": False}
    (DIRECTORY / "geometry-validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
