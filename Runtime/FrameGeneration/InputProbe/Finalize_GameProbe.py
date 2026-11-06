import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import time


KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
KERNEL32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
KERNEL32.OpenProcess.restype = wintypes.HANDLE
KERNEL32.CloseHandle.argtypes = [wintypes.HANDLE]
KERNEL32.CloseHandle.restype = wintypes.BOOL
KERNEL32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
KERNEL32.QueryFullProcessImageNameW.restype = wintypes.BOOL
KERNEL32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
KERNEL32.WaitForSingleObject.restype = wintypes.DWORD
KERNEL32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
KERNEL32.GetExitCodeProcess.restype = wintypes.BOOL


def identity(pid, expected):
    process = KERNEL32.OpenProcess(0x1000 | 0x100000, False, pid)
    if not process:
        error = ctypes.get_last_error()
        ended = error in (87, 1168)
        return {"process_live": False, "confirmed_game_ended": ended, "query_error": error,
                "reason": "process_absent" if ended else "process_identity_unavailable"}
    try:
        waited = KERNEL32.WaitForSingleObject(process, 0)
        if waited == 0:
            code = wintypes.DWORD()
            KERNEL32.GetExitCodeProcess(process, ctypes.byref(code))
            return {"process_live": False, "confirmed_game_ended": True, "exit_code": code.value, "reason": "process_exited"}
        if waited != 258:
            return {"process_live": False, "confirmed_game_ended": False, "reason": "process_wait_unavailable"}
        name = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(name))
        if not KERNEL32.QueryFullProcessImageNameW(process, 0, name, ctypes.byref(length)):
            return {"process_live": False, "confirmed_game_ended": False,
                    "query_error": ctypes.get_last_error(), "reason": "process_path_unavailable"}
        matches = os.path.normcase(os.path.abspath(name.value)) == os.path.normcase(os.path.abspath(expected))
        return {"process_live": matches, "confirmed_game_ended": not matches, "identity_matches": matches,
                "observed_process_path": name.value, "reason": "matching_process" if matches else "pid_identity_changed"}
    finally:
        KERNEL32.CloseHandle(process)


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def main():
    root = Path(__file__).resolve().parent / "results" / "GameProbe"
    parser = argparse.ArgumentParser()
    parser.add_argument("--status", type=Path, default=root / "native_input_status.json")
    parser.add_argument("--game", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--max-age", type=float, default=5.0)
    args = parser.parse_args()
    if not (0 < args.max_age <= 60):
        parser.error("--max-age must be greater than zero and at most 60 seconds")
    source = json.loads(args.status.read_text(encoding="utf-8"))
    if args.game is None:
        launch = json.loads((args.status.parent / "launch_status.json").read_text(encoding="utf-8"))
        game = Path(launch["game_executable"])
    else:
        game = args.game
        if game.is_dir():
            game = game / "left4dead2.exe"
    pid = source.get("pid")
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        raise ValueError("The source report must identify a real game PID")
    now = time.time()
    heartbeat = source.get("heartbeat")
    age = now - heartbeat if isinstance(heartbeat, (float, int)) and not isinstance(heartbeat, bool) else None
    fresh = age is not None and -1 <= age <= args.max_age
    check = identity(pid, game)
    telemetry_live = check["process_live"] and fresh and source.get("session_active") is True and source.get("resources_are_live") is True
    summary = {"author": "Anokianx", "schema_version": 1, "method": "external_process_identity_and_heartbeat_check",
               "source_pid": pid, "expected_game_path": str(game), "checked_at": now, "heartbeat_age_seconds": age,
               "heartbeat_fresh": fresh, "telemetry_live": telemetry_live, "native_fg_ready": False,
               "cached_source_session_active": source.get("session_active"),
               "cached_source_resources_are_live": source.get("resources_are_live"), "addon_uninit_verified": False,
               **check}
    if check["confirmed_game_ended"]:
        final = dict(source)
        final["session_active"] = False
        final["resources_are_live"] = False
        final["native_fg_ready"] = False
        final["finalization"] = {**summary, "resource_liveness": "historical"}
        destination = args.output or args.status.parent / "final_observation.json"
        write(destination, final)
        summary["final_observation"] = str(destination)
        finalized_streams = []
        for relative in ("Provenance/status.json", "NativeUniform/status.json", "mapped_layer_status.json"):
            stream = args.status.parent / relative
            if not stream.exists():
                continue
            data = json.loads(stream.read_text(encoding="utf-8"))
            if data.get("pid") != pid:
                raise ValueError("Auxiliary diagnostic stream has a different process identity: " + relative)
            data["session_active"] = False
            data["resources_are_live"] = False
            data["native_fg_ready"] = False
            data["finalization"] = {**summary, "resource_liveness": "historical"}
            target = stream.parent / ("final_observation.json" if stream.name == "status.json" else "mapped_layer_final_observation.json")
            write(target, data)
            finalized_streams.append({"source": str(stream), "final_observation": str(target), "resource_liveness": "historical"})
        summary["auxiliary_streams"] = finalized_streams
    write(args.status.parent / "finalization_status.json", summary)
    print(json.dumps(summary, ensure_ascii=True))
    return 0 if check["confirmed_game_ended"] or telemetry_live else 2


if __name__ == "__main__":
    raise SystemExit(main())
