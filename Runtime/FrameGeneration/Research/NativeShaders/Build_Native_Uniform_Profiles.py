import argparse
import collections
import hashlib
import json
import pathlib
import re
import struct

from Scan_Native_Shaders import decode_lzma, require, shader_metadata


AUTHOR = "Anokianx"
DIRECTORY = pathlib.Path(__file__).resolve().parent
NATIVE_FAMILIES = re.compile(r"(infected|phong|vertexlit_and_unlit_generic(?:_bump)?)_vs(?:20|30)\.vcs")


def raw_combinations(data):
    version, total, dynamic, flags, centroid, static_count, crc = struct.unpack_from("<7I", data)
    require(version == 6 and static_count > 1, "source must be validated VCS6")
    records = [struct.unpack_from("<II", data, 28 + index * 8) for index in range(static_count)]
    for (static_id, start), (_, end) in zip(records, records[1:]):
        require(0 < start < end <= len(data), "invalid validated static interval")
        position = start
        while position < end:
            require(position + 4 <= end, "missing static block header")
            word, = struct.unpack_from("<I", data, position)
            position += 4
            if word == 0xFFFFFFFF:
                require(position == end, "bytes after static terminator")
                break
            packed_size = word & 0x3FFFFFFF
            require(word >> 30 == 1 and position + packed_size <= end, "invalid compressed block")
            block = decode_lzma(data[position:position + packed_size])
            position += packed_size
            cursor = 0
            while cursor < len(block):
                require(cursor + 8 <= len(block), "truncated combo header")
                dynamic_id, size = struct.unpack_from("<II", block, cursor)
                cursor += 8
                require(dynamic_id < dynamic and cursor + size <= len(block), "invalid combo interval")
                bytecode = block[cursor:cursor + size]
                cursor += size
                yield static_id, dynamic_id, bytecode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reflection", type=pathlib.Path, default=DIRECTORY / "spirv-input-reflection.json")
    arguments = parser.parse_args()
    reflection = json.loads(arguments.reflection.read_text(encoding="utf-8"))
    inventory = json.loads((DIRECTORY / "native_shader_inventory.json").read_text(encoding="utf-8"))
    reference = DIRECTORY / "Reference/DXVK-v2.3.1"
    manifest = json.loads((reference / "reference-manifest.json").read_text(encoding="utf-8"))
    for record in manifest["files"]:
        require(hashlib.sha256((reference / record["path"]).read_bytes()).hexdigest() == record["sha256"], "DXVK reference changed")
    shader_source = (reference / "src/d3d9/d3d9_shader.cpp").read_text(encoding="utf-8")
    require("Sha1Hash::compute(pShaderBytecode, info.bytecodeByteLength)" in shader_source, "source key hash implementation differs")
    wanted = {name: [] for record in reflection["records"] for name in record.get("debug_strings", []) if re.fullmatch(r"VS_[0-9a-f]{40}", name) and record.get("dxvk_hardware_vertex_profile_candidates")}
    game = pathlib.Path(inventory["game"])
    count = 0
    validated_files = 0
    for record in inventory["files"]:
        relative = pathlib.PurePosixPath(record["path"])
        if relative.parent.as_posix() != "hl2/shaders/fxc" or not re.search(r"_vs\d+\.vcs$", relative.name):
            continue
        require(record["parse_status"] == "complete", "native shader scan is incomplete")
        data = (game / record["path"]).read_bytes()
        require(hashlib.sha256(data).hexdigest() == record["sha256"], "native VCS changed after validated scan")
        validated_files += 1
        for static_id, dynamic_id, bytecode in raw_combinations(data):
            count += 1
            key = "VS_" + hashlib.sha1(bytecode).hexdigest()
            if key not in wanted:
                continue
            bindings, table_count, outputs, version = shader_metadata(bytecode)
            camera_bindings = [entry for entry in bindings if entry["name"] == "cViewProj" and entry["register_set"] == "float4" and entry["register_index"] == 8 and entry["register_count"] == 4 and entry["rows"] == 4 and entry["columns"] == 4]
            wanted[key].append({"file": record["path"], "file_sha256": record["sha256"], "static_id": static_id, "dynamic_id": dynamic_id, "bytecode_bytes": len(bytecode), "bytecode_sha256": hashlib.sha256(bytecode).hexdigest(), "raw_vcs_sha1": key[3:], "c_viewproj_ctab": camera_bindings})
    origins = {"author": AUTHOR, "date": "2026-10-15", "hash_form": "Exact full raw VCS D3D9 bytecode, including original CTAB/comments/END; no stripping or mutation.", "dxvk_source_commit": manifest["commit"], "validated_vertex_source_files": validated_files, "scanned_vertex_combinations": count, "matches": wanted}
    (DIRECTORY / "native-vs-exact-hash-matches.json").write_text(json.dumps(origins, indent=2) + "\n", encoding="utf-8", newline="\n")
    profiles = []
    for record in reflection["records"]:
        for candidate in record.get("dxvk_hardware_vertex_profile_candidates", []):
            if not candidate["vertex_entry_point_present"] or candidate["c8_c11_indices_directly_used_by_dot"] != [8, 9, 10, 11]:
                continue
            native = [origin for name in record["debug_strings"] for origin in wanted.get(name, []) if NATIVE_FAMILIES.fullmatch(pathlib.PurePosixPath(origin["file"]).name) and origin["c_viewproj_ctab"]]
            if not native:
                continue
            source = pathlib.Path(record["file"]).resolve(strict=True)
            require(hashlib.sha256(source.read_bytes()).hexdigest() == record["sha256"], "captured SPIR-V changed after reflection")
            profiles.append({"shader_sha256": record["sha256"], "spirv_file": str(source), "stage": "vertex", "descriptor_set": candidate["descriptor_set"], "binding": candidate["binding"], "relative_byte_offset": 384, "byte_length": 64, "source_dxvk_version": "2.3.1", "vertex_entry_point": next(entry["name"] for entry in record["entry_points"] if entry["execution_model"] == "Vertex"), "native_vcs_exact_origins": native, "evidence": {"float_member_offset": 256, "array_stride": 16, "register_indices": [8, 9, 10, 11], "direct_opdot_usage": True}, "profile_candidate": True, "camera_semantics_verified": False})
    report = {"schema_version": 1, "author": AUTHOR, "date": "2026-10-15", "profiles": profiles, "native_camera_ready": False, "camera_semantics_verified": False, "source_sha1_hash_form": origins["hash_form"], "source_dxvk_commit": manifest["commit"], "reflection_sha256": hashlib.sha256(arguments.reflection.read_bytes()).hexdigest(), "main_view_and_frame_semantics_verified": False}
    output = DIRECTORY / "native-uniform-profiles.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    families = collections.Counter(pathlib.PurePosixPath(profile["native_vcs_exact_origins"][0]["file"]).name for profile in profiles)
    print(json.dumps({"output": str(output), "profiles": len(profiles), "bytes": output.stat().st_size, "scanned_vertex_combinations": count, "exact_matching_source_keys": sum(bool(value) for value in wanted.values()), "families": dict(families)}))


if __name__ == "__main__":
    main()
