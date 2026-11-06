import argparse
import collections
import hashlib
import json
import lzma
import pathlib
import re
import struct
import sys


AUTHOR = "Anokianx"
MAX_BLOCK_SIZE = 1 << 17
REGISTER_SETS = {0: "bool", 1: "int4", 2: "float4", 3: "sampler"}
KEYWORDS = {
    "history": re.compile(r"prev|previous|history|last.?frame|old.?world|old.?view", re.I),
    "current": re.compile(r"current|frame|time|delta", re.I),
    "motion": re.compile(r"velocity|motion|blur|flow|speed", re.I),
    "geometry": re.compile(r"bone|skin|world|view|proj|camera|eye", re.I),
    "deformation": re.compile(r"bone|skin|morph|flex|model", re.I),
}
ASSEMBLY_OPERANDS = {0: 0, 1: 2, 2: 3, 3: 3, 4: 4, 5: 3, 6: 2, 7: 2, 8: 3, 9: 3, 10: 3, 11: 3, 12: 3, 13: 3, 14: 2, 15: 2, 16: 2, 17: 3, 18: 4, 19: 2, 20: 3, 21: 3, 22: 3, 23: 3, 24: 3, 31: 2, 64: 1, 65: 1, 66: 1, 81: 5, 0xFFFD: 0}
FORMAT_REFERENCES = {
    "vcs_header": "https://github.com/ValveSoftware/source-sdk-2013/blob/be57e3d01ac7d7aa6a2cacbfef187c0b46616db0/src/public/materialsystem/shader_vcs_version.h",
    "constant_info": "https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxshader-constantinfo",
    "type_info": "https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxshader-typeinfo",
}


class ParseError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ParseError(message)


def unpack(data, position, fmt):
    require(position >= 0 and position + struct.calcsize(fmt) <= len(data), "record exceeds data bounds")
    return struct.unpack_from(fmt, data, position)


def c_string(data, position):
    require(0 <= position < len(data), "string offset exceeds constant table")
    end = data.find(b"\0", position, min(len(data), position + 4096))
    require(end >= position, "unterminated constant-table string")
    return data[position:end].decode("ascii", errors="strict")


def constant_table(data):
    size, creator, version, count, offset, flags, target = unpack(data, 0, "<7I")
    require(size == 28 and count <= 4096, "unsupported constant table header")
    require(offset + count * 20 <= len(data), "constant table entries exceed comment")
    result = []
    for index in range(count):
        name_offset, register_set, register_index, register_count, reserved, type_offset, default = unpack(data, offset + index * 20, "<IHHHHII")
        parameter_class, parameter_type, rows, columns, elements, members, member_info = unpack(data, type_offset, "<6HI")
        require(register_set in REGISTER_SETS, "unknown constant register set")
        result.append({
            "name": c_string(data, name_offset),
            "register_set": REGISTER_SETS[register_set],
            "register_index": register_index,
            "register_count": register_count,
            "rows": rows,
            "columns": columns,
            "elements": elements,
            "parameter_class": parameter_class,
            "parameter_type": parameter_type,
            "struct_members": members,
        })
    return result, version, c_string(data, target)


def shader_metadata(bytecode):
    require(len(bytecode) >= 8 and len(bytecode) % 4 == 0, "invalid DWORD shader bytecode size")
    words = struct.unpack(f"<{len(bytecode) // 4}I", bytecode)
    version = words[0]
    shader_kind = version >> 16
    major = version >> 8 & 255
    require(shader_kind in (0xFFFE, 0xFFFF), "unexpected shader version token")
    position = 1
    table_count = 0
    bindings = []
    output_registers = set()
    while position < len(words):
        token = words[position]
        opcode = token & 0xFFFF
        if opcode == 0xFFFF:
            require(position + 1 == len(words), "bytes after shader END")
            return bindings, table_count, sorted(output_registers), version
        if opcode == 0xFFFE:
            count = token >> 16 & 0x7FFF
            require(count > 0 and position + 1 + count <= len(words), "comment exceeds shader bytecode")
            start = (position + 1) * 4
            if bytecode[start:start + 4] == b"CTAB":
                entries, reflected_version, target = constant_table(bytecode[start + 4:start + count * 4])
                require(reflected_version == version, "CTAB and shader versions differ")
                bindings.extend(entries)
                table_count += 1
            position += count + 1
            continue
        if major >= 2:
            count = token >> 24 & 15
        else:
            require(opcode in ASSEMBLY_OPERANDS, "unsupported assembly shader opcode")
            count = ASSEMBLY_OPERANDS[opcode]
        require(position + 1 + count <= len(words), "instruction exceeds shader bytecode")
        if shader_kind == 0xFFFF and count and (1 <= opcode <= 24 or opcode in (32, 33, 34, 35, 36, 37, 46, 65, 66, 88, 90, 91, 92, 93, 94, 95)):
            destination = words[position + 1]
            register_type = (destination >> 28 & 7) | (destination >> 8 & 24)
            if destination & 0x80000000 and register_type == 8:
                output_registers.add(destination & 0x7FF)
        position += count + 1
    raise ParseError("shader lacks END token")


def decode_lzma(data):
    require(len(data) >= 17 and data[:4] == b"LZMA", "missing Valve LZMA header")
    raw_size, packed_size = unpack(data, 4, "<II")
    require(0 < raw_size <= MAX_BLOCK_SIZE and packed_size + 17 == len(data), "invalid LZMA block sizes")
    property_byte = data[12]
    require(property_byte < 225, "invalid LZMA properties")
    dictionary_size, = unpack(data, 13, "<I")
    require(0 < dictionary_size <= 1 << 26, "excessive LZMA dictionary size")
    filters = [{
        "id": lzma.FILTER_LZMA1,
        "dict_size": dictionary_size,
        "lc": property_byte % 9,
        "lp": property_byte // 9 % 5,
        "pb": property_byte // 45,
    }]
    decoder = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=filters)
    output = decoder.decompress(data[17:], max_length=raw_size)
    require(len(output) == raw_size and not decoder.unused_data, "LZMA unpacked size or trailing data mismatch")
    return output


def parse_vcs6(data):
    version, total, dynamic, flags, centroid, static_count, crc = unpack(data, 0, "<7I")
    require(version == 6 and dynamic > 0 and static_count >= 2, "unsupported VCS6 header")
    require(static_count <= 1_000_000, "excessive static record count")
    records = [unpack(data, 28 + index * 8, "<II") for index in range(static_count)]
    alias_position = 28 + static_count * 8
    alias_count, = unpack(data, alias_position, "<I")
    directory_end = alias_position + 4 + alias_count * 8
    require(directory_end <= len(data), "aliases exceed VCS file")
    require(records[-1][0] == 0xFFFFFFFF and records[-1][1] == len(data), "invalid VCS static sentinel")
    require(records[0][1] == directory_end, "unaccounted bytes between VCS directory and payload")
    static_ids = {identifier for identifier, offset in records[:-1]}
    require(len(static_ids) == static_count - 1, "duplicate static combo record")
    alias_ids = set()
    for index in range(alias_count):
        identifier, original = unpack(data, alias_position + 4 + index * 8, "<II")
        require(original in static_ids and identifier not in static_ids and identifier not in alias_ids, "invalid static alias")
        alias_ids.add(identifier)
    counters = collections.Counter()
    binding_counts = collections.Counter()
    bindings = {}
    output_sets = collections.Counter()
    versions = collections.Counter()
    for (static_id, start), (_, end) in zip(records, records[1:]):
        require(directory_end <= start < end <= len(data), "invalid static payload interval")
        position = start
        dynamic_ids = set()
        terminated = False
        while position < end:
            word, = unpack(data, position, "<I")
            position += 4
            if word == 0xFFFFFFFF:
                require(position == end, "trailing bytes after static block terminator")
                terminated = True
                break
            packed_size = word & 0x3FFFFFFF
            compression_kind = word >> 30
            require(compression_kind == 1 and position + packed_size <= end, "unsupported or truncated packed block")
            block = decode_lzma(data[position:position + packed_size])
            position += packed_size
            counters["compressed_blocks"] += 1
            counters["unpacked_bytes"] += len(block)
            cursor = 0
            while cursor < len(block):
                combo_id, byte_count = unpack(block, cursor, "<II")
                cursor += 8
                require(combo_id < dynamic and combo_id not in dynamic_ids, "invalid dynamic combo ID")
                require(cursor + byte_count <= len(block), "shader record crosses compressed block")
                entries, table_count, outputs, shader_version = shader_metadata(block[cursor:cursor + byte_count])
                cursor += byte_count
                dynamic_ids.add(combo_id)
                counters["unique_compiled_combinations"] += 1
                counters["combinations_with_ctab"] += bool(table_count)
                counters["ctab_tables"] += table_count
                versions[f"{shader_version:08x}"] += 1
                output_sets[",".join(str(value) for value in outputs)] += 1
                for entry in entries:
                    key = json.dumps(entry, sort_keys=True, separators=(",", ":"))
                    bindings[key] = entry
                    binding_counts[key] += 1
        require(terminated, "static payload lacks terminator")
    merged = []
    for key, value in sorted(bindings.items(), key=lambda item: (item[1]["name"], item[1]["register_set"], item[1]["register_index"], item[1]["register_count"])):
        merged.append({**value, "compiled_combinations": binding_counts[key]})
    return {
        "header": {"version": version, "logical_total_combinations": total, "dynamic_combinations": dynamic, "unique_static_combinations": static_count - 1, "static_aliases": alias_count, "flags": flags, "centroid_mask": centroid, "source_crc32": f"{crc:08x}"},
        "coverage": dict(counters),
        "shader_versions": dict(sorted(versions.items())),
        "pixel_color_output_register_sets": dict(sorted(output_sets.items())),
        "bindings": merged,
    }


def parse_vcs4(data):
    version, total, dynamic, flags, centroid, reference_size, crc = unpack(data, 0, "<7I")
    require(version == 4 and total > 0 and reference_size == 0, "unsupported VCS4 header or diff reference")
    require(total <= 4096 and 28 + total * 8 <= len(data), "invalid VCS4 combination directory")
    bindings = {}
    versions = collections.Counter()
    counters = collections.Counter()
    previous_end = 28 + total * 8
    for index in range(total):
        offset, size = unpack(data, 28 + index * 8, "<II")
        require(offset == previous_end and offset + size <= len(data), "invalid VCS4 shader interval")
        entries, table_count, outputs, shader_version = shader_metadata(data[offset:offset + size])
        previous_end = offset + size
        counters["unique_compiled_combinations"] += 1
        counters["combinations_with_ctab"] += bool(table_count)
        counters["ctab_tables"] += table_count
        versions[f"{shader_version:08x}"] += 1
        for entry in entries:
            key = json.dumps(entry, sort_keys=True, separators=(",", ":"))
            bindings[key] = entry
    require(previous_end == len(data), "unaccounted VCS4 bytes")
    return {
        "header": {"version": version, "logical_total_combinations": total, "dynamic_combinations": dynamic, "diff_reference_bytes": reference_size, "flags": flags, "centroid_mask": centroid, "source_crc32": f"{crc:08x}"},
        "coverage": dict(counters),
        "shader_versions": dict(sorted(versions.items())),
        "pixel_color_output_register_sets": {},
        "bindings": sorted(bindings.values(), key=lambda item: item["name"]),
    }


def scan_file(path, root):
    data = path.read_bytes()
    version, = unpack(data, 0, "<I")
    result = {"path": path.relative_to(root).as_posix(), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "vcs_version": version}
    try:
        result.update(parse_vcs4(data) if version == 4 else parse_vcs6(data))
        result["parse_status"] = "complete"
        names = sorted({entry["name"] for entry in result["bindings"]})
        result["keyword_hits"] = {group: [name for name in names if expression.search(name)] for group, expression in KEYWORDS.items()}
    except (ParseError, lzma.LZMAError, UnicodeError, struct.error) as error:
        result.update({"parse_status": "error", "error": str(error)})
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", type=pathlib.Path, default=pathlib.Path(r"H:\SteamLibrary\steamapps\common\Left 4 Dead 2"))
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path(__file__).with_name("native_shader_inventory.json"))
    arguments = parser.parse_args()
    game = arguments.game.resolve(strict=True)
    require(not arguments.output.resolve().is_relative_to(game), "output must remain outside the game directory")
    source = game / "hl2" / "shaders"
    paths = sorted(source.rglob("*.vcs"))
    require(paths, "no official shader files found")
    records = []
    for index, path in enumerate(paths, 1):
        record = scan_file(path, game)
        records.append(record)
        if index % 25 == 0 or record["parse_status"] == "error":
            print(f"{index}/{len(paths)} {path.name}: {record['parse_status']}", flush=True)
    totals = collections.Counter()
    for record in records:
        totals["files"] += 1
        totals["file_bytes"] += record["bytes"]
        totals["files_complete"] += record["parse_status"] == "complete"
        totals["files_vcs4_without_ctab"] += record["vcs_version"] == 4 and not record.get("coverage", {}).get("ctab_tables")
        totals["files_error"] += record["parse_status"] == "error"
        totals.update(record.get("coverage", {}))
    excluded = []
    shader_directories = set(game.glob("*/shaders/*")) | set(game.glob("*/*/shaders/*"))
    for directory in sorted(shader_directories):
        if directory.is_dir() and not directory.is_relative_to(source):
            candidates = sorted(directory.rglob("*.vcs"))
            if candidates:
                excluded.append({"directory": directory.relative_to(game).as_posix(), "files": [path.relative_to(game).as_posix() for path in candidates], "reason": "outside inspected hl2 native shader path; provenance not assumed"})
    all_names = sorted({entry["name"] for record in records for entry in record.get("bindings", [])})
    report = {
        "schema_version": 1,
        "author": AUTHOR,
        "date": "2026-10-15",
        "game": str(game),
        "read_only_game_files": True,
        "shader_sources_installed_or_modified": False,
        "format_references": FORMAT_REFERENCES,
        "summary": dict(totals),
        "unique_constant_names": all_names,
        "keyword_name_hits": {group: [name for name in all_names if expression.search(name)] for group, expression in KEYWORDS.items()},
        "native_motion_vectors_verified": False,
        "limitations": ["Names and register outputs cannot prove pixel contents or runtime pass selection.", "Shader constants are optimized reflection records, not an exhaustive engine state inventory.", "Two VCS4 assembly shader files contain no CTAB names; their bytecode intervals and END tokens are validated.", "Only unique compiled static records are decompressed; static aliases are fully validated but not decoded twice.", "Valve raw LZMA blocks may omit an end-of-stream marker; decoding stops at the stored output length and validates every resulting combination record.", "No optical flow, shader modification, memory injection or game file replacement is performed."],
        "excluded_directories": excluded,
        "files": records,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    arguments.output.write_text(encoded, encoding="utf-8", newline="\n")
    current_transform_names = {"cModelViewProj", "cViewProj", "cModel", "cViewModel", "cProj"}
    selected_motion_names = {"BlurSampler", "FlowMapSampler", "FlowNoiseSampler", "FlowSampler", "FlowmapSampler", "SideSpeedMap", "g_flMaxMotionBlur", "g_tFlowSampler", "g_vBlurAmounts", "g_vFlowParams1", "g_vFlowParams2", "g_vInteriorColor_RefractBlur"}
    findings = {
        "author": AUTHOR,
        "date": report["date"],
        "inventory_file": arguments.output.name,
        "inventory_sha256": hashlib.sha256(arguments.output.read_bytes()).hexdigest(),
        "summary": report["summary"],
        "keyword_name_hits": report["keyword_name_hits"],
        "native_motion_vectors_verified": False,
        "native_camera_verified": False,
        "current_transform_bindings": [{"path": record["path"], "sha256": record["sha256"], "bindings": [entry for entry in record.get("bindings", []) if entry["name"] in current_transform_names]} for record in records if any(entry["name"] in current_transform_names for entry in record.get("bindings", []))],
        "motion_keyword_shaders": [{"path": record["path"], "sha256": record["sha256"], "keyword_hits": record.get("keyword_hits", {}).get("motion", []), "samplers": [entry for entry in record.get("bindings", []) if entry["register_set"] == "sampler"]} for record in records if any(entry["name"] in selected_motion_names for entry in record.get("bindings", []))],
        "multiple_color_output_shaders": [{"path": record["path"], "sha256": record["sha256"], "register_sets": record["pixel_color_output_register_sets"]} for record in records if any("," in key for key in record.get("pixel_color_output_register_sets", {}))],
        "excluded_directories": report["excluded_directories"],
        "limitations": report["limitations"],
    }
    arguments.output.with_name("native_shader_findings.json").write_text(json.dumps(findings, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(arguments.output), "summary": report["summary"], "keywords": report["keyword_name_hits"]}, ensure_ascii=False))
    return int(totals["files_error"] != 0)


if __name__ == "__main__":
    sys.exit(main())
