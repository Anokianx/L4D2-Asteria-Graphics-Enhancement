from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import time


ROOT = Path(__file__).resolve().parent
RESEARCH = ROOT.parent.parent / "Research"
SDK = RESEARCH / "sdk-v1.1.4" / "sdk"
GPU = SDK / "include" / "FidelityFX" / "gpu"
OUTPUT = ROOT / "Generated"
TOOLS = ROOT / "Tools"


def compile_pass(effect, source, suffix, half):
    name = source.stem + suffix
    args = [str(TOOLS / "FidelityFX_SC.exe"), "-compiler=glslang", "-e", "CS",
            "--target-env", "vulkan1.2", "-S", "comp", "-Os", "-DFFX_GLSL=1",
            "-reflection", "-deps=gcc", "-DFFX_GPU=1", f"-DFFX_HALF={half}"]
    if effect == "frameinterpolation":
        args += ["-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
                 "-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
                 "-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
                 "-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
                 "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
                 "-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={0,1}",
                 "-DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS={0,1}",
                 "-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={0,1}"]
    else:
        args += ["-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={0,1}"]
    args += [f"-name={name}", f"-I{GPU}", f"-I{GPU / effect}",
             f"-output={OUTPUT}", str(source)]
    started = time.monotonic()
    result = subprocess.run(args, cwd=TOOLS, capture_output=True, text=True)
    log = result.stdout + result.stderr
    (ROOT / "Logs" / f"{name}.log").write_text(log, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{name}: {result.returncode}\n{log}")
    if not (OUTPUT / f"{name}_permutations.h").is_file():
        raise RuntimeError(f"{name}: compiler did not produce permutation header")
    return {"effect": effect, "pass": source.stem, "variant": suffix or "fp32_wave32",
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "command": args, "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "result": log.strip()}


def inspect_outputs():
    bytecode = []
    binary = ROOT / "SPIRV"
    binary.mkdir(exist_ok=True)
    for header in sorted(OUTPUT.glob("*.h")):
        text = header.read_text(encoding="utf-8")
        match = re.search(r"static const unsigned char \w+_data\[\] = \{(.*?)\};", text, re.S)
        if not match:
            continue
        data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match[1]))
        if len(data) % 4 or len(data) < 20 or struct.unpack_from("<I", data)[0] != 0x07230203:
            raise RuntimeError(f"Invalid SPIR-V emitted in {header.name}")
        declared = re.search(r"static const uint32_t \w+_size = (\d+);", text)
        if not declared or int(declared[1]) != len(data):
            raise RuntimeError(f"SPIR-V size mismatch in {header.name}")
        (binary / f"{header.stem}.spv").write_bytes(data)
        bytecode.append({"header": header.name, "bytes": len(data),
                         "spirv_version": hex(struct.unpack_from("<I", data, 4)[0]),
                         "sha256": hashlib.sha256(data).hexdigest()})
    return bytecode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--workers", type=int, default=2)
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    (ROOT / "Logs").mkdir(exist_ok=True)
    shutil.copy2(RESEARCH / "sdk-v1.1.4" / "LICENSE.txt", ROOT / "LICENSE_AMD.txt")
    variants = [("", 0), ("_wave64", 0), ("_16bit", 1), ("_wave64_16bit", 1)]
    tasks = []
    for effect in ("frameinterpolation", "opticalflow"):
        for source in sorted((SDK / "src" / "backends" / "vk" / "shaders" / effect).glob("*.glsl")):
            for suffix, half in variants:
                tasks.append((effect, source, suffix, half))
    results = []
    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as executor:
        pending = [executor.submit(compile_pass, *task) for task in tasks]
        for future in as_completed(pending):
            item = future.result()
            results.append(item)
            print(f"{len(results)}/{len(tasks)} {item['pass']}{item['variant']}", flush=True)
    bytecode = inspect_outputs()
    source = json.loads((RESEARCH / "source-manifest.json").read_text())
    manifest = {"repository": source["repository"], "tag": source["tag"], "commit": source["commit"],
                "api": "Vulkan 1.2", "passes": 18, "variant_compiles": len(results),
                "permutation_headers": len(list(OUTPUT.glob("*_permutations.h"))),
                "bytecode_headers": len(bytecode), "bytecode_bytes": sum(x["bytes"] for x in bytecode),
                "compiles": sorted(results, key=lambda x: (x["effect"], x["pass"], x["variant"])),
                "bytecode": bytecode}
    (ROOT / "generated-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: manifest[key] for key in ("variant_compiles", "permutation_headers",
                                                       "bytecode_headers", "bytecode_bytes")}), flush=True)


if __name__ == "__main__":
    main()
