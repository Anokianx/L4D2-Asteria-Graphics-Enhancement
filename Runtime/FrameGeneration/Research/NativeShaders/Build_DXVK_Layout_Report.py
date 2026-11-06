import hashlib
import json
import pathlib
import re


AUTHOR = "Anokianx"
DIRECTORY = pathlib.Path(__file__).resolve().parent


def main():
    reference = DIRECTORY / "Reference/DXVK-v2.3.1"
    manifest = json.loads((reference / "reference-manifest.json").read_text(encoding="utf-8"))
    for record in manifest["files"]:
        if hashlib.sha256((reference / record["path"]).read_bytes()).hexdigest() != record["sha256"]:
            raise ValueError("DXVK reference hash changed")
    caps = (reference / "src/d3d9/d3d9_caps.h").read_text(encoding="utf-8")
    int_count = int(re.search(r"MaxOtherConstants\s*=\s*(\d+)", caps)[1])
    float_count = int(re.search(r"MaxFloatConstantsVS\s*=\s*(\d+)", caps)[1])
    compiler = (reference / "src/dxso/dxso_compiler.cpp").read_text(encoding="utf-8")
    layout = (reference / "src/d3d9/d3d9_constant_layout.h").read_text(encoding="utf-8")
    context = (reference / "src/dxvk/dxvk_context.cpp").read_text(encoding="utf-8")
    buffer = (reference / "src/dxvk/dxvk_buffer.h").read_text(encoding="utf-8")
    signatures = {
        "float_member_after_int_member": "floatOffset()   const { return intOffset() + intSize(); }" in layout,
        "float_member_index_one": "m_module.memberDecorateOffset(structType, 1, m_layout->floatOffset());" in compiler,
        "float_array_stride_sixteen": "m_module.decorateArrayStride(members[1], 16);" in compiler,
        "ordinary_ubo": "DxvkBindingInfo binding = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER };" in compiler,
        "descriptor_offset_includes_physical_and_ring_slice": "result.buffer.offset = m_physSlice.offset + offset;" in buffer,
        "descriptor_bind_dynamic_offset_count_zero": bool(re.search(r"cmdBindDescriptorSets\(BindPoint,[\s\S]{0,200}firstSet,[\s\S]{0,120}0, nullptr\);", context)),
    }
    if not all(signatures.values()) or int_count != 16 or float_count != 256:
        raise ValueError("DXVK source does not match the documented hardware profile")
    log_path = DIRECTORY.parents[1] / "InputProbe/results/GameProbe/left4dead2_d3d9.log"
    log_data = log_path.read_bytes()
    version_match = re.search(rb"DXVK:\s*(v[^\r\n]+)", log_data)
    if not version_match or version_match[1].decode("ascii") != manifest["version"]:
        raise ValueError("observed DXVK log version differs from reference")
    shader_root = DIRECTORY.parents[2] / "sdk/reshade"
    events = (shader_root / "reshade_events.hpp").read_text(encoding="utf-8")
    trait = re.search(r"RESHADE_DEFINE_ADDON_EVENT_TRAITS\(addon_event::bind_descriptor_tables,[^\n]+", events)[0]
    if "dynamic" in trait.lower():
        raise ValueError("public ReShade signature changed; re-audit dynamic offset support")
    vertex_offsets = {str(register): int_count * 16 + register * 16 for register in range(8, 12)}
    profile_path = DIRECTORY / "native-uniform-profiles.json"
    profile_manifest = json.loads(profile_path.read_text(encoding="utf-8")) if profile_path.is_file() else {"profiles": []}
    report = {
        "author": AUTHOR,
        "date": "2026-10-15",
        "observed_dxvk": {"version": version_match[1].decode("ascii"), "log": str(log_path), "log_sha256": hashlib.sha256(log_data).hexdigest(), "source_commit": manifest["commit"], "release_binary_equivalence_verified": False},
        "source_signature_checks": signatures,
        "source_reference_manifest_sha256": hashlib.sha256((reference / "reference-manifest.json").read_bytes()).hexdigest(),
        "hardware_vertex_constant_profile": {
            "storage": "ordinary Vulkan UNIFORM_BUFFER",
            "block_type_name": "cbuffer_t",
            "variable_name": "c",
            "integer_member": {"index": 0, "name": "i", "offset": 0, "elements": int_count, "array_stride": 16},
            "float_member": {"index": 1, "name": "f", "offset": int_count * 16, "elements": float_count, "array_stride": 16},
            "c8_to_c11_offsets_relative_to_descriptor_range": vertex_offsets,
            "c_viewproj_candidate_byte_offset": vertex_offsets["8"],
            "c_viewproj_candidate_byte_length": 64,
            "access_chain_expected_member_and_register_indices": [[1, register] for register in range(8, 12)],
            "logical_compiler_descriptor_set": 0,
            "logical_compiler_resource_binding": 0,
            "standard_remapped_vertex_descriptor_set": 2,
            "final_binding": "Per actual captured shader; binding list sorting and shader resource use change the mapped binding number.",
        },
        "software_vertex_constant_profile": {"separate_float_block": True, "float_member_index": 0, "float_member_offset": 0, "float_array_stride": 16, "c8_to_c11_relative_offsets": [128, 144, 160, 176], "possible_storage": "UNIFORM_BUFFER or STORAGE_BUFFER; must inspect actual SPIR-V Block/BufferBlock and descriptor type"},
        "ordinary_descriptor_address_formula": "VkDescriptorBufferInfo.offset + reflected member Offset + register index * reflected ArrayStride",
        "dynamic_descriptor_formula_if_present": "ordinary descriptor offset + bind-time dynamic offset + reflected member Offset + register index * reflected ArrayStride",
        "public_reshade": {"bind_descriptor_tables_signature_has_dynamic_offsets": False, "buffer_range_fields": ["buffer", "offset", "size"], "ordinary_dxvk_offset_is_descriptor_update_visible": True, "signature_sha256": hashlib.sha256(trait.encode("utf-8")).hexdigest(), "dynamic_offset_limitation_is_current_standard_dxvk_blocker": False},
        "actual_native_shader_profiles": {"path": str(profile_path), "profiles": len(profile_manifest["profiles"]), "sha256": hashlib.sha256(profile_path.read_bytes()).hexdigest() if profile_path.is_file() else None, "actual_spirv_layout_and_exact_native_vcs_origin_verified": bool(profile_manifest["profiles"]), "main_view_and_frame_semantics_verified": False},
        "spirv_does_not_distinguish_ordinary_vs_dynamic_descriptor_type": True,
        "ordinary_descriptor_claim_scope": "Matching upstream DXVK implementation, not a raw actual-game VkDescriptorSetLayout descriptorType capture.",
        "still_unverified": ["Actual installed binary equivalence with unmodified upstream v2.3.1; raw actual-game Vulkan descriptor type and bind-time dynamicOffsetCount have not been recorded.", "Current draw descriptor association, buffer range/lifetime, CPU-write timing and GPU submission/frame provenance.", "Current main view versus weapon/shadow/reflection/UI classification, projection conventions and correct frame alignment.", "Existing native dense per-pixel motion vectors and previous deformation transforms."],
        "native_camera_verified": False,
        "native_motion_vectors_verified": False,
        "game_files_modified": False,
        "gpu_memory_read": False,
    }
    output = DIRECTORY / "dxvk-constant-layout.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"output": str(output), "source_signatures_verified": all(signatures.values()), "dxvk": report["observed_dxvk"]["version"], "c8_c11_offsets": vertex_offsets}))


if __name__ == "__main__":
    main()
