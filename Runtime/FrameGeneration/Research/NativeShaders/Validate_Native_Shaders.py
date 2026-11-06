import hashlib
import json
import pathlib
import struct

from Scan_Native_Shaders import ParseError, decode_lzma, parse_vcs6, require


AUTHOR = "Anokianx"


def expect_rejected(function, data):
    try:
        function(data)
    except (ParseError, ValueError):
        return True
    raise AssertionError("malformed input was accepted")


def main():
    directory = pathlib.Path(__file__).resolve().parent
    inventory_path = directory / "native_shader_inventory.json"
    findings_path = directory / "native_shader_findings.json"
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    findings = json.loads(findings_path.read_text(encoding="utf-8"))
    game = pathlib.Path(inventory["game"])
    require(not directory.is_relative_to(game), "validation output must remain outside the game directory")
    require(inventory["summary"]["files_error"] == 0, "scan contains errors")
    require(hashlib.sha256(inventory_path.read_bytes()).hexdigest() == findings["inventory_sha256"], "findings inventory hash mismatch")
    unchanged = 0
    for record in inventory["files"]:
        data = (game / record["path"]).read_bytes()
        require(len(data) == record["bytes"] and hashlib.sha256(data).hexdigest() == record["sha256"], "source changed after scan")
        unchanged += 1
    source = game / "hl2" / "shaders" / "fxc" / "motion_blur_ps20b.vcs"
    data = source.read_bytes()
    parsed = parse_vcs6(data)
    require(parsed["coverage"]["unique_compiled_combinations"] == 15, "motion blur combination coverage mismatch")
    samplers = [entry for entry in parsed["bindings"] if entry["register_set"] == "sampler"]
    require(len(samplers) == 1 and samplers[0]["name"] == "g_tTexSampler" and samplers[0]["register_index"] == 0 and samplers[0]["compiled_combinations"] == 15, "motion blur sampler reflection mismatch")
    malformed = {}
    changed = bytearray(data)
    struct.pack_into("<I", changed, 36, 0)
    malformed["static_sentinel"] = expect_rejected(parse_vcs6, bytes(changed))
    changed = bytearray(data)
    struct.pack_into("<I", changed, 32, len(data) + 4)
    malformed["static_payload_bounds"] = expect_rejected(parse_vcs6, bytes(changed))
    changed = bytearray(data)
    struct.pack_into("<I", changed, 48, 0x40000000 | len(data))
    malformed["compressed_block_bounds"] = expect_rejected(parse_vcs6, bytes(changed))
    malformed["truncated_file"] = expect_rejected(parse_vcs6, data[:-1])
    packed_size = struct.unpack_from("<I", data, 48)[0] & 0x3FFFFFFF
    malformed["invalid_lzma_magic"] = expect_rejected(decode_lzma, b"BAD!" + data[56:52 + packed_size])
    report = {
        "author": AUTHOR,
        "date": "2026-10-15",
        "passed": True,
        "source_files_hash_unchanged": unchanged,
        "inventory_sha256": hashlib.sha256(inventory_path.read_bytes()).hexdigest(),
        "motion_blur_dynamic_combinations": 15,
        "motion_blur_single_sampler_verified_all_combinations": True,
        "malformed_inputs_rejected": malformed,
        "native_motion_vectors_verified": False,
        "native_camera_verified": False,
    }
    output = directory / "native_shader_validation.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
