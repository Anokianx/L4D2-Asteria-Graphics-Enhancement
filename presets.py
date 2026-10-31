from __future__ import annotations

import json
from pathlib import Path

class PresetStore:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def save(self, name: str, values: dict):
        path = self.root / (str(name).strip() or "default")
        path = path.with_suffix(".json")
        path.write_text(json.dumps(values, ensure_ascii=False, indent=2), encoding="utf-8")
        return path

    def load(self, name: str):
        path = (self.root / str(name)).with_suffix(".json")
        if not path.exists():
            return {}
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            return {}
