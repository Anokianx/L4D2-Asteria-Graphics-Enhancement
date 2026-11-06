import hashlib
import json
import pathlib
import urllib.request


AUTHOR = "Anokianx"
COMMIT = "037d0fa1ad093065b18dad968d5898cbcd143e0e"
FILES = [
    "LICENSE",
    "src/d3d9/d3d9_caps.h",
    "src/d3d9/d3d9_device.cpp",
    "src/d3d9/d3d9_shader.cpp",
    "src/d3d9/d3d9_constant_buffer.cpp",
    "src/d3d9/d3d9_constant_buffer.h",
    "src/d3d9/d3d9_constant_layout.h",
    "src/d3d9/d3d9_constant_set.h",
    "src/dxso/dxso_compiler.cpp",
    "src/dxso/dxso_module.cpp",
    "src/dxso/dxso_isgn.h",
    "src/dxso/dxso_common.h",
    "src/dxso/dxso_util.h",
    "src/dxvk/dxvk_shader.cpp",
    "src/dxvk/dxvk_pipelayout.cpp",
    "src/dxvk/dxvk_pipelayout.h",
    "src/dxvk/dxvk_context.cpp",
    "src/dxvk/dxvk_context.h",
    "src/dxvk/dxvk_buffer.h",
    "src/dxvk/dxvk_buffer.cpp",
    "src/dxvk/dxvk_cmdlist.h",
]


def main():
    directory = pathlib.Path(__file__).resolve().parent / "Reference" / "DXVK-v2.3.1"
    manifest = {"author": AUTHOR, "version": "v2.3.1", "commit": COMMIT, "upstream": "https://github.com/doitsujin/dxvk", "files": []}
    for relative in FILES:
        url = f"https://raw.githubusercontent.com/doitsujin/dxvk/{COMMIT}/{relative}"
        destination = directory / relative
        if destination.is_file():
            content = destination.read_bytes()
        else:
            with urllib.request.urlopen(url, timeout=30) as response:
                content = response.read(2_000_001)
            if len(content) > 2_000_000:
                raise ValueError("reference file exceeds size limit")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)
        manifest["files"].append({"path": relative, "url": url, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
    (directory / "reference-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    spirv_directory = directory.parent / "SPIRV-Headers-1.3.268.0"
    spirv_manifest = {"author": AUTHOR, "tag": "vulkan-sdk-1.3.268.0", "upstream": "https://github.com/KhronosGroup/SPIRV-Headers", "files": []}
    for relative in ["LICENSE", "include/spirv/unified1/spirv.core.grammar.json"]:
        url = f"https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/vulkan-sdk-1.3.268.0/{relative}"
        destination = spirv_directory / relative
        if destination.is_file():
            content = destination.read_bytes()
        else:
            with urllib.request.urlopen(url, timeout=30) as response:
                content = response.read(2_000_001)
            if len(content) > 2_000_000:
                raise ValueError("SPIR-V reference file exceeds size limit")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)
        spirv_manifest["files"].append({"path": relative, "url": url, "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()})
    (spirv_directory / "reference-manifest.json").write_text(json.dumps(spirv_manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"directory": str(directory), "commit": COMMIT, "files": len(FILES), "bytes": sum(record["bytes"] for record in manifest["files"])}))


if __name__ == "__main__":
    main()
