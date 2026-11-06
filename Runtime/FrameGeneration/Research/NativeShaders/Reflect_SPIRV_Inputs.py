import argparse
import collections
import hashlib
import json
import pathlib
import struct
import sys


AUTHOR = "Anokianx"
DIRECTORY = pathlib.Path(__file__).resolve().parent
GRAMMAR = DIRECTORY / "Reference/SPIRV-Headers-1.3.268.0/include/spirv/unified1/spirv.core.grammar.json"
MAX_BYTES = 4 << 20
MAX_BOUND = 1 << 24
STORAGE = {0: "UniformConstant", 1: "Input", 2: "Uniform", 3: "Output", 4: "Workgroup", 5: "CrossWorkgroup", 6: "Private", 7: "Function", 8: "Generic", 9: "PushConstant", 10: "AtomicCounter", 11: "Image", 12: "StorageBuffer"}
EXECUTION = {0: "Vertex", 1: "TessellationControl", 2: "TessellationEvaluation", 3: "Geometry", 4: "Fragment", 5: "GLCompute", 6: "Kernel"}
DECORATIONS = {2: "Block", 3: "BufferBlock", 4: "RowMajor", 5: "ColMajor", 6: "ArrayStride", 7: "MatrixStride", 11: "BuiltIn", 30: "Location", 31: "Component", 32: "Index", 33: "Binding", 34: "DescriptorSet", 35: "Offset"}


class ReflectionError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ReflectionError(message)


def literal_string(words, start):
    require(start < len(words), "missing literal string")
    data = struct.pack(f"<{len(words) - start}I", *words[start:])
    end = data.find(b"\0")
    require(end >= 0 and end < 65536, "unterminated or excessive literal string")
    consumed = end // 4 + 1
    require(not any(data[end + 1:consumed * 4]), "nonzero string padding")
    try:
        value = data[:end].decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise ReflectionError("invalid UTF-8 literal string") from error
    return value, start + consumed


def reflect(data):
    require(20 <= len(data) <= MAX_BYTES and len(data) % 4 == 0, "invalid SPIR-V byte length")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    magic, version, generator, bound, schema = words[:5]
    require(magic == 0x07230203, "not little-endian SPIR-V")
    require(version & 0xFF0000FF == 0 and version >> 16 == 1 and version >> 8 & 255 <= 6, "unsupported SPIR-V version")
    require(1 < bound <= MAX_BOUND and schema == 0, "invalid SPIR-V ID bound or schema")
    grammar = json.loads(GRAMMAR.read_text(encoding="utf-8"))
    instructions = {record["opcode"]: record for record in grammar["instructions"]}
    definitions = {}
    parsed = []
    position = 5
    while position < len(words):
        token = words[position]
        count, opcode = token >> 16, token & 0xFFFF
        require(count > 0 and position + count <= len(words), "zero-length or truncated SPIR-V instruction")
        require(opcode in instructions, f"opcode {opcode} is absent from pinned SPIR-V grammar")
        operands = words[position + 1:position + count]
        specification = instructions[opcode].get("operands", [])
        minimum = sum("quantifier" not in operand for operand in specification)
        require(len(operands) >= minimum, "instruction has too few operands")
        if specification and specification[0]["kind"] == "IdResult":
            result = operands[0]
        elif len(specification) > 1 and specification[0]["kind"] == "IdResultType" and specification[1]["kind"] == "IdResult":
            result = operands[1]
        else:
            result = None
        if result is not None:
            require(0 < result < bound and result not in definitions, "out-of-bound or duplicate result ID")
            definitions[result] = (opcode, operands)
        parsed.append((opcode, operands, position))
        position += count
    require(any(opcode == 14 for opcode, _, _ in parsed), "SPIR-V lacks memory model")
    names = {}
    strings = {}
    member_names = {}
    decorations = collections.defaultdict(dict)
    member_decorations = collections.defaultdict(dict)
    entry_points = []
    types = {}
    variables = {}
    constants = {}
    specialization_defaults = {}
    chains = {}
    copies = {}
    groups = []

    def check_id(identifier):
        require(0 < identifier < bound and identifier in definitions, "undefined or out-of-bound reflected ID")

    def decorate(destination, decoration, arguments):
        if decoration in (2, 3, 4, 5):
            require(not arguments, "flag decoration has unexpected operands")
        if decoration in (6, 7, 11, 30, 31, 32, 33, 34, 35):
            require(len(arguments) == 1, "numeric decoration has invalid arity")
        if decoration in destination:
            require(destination[decoration] == list(arguments), "conflicting reflected decoration")
        destination[decoration] = list(arguments)

    for opcode, operands, offset in parsed:
        if opcode in (5, 7):
            check_id(operands[0])
            name, end = literal_string(operands, 1)
            require(end == len(operands), "extra operands after name string")
            (strings if opcode == 7 else names)[operands[0]] = name
        elif opcode == 6:
            check_id(operands[0])
            name, end = literal_string(operands, 2)
            require(end == len(operands), "extra operands after member name")
            member_names[(operands[0], operands[1])] = name
        elif opcode == 15:
            check_id(operands[1])
            require(definitions[operands[1]][0] == 54, "entry point is not a function")
            name, end = literal_string(operands, 2)
            for identifier in operands[end:]:
                check_id(identifier)
                require(definitions[identifier][0] == 59, "entry-point interface is not a variable")
            entry_points.append({"execution_model": EXECUTION.get(operands[0], str(operands[0])), "function_id": operands[1], "name": name, "interface_ids": list(operands[end:])})
        elif 19 <= opcode <= 33:
            expected = {19: 1, 20: 1, 21: 3, 22: 2, 23: 3, 24: 3, 26: 1, 27: 2, 28: 3, 29: 2, 32: 3}
            if opcode in expected:
                require(len(operands) == expected[opcode], "type instruction has invalid arity")
            types[operands[0]] = {"opcode": opcode, "name": instructions[opcode]["opname"], "operands": list(operands[1:])}
        elif opcode in (41, 42, 43, 46, 48, 49, 50):
            type_id, identifier = operands[:2]
            check_id(type_id)
            scalar = types.get(type_id)
            if scalar and scalar["opcode"] in (20, 21):
                if scalar["opcode"] == 20:
                    value = int(opcode in (41, 48))
                else:
                    width, signed = scalar["operands"]
                    require(width in (8, 16, 32, 64), "unsupported integer width")
                    required = (width + 31) // 32
                    require(opcode == 46 or len(operands[2:]) == required, "integer constant word count mismatch")
                    value = 0 if opcode == 46 else sum(word << (32 * index) for index, word in enumerate(operands[2:]))
                    value &= (1 << width) - 1
                    if signed and value >> (width - 1):
                        value -= 1 << width
                (specialization_defaults if opcode in (48, 49, 50) else constants)[identifier] = value
        elif opcode == 59:
            require(len(operands) in (3, 4), "invalid OpVariable arity")
            check_id(operands[0])
            variables[operands[1]] = {"type_id": operands[0], "storage_class_id": operands[2]}
        elif opcode in (65, 66, 67, 70):
            check_id(operands[0])
            check_id(operands[2])
            for index in operands[3:]:
                check_id(index)
            chains[operands[1]] = {"type_id": operands[0], "base_id": operands[2], "index_ids": list(operands[3:]), "pointer_element_index": opcode in (67, 70), "word_offset": offset}
        elif opcode == 83:
            require(len(operands) == 3, "invalid OpCopyObject arity")
            check_id(operands[2])
            copies[operands[1]] = operands[2]
        elif opcode == 71:
            check_id(operands[0])
            decorate(decorations[operands[0]], operands[1], operands[2:])
        elif opcode == 72:
            check_id(operands[0])
            decorate(member_decorations[(operands[0], operands[1])], operands[2], operands[3:])
        elif opcode in (74, 75):
            groups.append((opcode, operands))
    require(entry_points, "SPIR-V lacks entry point")
    for identifier, member in member_names.keys() | member_decorations.keys():
        require(identifier in types and types[identifier]["opcode"] == 30 and member < len(types[identifier]["operands"]), "invalid struct member metadata")
    for opcode, operands in groups:
        check_id(operands[0])
        if opcode == 74:
            targets = [(identifier, None) for identifier in operands[1:]]
        else:
            require(len(operands[1:]) % 2 == 0, "invalid group member target pairs")
            targets = list(zip(operands[1::2], operands[2::2]))
        for identifier, member in targets:
            check_id(identifier)
            target = decorations[identifier] if member is None else member_decorations[(identifier, member)]
            for decoration, arguments in decorations[operands[0]].items():
                decorate(target, decoration, arguments)

    def value_of(mapping, key):
        return mapping.get(key, [None])[0]

    def describe_type(identifier, depth=0):
        require(depth <= 16 and identifier in types, "recursive or missing reflected type")
        record = types[identifier]
        operands = record["operands"]
        result = {"id": identifier, "kind": record["name"], "name": names.get(identifier, "")}
        if record["opcode"] in (21, 22):
            result["width"] = operands[0]
            if record["opcode"] == 21:
                result["signed"] = bool(operands[1])
        elif record["opcode"] in (23, 24):
            result.update({"element_type": describe_type(operands[0], depth + 1), "elements": operands[1]})
        elif record["opcode"] in (28, 29):
            result.update({"element_type": describe_type(operands[0], depth + 1), "array_stride": value_of(decorations[identifier], 6)})
            if record["opcode"] == 28:
                result["elements"] = constants.get(operands[1])
                result["length_id"] = operands[1]
        elif record["opcode"] == 30:
            members = []
            for member, type_id in enumerate(operands):
                attributes = member_decorations[(identifier, member)]
                members.append({"index": member, "name": member_names.get((identifier, member), ""), "offset": value_of(attributes, 35), "location": value_of(attributes, 30), "builtin": value_of(attributes, 11), "matrix_stride": value_of(attributes, 7), "row_major": 4 in attributes, "column_major": 5 in attributes, "type": describe_type(type_id, depth + 1)})
            result.update({"block": 2 in decorations[identifier], "buffer_block": 3 in decorations[identifier], "members": members})
        elif record["opcode"] == 32:
            result.update({"storage_class_id": operands[0], "pointee": describe_type(operands[1], depth + 1)})
        return result

    descriptors = []
    interfaces = []
    for identifier, variable in sorted(variables.items()):
        storage = variable["storage_class_id"]
        attributes = decorations[identifier]
        if storage not in (0, 1, 2, 3, 9, 12):
            continue
        pointer = types.get(variable["type_id"])
        require(pointer and pointer["opcode"] == 32 and pointer["operands"][0] == storage, "variable pointer/storage mismatch")
        record = {"id": identifier, "name": names.get(identifier, ""), "storage_class": STORAGE.get(storage, str(storage)), "storage_class_id": storage, "descriptor_set": value_of(attributes, 34), "binding": value_of(attributes, 33), "location": value_of(attributes, 30), "component": value_of(attributes, 31), "index": value_of(attributes, 32), "builtin": value_of(attributes, 11), "type": describe_type(variable["type_id"])}
        (interfaces if storage in (1, 3) else descriptors).append(record)

    def trace_root(identifier, visited=None):
        visited = set() if visited is None else visited
        require(identifier not in visited and len(visited) <= 16, "cyclic or excessive pointer provenance")
        visited.add(identifier)
        if identifier in variables:
            return identifier, [], False
        if identifier in copies:
            return trace_root(copies[identifier], visited)
        if identifier in chains:
            record = chains[identifier]
            root, indices, unsupported = trace_root(record["base_id"], visited)
            return root, indices + record["index_ids"], unsupported or record["pointer_element_index"]
        return None, [], True

    def byte_offset(root, indices, unsupported):
        if unsupported or any(identifier not in constants for identifier in indices):
            return None
        pointer = types.get(variables[root]["type_id"])
        if not pointer or pointer["opcode"] != 32:
            return None
        type_id = pointer["operands"][1]
        offset = 0
        for identifier in indices:
            index = constants[identifier]
            if index < 0 or type_id not in types:
                return None
            record = types[type_id]
            operands = record["operands"]
            if record["opcode"] == 30:
                if index >= len(operands):
                    return None
                member_offset = value_of(member_decorations[(type_id, index)], 35)
                if member_offset is None:
                    return None
                offset += member_offset
                type_id = operands[index]
            elif record["opcode"] in (28, 29):
                stride = value_of(decorations[type_id], 6)
                if stride is None or record["opcode"] == 28 and (operands[1] not in constants or index >= constants[operands[1]]):
                    return None
                offset += index * stride
                type_id = operands[0]
            elif record["opcode"] == 23:
                element = types.get(operands[0])
                if index >= operands[1] or not element or element["opcode"] not in (21, 22) or element["operands"][0] % 8:
                    return None
                offset += index * (element["operands"][0] // 8)
                type_id = operands[0]
            else:
                return None
        return offset

    access_records = []
    loads_by_pointer = collections.defaultdict(list)
    dots_by_source = collections.defaultdict(list)
    for opcode, operands, offset in parsed:
        if opcode == 61:
            loads_by_pointer[operands[2]].append(operands[1])
        elif opcode == 148:
            require(len(operands) == 4, "invalid OpDot arity")
            dots_by_source[operands[2]].append(operands[1])
            dots_by_source[operands[3]].append(operands[1])
    for identifier, chain in sorted(chains.items()):
        root, indices, unsupported = trace_root(identifier)
        if root is None or variables[root]["storage_class_id"] not in (0, 2, 9, 12):
            continue
        load_results = loads_by_pointer[identifier]
        access_records.append({"result_id": identifier, "root_variable_id": root, "root_variable_name": names.get(root, ""), "word_offset": chain["word_offset"], "index_ids": indices, "constant_indices": [constants.get(index) for index in indices], "specialization_default_indices": [specialization_defaults.get(index) for index in indices], "relative_byte_offset_from_descriptor_range": byte_offset(root, indices, unsupported), "pointer_element_index_not_resolved": unsupported, "direct_load_result_ids": load_results, "direct_dot_result_ids": sorted({dot for load in load_results for dot in dots_by_source[load]})})
    hardware_float_blocks = []
    for descriptor in descriptors:
        pointer = descriptor["type"]
        block = pointer.get("pointee", {})
        members = block.get("members", [])
        if descriptor["storage_class_id"] != 2 or not block.get("block") or len(members) != 2:
            continue
        integer_member, float_member = members
        integer_array, float_array = integer_member["type"], float_member["type"]
        integer_vector, float_vector = integer_array.get("element_type", {}), float_array.get("element_type", {})
        integer_scalar, float_scalar = integer_vector.get("element_type", {}), float_vector.get("element_type", {})
        matches = integer_member["offset"] == 0 and float_member["offset"] == 256 and integer_array.get("array_stride") == 16 and float_array.get("array_stride") == 16 and integer_array.get("elements") == 16 and float_array.get("elements") == 256 and integer_vector.get("elements") == 4 and float_vector.get("elements") == 4 and integer_scalar.get("kind") == "OpTypeInt" and integer_scalar.get("width") == 32 and float_scalar.get("kind") == "OpTypeFloat" and float_scalar.get("width") == 32
        if not matches:
            continue
        accesses = [record for record in access_records if record["root_variable_id"] == descriptor["id"] and len(record["constant_indices"]) == 2 and record["constant_indices"][0] == 1 and record["constant_indices"][1] in range(8, 12)]
        hardware_float_blocks.append({"variable_id": descriptor["id"], "descriptor_set": descriptor["descriptor_set"], "binding": descriptor["binding"], "float_member_offset": 256, "array_stride": 16, "c8_c11_relative_offsets": [384, 400, 416, 432], "c8_c11_indices_observed": sorted({record["constant_indices"][1] for record in accesses}), "c8_c11_indices_directly_used_by_dot": sorted({record["constant_indices"][1] for record in accesses if record["direct_dot_result_ids"]}), "vertex_entry_point_present": any(entry["execution_model"] == "Vertex" for entry in entry_points), "profile_match_only": True, "native_camera_verified": False})
    return {
        "spirv_version": f"{version >> 16}.{version >> 8 & 255}",
        "generator_word": generator,
        "id_bound": bound,
        "instruction_count": len(parsed),
        "entry_points": entry_points,
        "debug_strings": sorted(strings.values()),
        "descriptors_and_push_constants": descriptors,
        "input_output_locations": interfaces,
        "uniform_access_chain_hints": access_records,
        "dxvk_hardware_vertex_profile_candidates": hardware_float_blocks,
        "validation_scope": "Header, bounded instruction stream, known grammar opcodes/result IDs and reflected metadata consistency. This is not full SPIR-V semantic validation.",
        "buffer_bytes_read": False,
        "native_camera_verified": False,
        "native_motion_vectors_verified": False,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=pathlib.Path, default=DIRECTORY.parents[1] / "InputProbe/results/GameNativeProvenance/Provenance/Shaders")
    parser.add_argument("--output", type=pathlib.Path, default=DIRECTORY / "spirv-input-reflection.json")
    arguments = parser.parse_args()
    require(arguments.output.resolve().is_relative_to(DIRECTORY), "reflection output must remain within the research directory")
    paths = sorted(set(arguments.input.glob("*.bin")) | set(arguments.input.glob("*.spv"))) if arguments.input.is_dir() else [arguments.input] if arguments.input.is_file() else []
    require(len(paths) <= 4096, "shader file limit exceeded")
    records = []
    for path in paths:
        record = {"file": str(path), "bytes": path.stat().st_size}
        try:
            require(record["bytes"] <= MAX_BYTES, "shader size limit exceeded")
            data = path.read_bytes()
            record["sha256"] = hashlib.sha256(data).hexdigest()
            record.update(reflect(data))
            record["status"] = "reflected"
        except (ValueError, UnicodeError, struct.error, IndexError) as error:
            record.update({"status": "rejected", "reason": str(error)})
        records.append(record)
    report = {"author": AUTHOR, "date": "2026-10-15", "read_only": True, "input": str(arguments.input), "source_available": bool(paths), "summary": dict(collections.Counter(record["status"] for record in records)), "records": records, "native_camera_verified": False, "native_motion_vectors_verified": False}
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(arguments.output), "source_available": bool(paths), "summary": report["summary"]}))
    return int(any(record["status"] == "rejected" for record in records))


if __name__ == "__main__":
    sys.exit(main())
