import concurrent.futures
import hashlib
import json
from pathlib import Path
import urllib.request

root = Path(__file__).resolve().parent
tree = json.loads((root / "tree-vulkan-headers-v1.3.250.json").read_text(encoding="utf-8-sig"))
commit = tree["sha"]
destination = root / "vulkan-headers-v1.3.250"


def fetch(entry):
    path = entry["path"]
    target = destination / path
    url = f"https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/{commit}/{path}"
    if target.is_file():
        data = target.read_bytes()
    else:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "Asteria-FSR-Research"}), timeout=30) as response:
            data = response.read()
    blob = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
    if blob != entry["sha"]:
        raise RuntimeError(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {"path": path, "url": url, "bytes": len(data), "git_blob_sha1": blob, "sha256": hashlib.sha256(data).hexdigest()}


entries = [entry for entry in tree["tree"] if entry["type"] == "blob" and (entry["path"] == "LICENSE.txt" or (entry["path"].startswith(("include/vulkan/", "include/vk_video/")) and entry["path"].endswith(".h")))]
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
    files = list(executor.map(fetch, entries))
result = {"repository": "https://github.com/KhronosGroup/Vulkan-Headers", "tag": "v1.3.250", "commit": commit, "downloaded_bytes": sum(entry["bytes"] for entry in files), "files": files}
(root / "vulkan-headers-manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(f"Verified {len(files)} Vulkan header files, {result['downloaded_bytes']} bytes")
