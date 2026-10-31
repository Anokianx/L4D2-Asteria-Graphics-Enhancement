EFFECTS = {
    "smaa": {
        "group": "quality",
        "title": "SMAA",
        "description": "Smooth jagged geometric edges while preserving detail.",
        "file": "AsteriaSMAA.fx",
        "technique": "SMAA",
        "enabled": True,
        "parameters": [
            {"key": "strength", "label": "EDGE SENSITIVITY", "kind": "slider", "default": 0.42, "min": 0.0, "max": 1.0, "step": 0.01},
            {"key": "quality", "label": "QUALITY", "kind": "choice", "default": "medium", "choices": ("low", "medium", "high")},
        ],
    },
    "cas": {
        "group": "quality",
        "title": "CAS",
        "description": "Adaptive detail sharpening with controlled contrast.",
        "file": "AsteriaCAS.fx",
        "technique": "ContrastAdaptiveSharpen",
        "enabled": True,
        "parameters": [
            {"key": "strength", "label": "SHARPNESS", "kind": "slider", "default": 0.35, "min": 0.0, "max": 1.0, "step": 0.01},
        ],
    },
    "bloom": {
        "group": "color",
        "title": "BLOOM",
        "description": "Soft highlight diffusion with a quarter-resolution blur.",
        "file": "AsteriaBloom.fx",
        "technique": "AsteriaBloom",
        "enabled": False,
        "parameters": [
            {"key": "strength", "label": "INTENSITY", "kind": "slider", "default": 0.35, "min": 0.0, "max": 1.0, "step": 0.01},
            {"key": "threshold", "label": "HIGHLIGHT THRESHOLD", "kind": "slider", "default": 0.75, "min": 0.0, "max": 1.0, "step": 0.01},
            {"key": "radius", "label": "DIFFUSION RADIUS", "kind": "slider", "default": 2.0, "min": 0.5, "max": 6.0, "step": 0.1},
        ],
    },
    "color": {
        "group": "color",
        "title": "COLOR GRADE",
        "description": "Adjust exposure, contrast, color balance and vibrance.",
        "file": "AsteriaColor.fx",
        "technique": "AsteriaColor",
        "enabled": False,
        "parameters": [
            {"key": "exposure", "label": "EXPOSURE / EV", "kind": "slider", "default": 0.0, "min": -2.0, "max": 2.0, "step": 0.05},
            {"key": "contrast", "label": "CONTRAST", "kind": "slider", "default": 1.0, "min": 0.5, "max": 1.5, "step": 0.01},
            {"key": "saturation", "label": "SATURATION", "kind": "slider", "default": 1.0, "min": 0.0, "max": 2.0, "step": 0.01},
            {"key": "temperature", "label": "COOL / WARM", "kind": "slider", "default": 0.0, "min": -1.0, "max": 1.0, "step": 0.02},
            {"key": "vibrance", "label": "VIBRANCE", "kind": "slider", "default": 0.0, "min": -1.0, "max": 1.0, "step": 0.02},
        ],
    },
    "tonemap": {
        "group": "color",
        "title": "TONE MAP",
        "description": "SDR tonal curves with filmic, ACES and Reinhard modes.",
        "file": "AsteriaTonemap.fx",
        "technique": "AsteriaTonemap",
        "enabled": False,
        "parameters": [
            {"key": "strength", "label": "CURVE MIX", "kind": "slider", "default": 0.5, "min": 0.0, "max": 1.0, "step": 0.01},
            {"key": "mode", "label": "CURVE", "kind": "choice", "default": "filmic", "choices": ("filmic", "aces", "reinhard")},
        ],
    },
    "vignette": {
        "group": "finish",
        "title": "VIGNETTE",
        "description": "A soft peripheral falloff that keeps the aiming area clear.",
        "file": "AsteriaVignette.fx",
        "technique": "AsteriaVignette",
        "enabled": False,
        "parameters": [
            {"key": "strength", "label": "INTENSITY", "kind": "slider", "default": 0.2, "min": 0.0, "max": 1.0, "step": 0.01},
            {"key": "radius", "label": "CLEAR RADIUS", "kind": "slider", "default": 0.75, "min": 0.3, "max": 1.0, "step": 0.01},
            {"key": "softness", "label": "EDGE SOFTNESS", "kind": "slider", "default": 0.35, "min": 0.05, "max": 1.0, "step": 0.01},
        ],
    },
    "deband": {
        "group": "quality",
        "title": "DEBAND",
        "description": "Reduce smooth-gradient banding while preserving strong edges.",
        "file": "AsteriaDeband.fx",
        "technique": "AsteriaDeband",
        "enabled": False,
        "parameters": [
            {"key": "strength", "label": "INTENSITY", "kind": "slider", "default": 0.3, "min": 0.0, "max": 1.0, "step": 0.01},
            {"key": "radius", "label": "SAMPLE RADIUS / PX", "kind": "slider", "default": 8.0, "min": 1.0, "max": 24.0, "step": 1.0},
            {"key": "quality", "label": "QUALITY", "kind": "choice", "default": "medium", "choices": ("low", "medium", "high")},
        ],
    },
    "marker": {
        "group": "runtime",
        "title": "PIPELINE MARKER",
        "description": "A cyan screen edge for checking the active rendering path.",
        "file": "Asteria.fx",
        "technique": "AsteriaMarker",
        "enabled": False,
        "parameters": [
            {"key": "strength", "label": "MARKER STRENGTH", "kind": "slider", "default": 0.7, "min": 0.0, "max": 1.0, "step": 0.01},
        ],
    },
}


def default_effect_values():
    return {
        key: {
            "enabled": effect["enabled"],
            **{parameter["key"]: parameter["default"] for parameter in effect["parameters"]},
        }
        for key, effect in EFFECTS.items()
    }
