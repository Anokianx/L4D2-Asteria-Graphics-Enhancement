import argparse
import hashlib
import json
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))

import capstone
import pefile

AUTHOR = "Anokianx"
PINNED_SHA256 = "8a6d468059394bf4cfce32f11dcba12ef45de88da1373084d5a2916d0bf7ba86"


class OfflineImage:
    def __init__(self, path):
        self.path = pathlib.Path(path).resolve()
        self.data = self.path.read_bytes()
        self.pe = pefile.PE(data=self.data)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def offset(self, address):
        return self.pe.get_offset_from_rva(address - self.base)

    def address(self, offset):
        return self.base + self.pe.get_rva_from_offset(offset)

    def section(self, address):
        rva = address - self.base
        for section in self.pe.sections:
            if section.VirtualAddress <= rva < section.VirtualAddress + max(section.Misc_VirtualSize, section.SizeOfRawData):
                return section
        return None

    def executable(self, address):
        section = self.section(address)
        return bool(section and section.Characteristics & 0x20000000)

    def bytes(self, address, count):
        offset = self.offset(address)
        return self.data[offset:offset + count]

    def integer(self, address):
        return struct.unpack("<I", self.bytes(address, 4))[0]

    def references(self, address):
        pattern = struct.pack("<I", address)
        result = []
        cursor = 0
        while True:
            cursor = self.data.find(pattern, cursor)
            if cursor < 0:
                return result
            result.append(self.address(cursor))
            cursor += 1

    def string(self, address):
        offset = self.offset(address)
        return self.data[offset:self.data.index(b"\0", offset)].decode("ascii", errors="replace")

    def instructions(self, address, count):
        return list(self.disassembler.disasm(self.bytes(address, count), address))

    def disassembly(self, address, count):
        return [{"rva": hex(instruction.address - self.base), "bytes": instruction.bytes.hex(),
                 "mnemonic": instruction.mnemonic, "operands": instruction.op_str}
                for instruction in self.instructions(address, count) if instruction.mnemonic != "int3"]

    def function(self, rva, count):
        address = self.base + rva
        return {"rva": hex(rva), "bytes": self.bytes(address, count).hex(),
                "disassembly": self.disassembly(address, count)}

    def rtti(self, name):
        decorated = f".?AV{name}@@\0".encode("ascii")
        string_offset = self.data.index(decorated)
        descriptor = self.address(string_offset - 8)
        result = []
        for reference in self.references(descriptor):
            locator = reference - 12
            values = struct.unpack("<5I", self.bytes(locator, 20))
            if values[0] != 0 or values[1] > 0x1000 or values[2] != 0 or values[3] != descriptor:
                continue
            for candidate in self.references(locator):
                table = candidate + 4
                section = self.section(table)
                if not section or section.Characteristics & 0x80000000:
                    continue
                if not self.executable(self.integer(table)):
                    continue
                entries = []
                cursor = table
                while self.executable(self.integer(cursor)) and len(entries) < 250:
                    entries.append(self.integer(cursor) - self.base)
                    cursor += 4
                result.append({"type_descriptor_rva": hex(descriptor - self.base),
                               "complete_object_locator_rva": hex(locator - self.base),
                               "object_offset": values[1], "vtable_rva": hex(table - self.base),
                               "entries_rva": entries})
        return result

    def registration(self, name):
        address = self.address(self.data.index(name.encode("ascii") + b"\0"))
        result = []
        for reference in self.references(address):
            code = self.bytes(reference - 1, 21)
            if code[0] != 0x68 or code[5] != 0x68 or code[10] != 0xB9 or code[15] != 0xE8 or code[20] != 0xC3:
                continue
            creator = struct.unpack_from("<I", code, 6)[0]
            creator_code = self.bytes(creator, 6)
            if creator_code[0] != 0xB8 or creator_code[5] != 0xC3:
                continue
            object_address = struct.unpack_from("<I", creator_code, 1)[0]
            result.append({"registration_rva": hex(reference - 1 - self.base),
                           "interface_name_rva": hex(address - self.base),
                           "creator_rva": hex(creator - self.base),
                           "creator_bytes": creator_code.hex(),
                           "object_rva": hex(object_address - self.base),
                           "object_initial_vtable_rva": hex(self.integer(object_address) - self.base),
                           "registration_disassembly": self.disassembly(reference - 1, 21)})
        return result


def declared_methods(path, name):
    source = pathlib.Path(path).read_text(encoding="utf-8")
    source = source[source.index(f"class {name}"):]
    source = source[:source.index("};")]
    source = re.sub(r"//[^\n]*|/\*.*?\*/", "", source, flags=re.S)
    result = []
    for declaration in re.findall(r"virtual\s+([^;]+);", source):
        declaration = " ".join(declaration.split())
        name_match = re.search(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", declaration)
        result.append({"name": name_match[1], "declaration": declaration})
    return result


def run(engine, output):
    image = OfflineImage(engine)
    digest = hashlib.sha256(image.data).hexdigest()
    if image.pe.FILE_HEADER.Machine != 0x14C or digest != PINNED_SHA256:
        raise RuntimeError("This report's semantic assertions require the pinned Windows x86 engine.dll SHA256")
    client_rtti = image.rtti("CEngineClient")
    render_rtti = image.rtti("CRender")
    render_view_rtti = image.rtti("CVRenderView")
    client = next(item for item in client_rtti if item["object_offset"] == 0)
    render = next(item for item in render_rtti if item["object_offset"] == 0)
    render_view = next(item for item in render_view_rtti if item["object_offset"] == 0)
    references = ROOT.parent / "l4d2-sdk-reference" / "public"
    sdk_methods = declared_methods(references / "cdll_int.h", "IVEngineClient")
    methods = []
    directly_verified = {5, 19, 20, 26, 27, 29, 30, 31, 35, 36, 37, 38}
    for index, function_rva in enumerate(client["entries_rva"][:45]):
        if index == 29:
            name = "HideLoadingPlaque"
            sdk_index = None
        else:
            sdk_index = index if index < 29 else index - 1
            name = sdk_methods[sdk_index]["name"]
        methods.append({"slot": index, "name": name, "rva": hex(function_rva),
                        "sdk_source_index": sdk_index,
                        "mapping_evidence": "machine_code_or_named_string" if index in directly_verified else "ordered_alignment_only"})
    assert client["vtable_rva"] == "0x359dec"
    assert client["entries_rva"][36:39] == [0x5D610, 0x5D620, 0x5D630]
    assert render["entries_rva"][12:14] == [0xC3130, 0xC3160]
    world_view = image.function(0xC3130, 39)
    world_screen = image.function(0xC3160, 39)
    assert world_view["bytes"] == "83b9e8000000017e1a8b81e80000008b89dc00000069c0940100008d84083cffffffc38d411cc3"
    assert world_screen["bytes"] == "83b9e8000000017e178b81e80000008b89dc00000069c0940100008d4408bcc38d819c000000c3"
    for function in (world_view, world_screen):
        assert not any(row["mnemonic"].startswith("call") or row["mnemonic"] in {"lock", "loop", "loopne", "loope"}
                       for row in function["disassembly"])
    slots = {str(index): hex(client["entries_rva"][index]) for index in (5, 19, 26, 27, 36, 37, 38)}
    functions = {
        "CreateInterface_export": image.function(0x286180, 9),
        "CreateInterface_factory": image.function(0x286110, 109),
        "GetScreenSize_material_context_chain": image.function(0x60680, 78),
        "GetViewAngles": image.function(0x5D130, 46),
        "IsInGame": image.function(0x5F1A0, 15),
        "IsConnected": image.function(0x5F1B0, 15),
        "shared_client_state_accessor": image.function(0x82CA0, 9),
        "HideLoadingPlaque": image.function(0x5D360, 66),
        "GetGameDirectory_wrong_prior_matrix_slot": image.function(0x5D610, 6),
        "WorldToScreenMatrix_stub": image.function(0x5D620, 13),
        "WorldToViewMatrix_stub": image.function(0x5D630, 13),
        "CRender_world_to_screen_getter": world_screen,
        "CRender_world_to_view_getter": world_view,
        "CRender_constructor": image.function(0xC4AD0, 170),
        "CVRenderView_GetFieldOfView_actual_slot30": image.function(0x115EE0, 13),
        "CRender_GetFieldOfView_slot17": image.function(0xC4C20, 26),
        "CVRenderView_GetMatricesForView_actual_slot53": image.function(0x1161A0, 46),
        "pure_matrix_generator_consumes_retail_CViewSetup": image.function(0xC32E0, 314),
        "VMatrix_copy_16_scalar_floats": image.function(0x27CE80, 106),
        "matrix_multiply_4x4_row_major": image.function(0x27D4E0, 1025),
        "native_perspective_projection_constructor": image.function(0x27EA10, 158)
    }
    assert hashlib.sha256(pathlib.Path(engine).read_bytes()).hexdigest() == digest
    report = {
        "author": AUTHOR, "date": "2026-10-15", "analysis": "read_only_offline_pe_machine_code",
        "engine": {"path": str(image.path), "sha256": digest, "bytes": len(image.data),
                   "machine": "x86", "pe_timestamp": image.pe.FILE_HEADER.TimeDateStamp,
                   "pe_timestamp_hex": hex(image.pe.FILE_HEADER.TimeDateStamp),
                   "preferred_image_base": hex(image.base), "size_of_image": image.pe.OPTIONAL_HEADER.SizeOfImage,
                   "source_hash_unchanged_after_analysis": True},
        "engine_not_loaded_or_executed": True, "remote_process_memory_not_read": True,
        "sdk_reference": {"commit": "2a31cd007b2d7d2f964dc093eedcf7a812cf9dd6",
                          "engine_interface_name": "VEngineClient013", "retail_layout_matches_sdk": False},
        "retail_profile": {
            "interface": "VEngineClient013", "object_rva": "0x425504", "vtable_rva": client["vtable_rva"],
            "selected_method_rva": slots, "extra_method": {"slot": 29, "name": "HideLoadingPlaque"},
            "world_to_screen_slot": 37, "world_to_view_slot": 38,
            "prior_slot36_returns": "const char* game directory, not a VMatrix reference",
            "render_global_pointer_rva": "0x427bc0", "render_object_initial_rva": "0x601f00",
            "render_vtable_rva": render["vtable_rva"], "native_camera_ready": False,
            "camera_semantics_verified": False, "frame_timing_verified": False,
            "matrix_memory_layout": "16 contiguous float32 values in row-major 4x4 layout, established by engine copy and multiplication code; no live value validation",
            "matrix_layout_evidence": {
                "copy_function_rva": "0x27ce80", "copy_float_offsets": [hex(index * 4) for index in range(16)],
                "multiply_function_rva": "0x27d4e0", "output_float_offsets": [hex(index * 4) for index in range(16)],
                "first_output_element": "left[0]*right[0]+left[1]*right[4]+left[2]*right[8]+left[3]*right[12]",
                "generator_composition": "world_to_screen = view_to_projection * world_to_view"
            },
            "getter_operation": {
                "world_to_view": {"renderer_slot": 12, "getter_rva": "0xc3130", "default_offset": "0x1c", "stack_entry_offset": "0xd0"},
                "world_to_screen": {"renderer_slot": 13, "getter_rva": "0xc3160", "default_offset": "0x9c", "stack_entry_offset": "0x150"},
                "stack_count_offset": "0xe8", "stack_base_offset": "0xdc", "stack_entry_bytes": "0x194",
                "stack_selection": "count > 1 selects entry (count - 1); otherwise retained renderer matrix",
                "complete_getter_bodies_have_calls": False, "complete_getter_bodies_have_locks_or_loops": False,
                "complete_getter_bodies_write_memory": False,
                "concurrent_mutation_or_stale_view_excluded": False
            },
            "GetScreenSize_material_context_dynamic_calls": ["material system slot 96", "context slot 2", "context slot 103", "context slot 3", "context slot 1"],
            "GetScreenSize_safe_on_Present_thread_verified": False,
            "previous_stall_cause_proven": False,
            "recommended_next_diagnostic": "Use guarded retail slots37/38 only; backbuffer dimensions replace GetScreenSize; retain unverified frame semantics",
            "all_first45_methods": methods
        },
        "render_view_boundary": {
            "interface": "VEngineRenderView013", "object_rva": "0x428f6c", "vtable_rva": render_view["vtable_rva"],
            "sdk_slot28_actual_behavior": "returns current view entity integer, not float FOV",
            "GetFieldOfView_actual_slot": 30, "GetMatricesForView_actual_slot": 53,
            "GetMatricesForView_computes_from_supplied_view_instead_of_sampling_current_view": True,
            "CViewSetup_sdk_layout_matches_retail": False,
            "matrix_generator_read_offsets": {"width": "0x10", "height": "0x18", "ortho_flag": "0x20", "fov": "0x34",
                                             "origin": "0x3c", "angles": "0x48", "zNear": "0x54", "zFar": "0x58", "aspect": "0x64"},
            "near_far_direct_sampling_available": False,
            "projection_recovery_from_native_matrices": {
                "projection": "world_to_screen * inverse(world_to_view)",
                "verified_perspective_structure": "P[3][2]=-1; P[3][3]=0; P[2][2]=far/(near-far); P[2][3]=far*near/(near-far)",
                "near": "P[2][3]/P[2][2]", "far": "P[2][3]/(P[2][2]+1)",
                "horizontal_fov_radians": "2*atan(1/P[0][0])",
                "vertical_fov_radians": "2*atan(1/P[1][1])",
                "live_projection_recovery_verified": False,
                "requires": "stable same-frame native matrices; perspective-layout validation; finite valid near/far and FOV; main-view validation"
            },
            "next_step": "Validate native main-view matrices and frame boundary, then recover their projection mathematically; do not construct matrices from guessed SDK CViewSetup or arbitrary FOV"
        },
        "rtti": {"CEngineClient": client_rtti, "CRender": render_rtti, "CVRenderView": render_view_rtti},
        "interface_registrations": {"VEngineClient013": image.registration("VEngineClient013"),
                                    "VEngineRenderView013": image.registration("VEngineRenderView013")},
        "named_method_evidence": {"HideLoadingPlaque": [image.string(image.base + 0x35A170), image.string(image.base + 0x35A120)],
                                  "Sound_ExtraUpdate": image.string(image.base + 0x35A1A4)},
        "functions": functions,
        "limits": ["No game process was started, DLL loaded, function called, or remote memory inspected",
                   "No native motion-vector source is established by this camera ABI report",
                   "Matching ABI does not establish which render view is present at the observer callback",
                   "Pure getters can still expose concurrently changing or stale matrices",
                   "First45 mapping beyond explicitly named or decoded methods is ordered alignment evidence"]
    }
    pathlib.Path(output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(pathlib.Path(output).resolve()), "engine_sha256": digest,
                      "world_to_screen_slot": 37, "world_to_view_slot": 38,
                      "native_camera_ready": False, "getter_calls_or_locks": False}))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", default=r"H:\SteamLibrary\steamapps\common\Left 4 Dead 2\bin\engine.dll")
    parser.add_argument("--output", default=str(ROOT / "retail-camera-abi.json"))
    arguments = parser.parse_args()
    run(arguments.engine, arguments.output)


if __name__ == "__main__":
    main()
