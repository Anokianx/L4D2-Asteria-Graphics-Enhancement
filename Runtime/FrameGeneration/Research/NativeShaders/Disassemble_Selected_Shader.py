import argparse
import ctypes
import hashlib
import json
import os
import pathlib
import struct

from Scan_Native_Shaders import decode_lzma, parse_vcs6, require


AUTHOR = "Anokianx"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", type=pathlib.Path, default=pathlib.Path(r"H:\SteamLibrary\steamapps\common\Left 4 Dead 2"))
    parser.add_argument("--shader", default="hdrselectrange_ps20.vcs")
    arguments = parser.parse_args()
    require(pathlib.Path(arguments.shader).name == arguments.shader, "shader must be a filename")
    path = arguments.game.resolve(strict=True) / "hl2" / "shaders" / "fxc" / arguments.shader
    data = path.read_bytes()
    parse_vcs6(data)
    start, = struct.unpack_from("<I", data, 32)
    packed_size = struct.unpack_from("<I", data, start)[0] & 0x3FFFFFFF
    block = decode_lzma(data[start + 4:start + 4 + packed_size])
    combo_id, byte_count = struct.unpack_from("<II", block)
    bytecode = block[8:8 + byte_count]
    system_root = pathlib.Path(os.environ["SystemRoot"])
    library = ctypes.WinDLL(str(system_root / "System32" / "d3dx9_43.dll"))
    disassemble = library.D3DXDisassembleShader
    disassemble.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    disassemble.restype = ctypes.c_long
    buffer = ctypes.c_void_p()
    shader = ctypes.create_string_buffer(bytecode)
    result = disassemble(shader, 0, None, ctypes.byref(buffer))
    require(result >= 0 and buffer.value, f"D3DXDisassembleShader failed: {result & 0xFFFFFFFF:08x}")
    vtable = ctypes.cast(buffer, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    get_pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtable[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_uint, ctypes.c_void_p)(vtable[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_uint, ctypes.c_void_p)(vtable[2])
    try:
        text = ctypes.string_at(get_pointer(buffer), get_size(buffer)).rstrip(b"\0").decode("ascii")
    finally:
        release(buffer)
    print(json.dumps({
        "author": AUTHOR,
        "source_path": str(path),
        "source_sha256": hashlib.sha256(data).hexdigest(),
        "static_record_index": 0,
        "dynamic_combo_id": combo_id,
        "bytecode_sha256": hashlib.sha256(bytecode).hexdigest(),
        "tool": "Microsoft D3DXDisassembleShader, system d3dx9_43.dll",
        "instructions": [line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith("//")],
    }, indent=2))


if __name__ == "__main__":
    main()
