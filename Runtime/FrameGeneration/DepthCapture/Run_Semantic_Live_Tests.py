import argparse
import os
from pathlib import Path
import subprocess
from Validate_Semantic_Live import validate


__author__ = "Anokianx"
ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxvk", type=Path)
    arguments = parser.parse_args()
    environment = os.environ.copy()
    environment.pop("VK_INSTANCE_LAYERS", None)
    environment.update({"ASTERIA_FG_ENABLE": "0", "ASTERIA_NATIVE_ENGINE_CAMERA": "0",
                        "ASTERIA_NATIVE_PIPELINE_PROBE": "0", "ASTERIA_NATIVE_UNIFORM_PROBE": "0",
                        "VK_LOADER_LAYERS_DISABLE": "~implicit~"})
    host = ROOT / "bin/AsteriaDepthSemanticHost.exe"
    backend = ("--dxvk", str(arguments.dxvk.resolve())) if arguments.dxvk else ()
    for control in ((), ("--no-depth",), ("--disabled",)):
        completed = subprocess.run([str(host), *control, *backend], cwd=ROOT, env=environment, timeout=40,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        prefix = "SemanticLiveVulkan" if arguments.dxvk else "SemanticLive"
        suffix = "NoDepth" if "--no-depth" in control else "Disabled" if "--disabled" in control else ""
        (ROOT / "results" / (prefix + suffix) / "host-run.log").write_text(completed.stdout, encoding="utf-8")
        if completed.returncode:
            print(completed.stdout[-6000:], end="")
            raise SystemExit(completed.returncode)
        print(next((line for line in completed.stdout.splitlines() if line.startswith("passed=")), "Host completed"))
    report = validate(bool(arguments.dxvk))
    print(f"Semantic live validation passed: {report['checks']} checks, {report['depth_pixels_checked']} actual GPU depth pixels")


if __name__ == "__main__":
    main()
