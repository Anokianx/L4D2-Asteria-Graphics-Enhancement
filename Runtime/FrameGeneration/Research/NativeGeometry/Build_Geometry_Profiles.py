import argparse
import collections
import ctypes
import hashlib
import json
import os
import pathlib
import re
import struct
import sys


AUTHOR = "Anokianx"
DIRECTORY = pathlib.Path(__file__).resolve().parent
SHADERS = DIRECTORY.parent / "NativeShaders"
sys.path.insert(0, str(SHADERS))
from Build_Native_Uniform_Profiles import raw_combinations
from Scan_Native_Shaders import require, shader_metadata


def digest(data):
    return hashlib.sha256(data).hexdigest()


def disassemble(bytecode):
    path = pathlib.Path(os.environ["SystemRoot"]) / "System32/d3dx9_43.dll"
    library = ctypes.WinDLL(str(path))
    function = library.D3DXDisassembleShader
    function.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    function.restype = ctypes.c_long
    result = ctypes.c_void_p()
    require(function(ctypes.create_string_buffer(bytecode), 0, None, ctypes.byref(result)) >= 0 and result.value, "native D3DX disassembly failed")
    table = ctypes.cast(result, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(table[3])
    size = ctypes.WINFUNCTYPE(ctypes.c_uint, ctypes.c_void_p)(table[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_uint, ctypes.c_void_p)(table[2])
    try:
        text = ctypes.string_at(pointer(result), size(result)).rstrip(b"\0").decode("ascii")
    finally:
        release(result)
    return [line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith("//")]


def assembly_usage(instructions, model):
    model_start = model["register_index"]
    model_end = model_start + model["register_count"]
    references = []
    declarations = []
    texture_instructions = []
    projection = []
    flex = []
    for index, line in enumerate(instructions):
        operation, _, operands = line.partition(" ")
        if operation.startswith("dcl"):
            declarations.append(line)
            continue
        if operation in ("def", "defi", "defb", "vs_3_0"):
            continue
        source = operands.partition(",")[2]
        for match in re.finditer(r"(?<![A-Za-z0-9_])c(\d+)(\[[^]]+\])?", source):
            register = int(match[1])
            if model_start <= register < model_end:
                references.append({"instruction_index": index, "instruction": line, "base_register": register, "relative_address": match[2]})
            if register in (8, 9, 10, 11) and operation == "dp4":
                projection.append({"instruction_index": index, "instruction": line, "register": register})
            if register == 13:
                flex.append({"instruction_index": index, "instruction": line})
        if operation.startswith("tex"):
            texture_instructions.append({"instruction_index": index, "instruction": line})
    require(references and sorted({entry["register"] for entry in projection}) == [8, 9, 10, 11], "model or projection is not used by executable instructions")
    relative = any(entry["relative_address"] for entry in references)
    require(relative == (model["register_count"] == 159), "model array range and executable relative addressing disagree")
    require(sorted({entry["base_register"] for entry in references}) == [58, 59, 60], "unexpected model register addressing")
    require(any(line == "dcl_position o0" for line in declarations), "unexpected clip output declaration")
    start = min(entry["instruction_index"] for entry in references)
    end = max(entry["instruction_index"] for entry in projection)
    require(start < end and any(re.fullmatch(r"(?:mov|mad|add) o0(?:\.[xyzw]+)?, .+", line) for line in instructions), "native model transform is not followed by projection/output")
    return {
        "executable_model_reads": references,
        "executable_projection_dp4": projection,
        "relative_model_addressing": relative,
        "direct_model_base_registers": sorted({entry["base_register"] for entry in references}),
        "clip_output": "o0",
        "model_to_projection_instruction_window": {"first": start, "last": end, "instructions": instructions[start:end + 1]},
        "vertex_declarations": declarations,
        "flex_scale_reads": flex,
        "vertex_texture_instructions": texture_instructions,
        "evidence_scope": "Executable D3D9 register reads and model-to-projection instruction window; not a complete branch/path or entity-lifetime proof.",
    }


def build():
    profile_path = SHADERS / "native-uniform-profiles.json"
    profile_bytes = profile_path.read_bytes()
    source = json.loads(profile_bytes)
    inventory_path = SHADERS / "native_shader_inventory.json"
    inventory_bytes = inventory_path.read_bytes()
    inventory = json.loads(inventory_bytes)
    reflection_path = SHADERS / "spirv-input-reflection.json"
    reflection_bytes = reflection_path.read_bytes()
    require(digest(reflection_bytes) == source["reflection_sha256"], "original SPIR-V reflection changed")
    reflection = {item["sha256"]: item for item in json.loads(reflection_bytes)["records"]}
    game = pathlib.Path(inventory["game"])
    wanted = collections.defaultdict(set)
    for item in source["profiles"]:
        for origin in item["native_vcs_exact_origins"]:
            wanted[origin["file"]].add((origin["static_id"], origin["dynamic_id"]))
    extracted = {}
    for filename, coordinates in wanted.items():
        data = (game / filename).read_bytes()
        record = next(item for item in inventory["files"] if item["path"] == filename)
        require(digest(data) == record["sha256"], "native VCS source changed")
        for static_id, dynamic_id, bytecode in raw_combinations(data):
            if (static_id, dynamic_id) in coordinates:
                extracted[(filename, static_id, dynamic_id)] = bytecode
    result = []
    assembly_directory = DIRECTORY / "Disassembly"
    assembly_directory.mkdir(exist_ok=True)
    for original in source["profiles"]:
        require(digest(pathlib.Path(original["spirv_file"]).read_bytes()) == original["shader_sha256"], "captured SPIR-V source changed")
        origins = []
        models = []
        instructions = None
        for origin in original["native_vcs_exact_origins"]:
            bytecode = extracted[(origin["file"], origin["static_id"], origin["dynamic_id"])]
            require(digest(bytecode) == origin["bytecode_sha256"] and hashlib.sha1(bytecode).hexdigest() == origin["raw_vcs_sha1"], "native bytecode identity changed")
            constants, count, _, version = shader_metadata(bytecode)
            require(count == 1 and version == 0xFFFE0300, "expected one native VS3 CTAB")
            model = [entry for entry in constants if entry["name"] == "cModel"]
            camera = [entry for entry in constants if entry["name"] == "cViewProj"]
            require(len(model) == 1 and camera == origin["c_viewproj_ctab"], "native model/camera metadata mismatch")
            require(model[0]["register_set"] == "float4" and model[0]["register_index"] == 58 and model[0]["register_count"] in (3, 159), "unsupported model constant range")
            require((model[0]["rows"], model[0]["columns"], model[0]["elements"], model[0]["parameter_class"]) == (4, 3, 53, 3), "unsupported native matrix declaration")
            require(model[0]["parameter_type"] == 3 and model[0]["struct_members"] == 0, "unsupported native model type")
            assembly = disassemble(bytecode)
            require(instructions is None or instructions == assembly, "same translated profile maps to different native executable instructions")
            instructions = assembly
            models.append(model[0])
            origins.append({**origin, "c_model_ctab": model, "geometry_adjacent_ctab": [entry for entry in constants if re.search(r"flex|morph|ellipsoid|misc", entry["name"], re.I)]})
        require(models and all(model == models[0] for model in models), "native aliases disagree about model range")
        model = models[0]
        candidate = next(candidate for candidate in reflection[original["shader_sha256"]]["dxvk_hardware_vertex_profile_candidates"] if candidate["descriptor_set"] == original["descriptor_set"] and candidate["binding"] == original["binding"])
        require(candidate["c8_c11_indices_directly_used_by_dot"] == [8, 9, 10, 11], "translated projection usage changed")
        usage = assembly_usage(instructions, model)
        asm_file = assembly_directory / (original["shader_sha256"] + ".asm.txt")
        asm_bytes = ("\n".join(instructions) + "\n").encode("ascii")
        asm_file.write_bytes(asm_bytes)
        relative_offset = original["evidence"]["float_member_offset"] + model["register_index"] * original["evidence"]["array_stride"]
        length = model["register_count"] * 16
        result.append({
            "shader_sha256": original["shader_sha256"],
            "spirv_file": original["spirv_file"],
            "descriptor_set": original["descriptor_set"],
            "binding": original["binding"],
            "stage": "vertex",
            "native_vcs_exact_origins": origins,
            "camera": {key: original[key] for key in ("relative_byte_offset", "byte_length", "evidence")},
            "model": {
                "ctab": model,
                "relative_byte_offset": relative_offset,
                "byte_length": length,
                "register_first": model["register_index"],
                "register_count": model["register_count"],
                "register_last_inclusive": model["register_index"] + model["register_count"] - 1,
                "matrix_count_in_referenced_range": model["register_count"] // 3,
                "matrix_stride_bytes": 48,
                "storage_vectors_per_matrix": 3,
                "storage_vector_components": 4,
                "shader_evaluation": "world_xyz = [dot(position_xyzw, cModel_row0), dot(position_xyzw, cModel_row1), dot(position_xyzw, cModel_row2)]; homogeneous world.w is supplied by the compiled path.",
                "declaration_layout": "CTAB MATRIX_COLUMNS float4x3[53], three float4 registers per active matrix; the executable DP4 rows are the operational storage layout.",
                "bounds_rule": "Use CTAB register_count, never declared elements*3 for an optimized single-matrix profile.",
            },
            "classification": "skinned_current_palette" if usage["relative_model_addressing"] else "single_current_model_matrix",
            "flex_compiled_reads_present": bool(usage["flex_scale_reads"]),
            "native_vertex_texture_reads_present": bool(usage["vertex_texture_instructions"]),
            "assembly_file": str(asm_file.relative_to(DIRECTORY)).replace("\\", "/"),
            "assembly_sha256": digest(asm_bytes),
            "assembly_evidence": usage,
            "native_previous_model_or_bones_observed": False,
            "native_per_pixel_motion_vectors_available": False,
            "entity_identity_verified": False,
            "main_view_and_frame_semantics_verified": False,
        })
    return {
        "schema_version": 1,
        "author": AUTHOR,
        "date": "2026-10-15",
        "source_profile_file": str(profile_path),
        "source_profile_sha256": digest(profile_bytes),
        "source_inventory_file": str(inventory_path),
        "source_inventory_sha256": digest(inventory_bytes),
        "source_reflection_file": str(reflection_path),
        "source_reflection_sha256": digest(reflection_bytes),
        "source_dxvk_commit": source["source_dxvk_commit"],
        "profiles": result,
        "summary": dict(collections.Counter(item["classification"] for item in result)),
        "native_model_data_semantics": "Only current engine-written model or bone-palette constants. No previous-frame palette or engine-provided motion-vector field is established.",
        "native_motion_vectors_available": False,
        "game_frame_generation_ready": False,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=pathlib.Path, default=DIRECTORY / "geometry-profiles.json")
    arguments = parser.parse_args()
    report = build()
    arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(arguments.output), "profiles": len(report["profiles"]), "summary": report["summary"], "bytes": arguments.output.stat().st_size}))


if __name__ == "__main__":
    main()
