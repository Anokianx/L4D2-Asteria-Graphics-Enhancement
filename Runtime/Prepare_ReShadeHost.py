import hashlib
import io
import json
import struct
import urllib.request
import zipfile
from pathlib import Path

VERSION = "6.7.2"
URL = "https://reshade.me/downloads/ReShade_Setup_6.7.2_Addon.exe"
SHA256 = "47d841b1cd03dd2032aa14f6b1365f1e8221f8399e4f2ad6f4629cbef4a14ef0"
ROOT = Path(__file__).resolve().parent
HOST = ROOT / "Host"
LAYER_NAME = "VK_LAYER_Asteria_ReShade"


def write_manifest(manifest):
    manifest["layer"].pop("disable_environment", None)
    manifest["layer"]["name"] = LAYER_NAME
    manifest["layer"]["library_path"] = ".\\ReShade32.dll"
    manifest["layer"]["description"] = "Asteria host using crosire's ReShade 6.7.2 (32-bit, full add-on support)"
    (HOST / "ReShade32.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


def prepare():
    HOST.mkdir(exist_ok=True)
    dll = HOST / "ReShade32.dll"
    metadata = HOST / "origin.json"
    if dll.is_file() and metadata.is_file() and (HOST / "ReShade32.json").is_file() and (HOST / "LICENSE.md").is_file():
        try:
            origin = json.loads(metadata.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            origin = {}
        if origin.get("installer_sha256") == SHA256 and origin.get("dll_sha256") == hashlib.sha256(dll.read_bytes()).hexdigest():
            write_manifest(json.loads((HOST / "ReShade32.json").read_text(encoding="utf-8")))
            origin["layer"] = LAYER_NAME
            metadata.write_text(json.dumps(origin, indent=2), encoding="utf-8")
            print("Asteria ReShade host already verified.")
            return
    request = urllib.request.Request(URL, headers={"User-Agent": "Asteria-Build/1.0"})
    with urllib.request.urlopen(request, timeout=60) as response:
        package = response.read()
    if hashlib.sha256(package).hexdigest() != SHA256:
        raise RuntimeError("Official ReShade package checksum mismatch")
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        runtime = archive.read("ReShade32.dll")
        manifest = json.loads(archive.read("ReShade32.json"))
    offset = struct.unpack_from("<I", runtime, 0x3C)[0]
    if runtime[:2] != b"MZ" or struct.unpack_from("<H", runtime, offset + 4)[0] != 0x14C:
        raise RuntimeError("ReShade host must be a 32-bit Windows library")
    dll.write_bytes(runtime)
    write_manifest(manifest)
    (HOST / "LICENSE.md").write_bytes((ROOT / "sdk" / "reshade" / "LICENSE.md").read_bytes())
    metadata.write_text(json.dumps({"project": "ReShade", "version": VERSION, "edition": "full add-on support",
                                    "source": URL, "layer": LAYER_NAME, "installer_sha256": SHA256,
                                    "dll_sha256": hashlib.sha256(runtime).hexdigest()}, indent=2), encoding="utf-8")
    print(f"Asteria ReShade Vulkan host prepared at {HOST}")


if __name__ == "__main__":
    prepare()
