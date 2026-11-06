import hashlib
import json
from pathlib import Path
import struct
import urllib.request
import zlib

root = Path(__file__).resolve().parent
release = json.loads((root / "release-v1.1.4.json").read_text(encoding="utf-8-sig"))
asset = release["assets"][0]
url = asset["browser_download_url"]


def read_range(start, end):
    request = urllib.request.Request(url, headers={"User-Agent": "Asteria-FSR-Research", "Range": f"bytes={start}-{end}", "Accept-Encoding": "identity"})
    with urllib.request.urlopen(request, timeout=30) as response:
        expected = f"bytes {start}-{end}/{asset['size']}"
        if response.status != 206 or response.headers.get("Content-Range") != expected:
            raise RuntimeError("Byte-range support required; refusing a complete archive download")
        return response.read(end - start + 1)


tail = (root / "legacy-release-zip-tail.bin").read_bytes()
eocd = struct.unpack_from("<4s4H2LH", tail, tail.rfind(b"PK\x05\x06"))
directory_size, directory_start = eocd[5:7]
directory = read_range(directory_start, directory_start + directory_size - 1)
(root / "legacy-release-zip-directory.bin").write_bytes(directory)
offset = 0
entries = []
while offset < len(directory):
    fields = struct.unpack_from("<4s6H3I5H2I", directory, offset)
    if fields[0] != b"PK\x01\x02":
        raise RuntimeError("Invalid ZIP directory")
    name_length, extra_length, comment_length = fields[10:13]
    name = directory[offset + 46:offset + 46 + name_length].decode("utf-8")
    if name.endswith(".dll") and ("fidelityfx" in name.lower() or "fsr3api" in name.lower()):
        entries.append({"path": name, "compressed_bytes": fields[8], "bytes": fields[9], "method": fields[4], "local_offset": fields[16]})
    offset += 46 + name_length + extra_length + comment_length

for entry in entries:
    start = entry["local_offset"]
    data = read_range(start, min(asset["size"] - 1, start + 65535))
    fields = struct.unpack_from("<4s5H3I2H", data)
    content_start = 30 + fields[9] + fields[10]
    if entry["method"] == 8:
        header = zlib.decompressobj(-15).decompress(data[content_start:], 4096)
    elif entry["method"] == 0:
        header = data[content_start:content_start + 4096]
    else:
        raise RuntimeError("Unsupported ZIP compression")
    pe = struct.unpack_from("<I", header, 0x3C)[0]
    if header[:2] != b"MZ" or header[pe:pe + 4] != b"PE\0\0":
        raise RuntimeError("Not a PE header")
    machine = struct.unpack_from("<H", header, pe + 4)[0]
    entry.update({"pe_machine": hex(machine), "architecture": "x64" if machine == 0x8664 else "other", "inspected_bytes": len(header), "header_sha256": hashlib.sha256(header).hexdigest(), "full_dll_downloaded": False})
    (root / (Path(entry["path"]).name + ".pe-header.bin")).write_bytes(header)
result = {"url": url, "asset_bytes": asset["size"], "directory_bytes": directory_size, "range_bytes_downloaded": directory_size + 65536 * len(entries), "entries": entries}
(root / "legacy-prebuilt-header-result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result, indent=2))
