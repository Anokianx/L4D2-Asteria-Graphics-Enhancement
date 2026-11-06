import hashlib
import json
from pathlib import Path
import struct
import urllib.request
import urllib.error

root = Path(__file__).resolve().parent
tree = json.loads((root / "tree-main.json").read_text(encoding="utf-8-sig"))
commit = tree["sha"]
path = "Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll"
url = f"https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/{commit}/{path}"
request = urllib.request.Request(url, headers={"User-Agent": "Asteria-FSR-Research", "Range": "bytes=0-4095", "Accept-Encoding": "identity"})
with urllib.request.urlopen(request, timeout=30) as response:
    data = response.read(4096)
    status = response.status
    content_range = response.headers.get("Content-Range")
pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
if data[:2] != b"MZ" or data[pe_offset:pe_offset + 4] != b"PE\0\0":
    raise RuntimeError("Not a PE header")
machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
(root / "latest-framegeneration-dx12.pe-header.bin").write_bytes(data)
result = {"commit": commit, "url": url, "http_status": status, "content_range": content_range, "inspected_bytes": len(data), "header_sha256": hashlib.sha256(data).hexdigest(), "pe_machine": hex(machine), "architecture": "x64" if machine == 0x8664 else "other", "full_dll_downloaded": False}
(root / "prebuilt-header-result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result))

release = json.loads((root / "release-v1.1.4.json").read_text(encoding="utf-8-sig"))
asset = release["assets"][0]
request = urllib.request.Request(asset["browser_download_url"], headers={"User-Agent": "Asteria-FSR-Research", "Range": f"bytes={asset['size'] - 65536}-{asset['size'] - 1}", "Accept-Encoding": "identity"})
result = {"url": asset["browser_download_url"], "asset_bytes": asset["size"], "official_digest": asset.get("digest")}
try:
    with urllib.request.urlopen(request, timeout=30) as response:
        result.update({"http_status": response.status, "content_range": response.headers.get("Content-Range"), "content_length": response.headers.get("Content-Length")})
        if response.status == 206:
            tail = response.read(65536)
            result["range_probe_downloaded_bytes"] = len(tail)
            (root / "legacy-release-zip-tail.bin").write_bytes(tail)
        else:
            result["range_probe_downloaded_bytes"] = 0
            result["range_probe_reason"] = "No byte-range response; complete archive download deliberately skipped"
except urllib.error.HTTPError as error:
    result.update({"http_status": error.code, "range_probe_downloaded_bytes": 0, "range_probe_reason": str(error.reason)})
(root / "legacy-release-range-result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result))
