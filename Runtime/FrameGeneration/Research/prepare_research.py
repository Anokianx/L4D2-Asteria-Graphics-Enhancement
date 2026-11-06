import concurrent.futures
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parent
TREE = json.loads((ROOT / "tree-v1.1.4.json").read_text(encoding="utf-8-sig"))
COMMIT = TREE["sha"]
SOURCE = ROOT / "sdk-v1.1.4"
EXACT = {
    "LICENSE.txt", "README.md", "CMakeLists.txt", "sdk/CMakeLists.txt",
    "ffx-api/CMakeLists.txt", "ffx-api/BuildFfxApiDll.bat",
    "docs/techniques/frame-interpolation.md", "docs/techniques/optical-flow.md",
    "docs/techniques/frame-interpolation-swap-chain.md",
    "docs/techniques/super-resolution-interpolation.md",
    "docs/getting-started/building-samples.md", "docs/getting-started/ffx-api.md",
    "docs/getting-started/migrating-to-fsr-3-1.md",
    "sdk/include/FidelityFX/host/backends/vk/ffx_vk.h",
    "sdk/src/backends/vk/CMakeLists.txt", "sdk/src/backends/vk/ffx_vk.cpp",
    "sdk/src/backends/vk/CMakeShadersFrameinterpolation.txt",
    "sdk/src/backends/vk/CMakeShadersOpticalflow.txt",
    "sdk/src/backends/shared/ffx_shader_blobs.cpp", "sdk/src/backends/shared/ffx_shader_blobs.h",
}
PREFIXES = (
    "sdk/src/components/frameinterpolation/", "sdk/src/components/opticalflow/",
    "sdk/src/shared/", "sdk/include/FidelityFX/gpu/frameinterpolation/",
    "sdk/include/FidelityFX/gpu/opticalflow/", "sdk/include/FidelityFX/gpu/spd/",
    "sdk/src/backends/vk/FrameInterpolationSwapchain/",
    "sdk/src/backends/vk/shaders/frameinterpolation/", "sdk/src/backends/vk/shaders/opticalflow/",
    "ffx-api/include/", "ffx-api/src/",
)


def selected(path):
    if path in EXACT or path.startswith(PREFIXES):
        return True
    parent = str(Path(path).parent).replace("\\", "/")
    if parent in ("sdk/include/FidelityFX/host", "sdk/include/FidelityFX/gpu") and path.endswith(".h"):
        return True
    return path.startswith("sdk/src/backends/shared/blob_accessors/ffx_frameinterpolation_") or path.startswith("sdk/src/backends/shared/blob_accessors/ffx_opticalflow_")


def fetch(entry):
    path = entry["path"]
    target = SOURCE / path
    url = f"https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/{COMMIT}/{path}"
    if target.is_file():
        data = target.read_bytes()
    else:
        request = urllib.request.Request(url, headers={"User-Agent": "Asteria-FSR-Research"})
        with urllib.request.urlopen(request, timeout=30) as response:
            data = response.read()
    blob_sha = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
    if blob_sha != entry["sha"]:
        raise RuntimeError(f"Git blob hash mismatch: {path}")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {"path": path, "url": url, "bytes": len(data), "git_blob_sha1": blob_sha, "sha256": hashlib.sha256(data).hexdigest()}


entries = [entry for entry in TREE["tree"] if entry["type"] == "blob" and selected(entry["path"])]
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
    files = list(executor.map(fetch, entries))
manifest = {"repository": "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK", "tag": "v1.1.4", "commit": COMMIT, "downloaded_bytes": sum(item["bytes"] for item in files), "files": files}
(ROOT / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Verified {len(files)} official files, {manifest['downloaded_bytes']} bytes")
