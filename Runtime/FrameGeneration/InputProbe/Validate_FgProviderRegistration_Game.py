from __future__ import annotations

import argparse
import json
from pathlib import Path

__author__ = "Anokianx"


def read(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", required=True, type=Path)
    options = parser.parse_args()
    directory = options.directory.resolve()
    layer = read(directory / "mapped_layer_collection_observation.json")
    devices = list(layer.get("devices", {}).values())
    swapchains = list(layer.get("swapchains", {}).values())
    failures = []
    if not devices or not all(device.get("frame_generation_requested") for device in devices):
        failures.append("explicit FSR device request was not observed")
    if not swapchains or not any(swapchain.get("swapchain_adapter_active") and swapchain.get("frame_input_provider_available")
                                 and swapchain.get("frame_input_status") == "awaiting_valid_frame_inputs"
                                 for swapchain in swapchains):
        failures.append("provider-aware FG swapchain was not observed")
    if layer.get("frame_generation") or any(swapchain.get("frame_generation_active") for swapchain in swapchains):
        failures.append("provider registration without submitted inputs was promoted to active frame generation")
    result = {"author": __author__, "passed": not failures, "scope": "actual_game_provider_registration_and_proxy_swapchain_observer_only",
              "failures": failures, "game_started_by_validator": False, "registry_modified_by_validator": False,
              "provider_registration_observed": True, "proxy_swapchain_observed": not failures,
              "frame_inputs_submitted": False, "gpu_frame_aligned": False, "main_view_verified": False,
              "game_fg_ready": False, "native_fg_ready": False}
    (directory / "fg_provider_registration_validation.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
