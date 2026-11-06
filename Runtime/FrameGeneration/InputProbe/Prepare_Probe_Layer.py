from pathlib import Path
import argparse
import json
import winreg


ROOT = Path(__file__).resolve().parent
MANIFEST = (ROOT.parent / "Layer/AsteriaFrameGenerationLayer.json").resolve()
STATE = ROOT / "results/ProbeLayerRegistration/state.json"
KEY = r"SOFTWARE\Khronos\Vulkan\ExplicitLayers"
ACCESS = winreg.KEY_SET_VALUE | winreg.KEY_QUERY_VALUE | winreg.KEY_WOW64_32KEY


def save(data):
    STATE.parent.mkdir(parents=True, exist_ok=True)
    temporary = STATE.with_suffix(".tmp")
    try:
        temporary.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        temporary.replace(STATE)
    except Exception:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def value(key):
    try:
        data, kind = winreg.QueryValueEx(key, str(MANIFEST))
        return {"exists": True, "data": data, "kind": kind}
    except FileNotFoundError:
        return {"exists": False}


def main():
    global STATE
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("prepare", "restore"))
    parser.add_argument("--state", type=Path)
    options = parser.parse_args()
    action = options.action
    if options.state:
        selected = options.state.resolve()
        if not selected.is_relative_to((ROOT / "results").resolve()):
            raise ValueError("Registration state must stay under this probe's results directory")
        STATE = selected
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if manifest["layer"]["name"] != "VK_LAYER_Asteria_FrameGeneration":
        raise RuntimeError("The manifest identity is not the isolated Asteria layer")
    with winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE, KEY, 0, ACCESS) as key:
        current = value(key)
        if action == "prepare":
            old = json.loads(STATE.read_text(encoding="utf-8")) if STATE.exists() else None
            if old and not old.get("restored"):
                if old["manifest"] != str(MANIFEST) or current != {"exists": True, "data": 0, "kind": winreg.REG_DWORD}:
                    raise RuntimeError("The previous test registration state cannot be safely reused")
                print(json.dumps({"prepared": True, "existing_test_state_reused": True}))
                return
            record = {"author": "Anokianx", "registry_view": 32, "manifest": str(MANIFEST), "previous": current,
                      "available_only_on_explicit_child_request": True, "restored": False}
            save(record)
            winreg.SetValueEx(key, str(MANIFEST), 0, winreg.REG_DWORD, 0)
            record["prepared"] = True
            save(record)
            print(json.dumps({"prepared": True, "manifest": str(MANIFEST), "previous_value_exists": current["exists"]}))
        else:
            record = json.loads(STATE.read_text(encoding="utf-8"))
            if record["manifest"] != str(MANIFEST):
                raise RuntimeError("The saved registration belongs to a different manifest")
            if record.get("restored"):
                print(json.dumps({"restored": True, "already_restored": True}))
                return
            if current != {"exists": True, "data": 0, "kind": winreg.REG_DWORD}:
                raise RuntimeError("The registration changed since preparation; it will not be overwritten")
            previous = record["previous"]
            if previous["exists"]:
                winreg.SetValueEx(key, str(MANIFEST), 0, previous["kind"], previous["data"])
            else:
                winreg.DeleteValue(key, str(MANIFEST))
            record["restored"] = value(key) == previous
            save(record)
            if not record["restored"]:
                raise RuntimeError("The previous registration was not restored")
            print(json.dumps({"restored": True, "previous_value_exists": previous["exists"]}))


if __name__ == "__main__":
    main()
