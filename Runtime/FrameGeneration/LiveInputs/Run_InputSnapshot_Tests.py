from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from Validate_InputSnapshot import ROOT, SOURCES, validate

__author__ = "Anokianx"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxvk", type=Path, required=True)
    options = parser.parse_args()
    source = options.dxvk.resolve()
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    before = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in SOURCES}
    environment = os.environ.copy()
    environment.update({"ASTERIA_FG_ENABLE": "0", "ASTERIA_NATIVE_ENGINE_CAMERA": "0", "ASTERIA_NATIVE_INPUT_PROBE": "0"})
    for flags, suffix in (((), ""), (("--no-depth",), "NoDepth"), (("--disabled",), "Disabled"),
                          (("--invalidate-source",), "Invalidated"), (("--resize",), "Resize"), (("--unqualified-context",), "Unqualified")):
        result = subprocess.run([str(ROOT / "LiveInputs/bin/AsteriaInputSnapshotHost.exe"), "--dxvk", str(source), *flags],
                                cwd=ROOT, env=environment, capture_output=True, timeout=40)
        output = ROOT / "LiveInputs/results" / ("InputSnapshotVulkan" + suffix)
        output.mkdir(parents=True, exist_ok=True)
        (output / "host.stdout.log").write_bytes(result.stdout)
        (output / "host.stderr.log").write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(f"Owned input snapshot host {suffix} failed ({result.returncode}); see {output}")
        print(f"Owned GPU snapshot host passed: {suffix or 'Normal'}", flush=True)
    if digest != hashlib.sha256(source.read_bytes()).hexdigest() or before != {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in SOURCES}:
        raise RuntimeError("Source or binary changed during snapshot tests")
    report = validate()
    print(json.dumps({key: value for key, value in report.items() if key != "artifacts"}, indent=2))
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
