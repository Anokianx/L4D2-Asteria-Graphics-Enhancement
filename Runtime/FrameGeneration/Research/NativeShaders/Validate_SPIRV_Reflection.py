import hashlib
import json
import pathlib
import struct

from Reflect_SPIRV_Inputs import DIRECTORY, ReflectionError, literal_string, reflect, require


AUTHOR = "Anokianx"


def rejected(data):
    try:
        reflect(data)
    except ReflectionError:
        return True
    raise AssertionError("malformed SPIR-V was accepted")


def main():
    shaders = sorted((DIRECTORY.parents[1] / "Port/Shaders/SPIRV").glob("*.spv"))
    require(shaders, "official shader fixtures are unavailable")
    for shader in shaders:
        reflect(shader.read_bytes())
    original = shaders[0].read_bytes()
    cases = {}
    data = bytearray(original)
    struct.pack_into("<I", data, 0, 0xDEADBEEF)
    cases["invalid_magic"] = rejected(bytes(data))
    data = bytearray(original)
    struct.pack_into("<I", data, 12, 1)
    cases["invalid_id_bound"] = rejected(bytes(data))
    data = bytearray(original)
    struct.pack_into("<I", data, 20, struct.unpack_from("<I", data, 20)[0] & 0xFFFF)
    cases["zero_instruction_length"] = rejected(bytes(data))
    data = bytearray(original)
    struct.pack_into("<I", data, 20, 0xFFFF0000 | (struct.unpack_from("<I", data, 20)[0] & 0xFFFF))
    cases["instruction_exceeds_file"] = rejected(bytes(data))
    cases["non_dword_truncation"] = rejected(original[:-1])
    cases["header_without_module"] = rejected(original[:20])
    try:
        literal_string([0x0080FFFF], 0)
    except ReflectionError:
        cases["invalid_utf8_name"] = True
    else:
        raise AssertionError("invalid UTF-8 literal was accepted")
    profile_file = DIRECTORY / "native-uniform-profiles.json"
    manifest = json.loads(profile_file.read_text(encoding="utf-8"))
    verified_profiles = 0
    for profile in manifest["profiles"]:
        path = pathlib.Path(profile["spirv_file"])
        data = path.read_bytes()
        require(hashlib.sha256(data).hexdigest() == profile["shader_sha256"], "whitelisted shader SHA mismatch")
        record = reflect(data)
        candidates = record["dxvk_hardware_vertex_profile_candidates"]
        require(any(candidate["descriptor_set"] == profile["descriptor_set"] and candidate["binding"] == profile["binding"] and candidate["float_member_offset"] == 256 and candidate["array_stride"] == 16 and candidate["c8_c11_indices_directly_used_by_dot"] == [8, 9, 10, 11] for candidate in candidates), "whitelist profile differs from actual shader")
        require(profile["relative_byte_offset"] == 384 and profile["byte_length"] == 64 and not profile["camera_semantics_verified"], "invalid whitelist bounds or readiness")
        require(profile["native_vcs_exact_origins"] and all(origin["c_viewproj_ctab"] for origin in profile["native_vcs_exact_origins"]), "whitelist lacks exact native CTAB origin")
        verified_profiles += 1
    report = {"author": AUTHOR, "date": "2026-10-15", "passed": True, "official_ffx_shader_files_reflected": len(shaders), "actual_game_profiles_revalidated": verified_profiles, "profile_manifest_sha256": hashlib.sha256(profile_file.read_bytes()).hexdigest(), "malformed_inputs_rejected": cases, "native_camera_semantics_verified": False, "native_motion_vectors_verified": False, "gpu_buffer_bytes_read": False}
    output = DIRECTORY / "spirv-reflector-fixture-validation.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
