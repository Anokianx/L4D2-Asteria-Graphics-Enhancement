from pathlib import Path
import json
from PIL import Image, ImageDraw, ImageFont

__author__ = "Anokianx"
ROOT = Path(__file__).resolve().parent


def main():
    output = ROOT / "results/SceneInterpolation"
    report = json.loads((output / "scene_interpolation.json").read_text(encoding="utf-8"))
    if not report.get("passed") or report.get("real_game_inputs"):
        raise ValueError("Expected a passing controlled GPU scene report")
    font_path = Path("C:/Windows/Fonts/segoeui.ttf")
    font = ImageFont.truetype(str(font_path), 20) if font_path.is_file() else ImageFont.load_default()
    small = ImageFont.truetype(str(font_path), 15) if font_path.is_file() else font
    canvas = Image.new("RGB", (1200, 984), "#0B1018")
    draw = ImageDraw.Draw(canvas)
    draw.text((24, 16), "ASTERIA / GPU DEPTH + ESTIMATED MOTION + FSR FRAME INTERPOLATION", font=font, fill="#EEF3FA")
    draw.text((24, 47), "Controlled synthetic scene GPU readbacks / 2x nearest crop / no game or displayed FG claim", font=small, fill="#9AAABD")
    for row, frame in enumerate((1, 4, 7)):
        directory = output / f"frame_{frame}"
        left = 196 + frame * 12
        crop = (left, 100, left + 184, 228)
        top = 112 + row * 292
        for col, (name, label) in enumerate((("previous", "PREVIOUS INPUT"), ("generated", "GENERATED MIDPOINT"), ("current", "CURRENT INPUT"))):
            with Image.open(directory / f"{name}.ppm") as image:
                panel = image.crop(crop).resize((368, 256), Image.Resampling.NEAREST)
            x = 24 + col * 392
            draw.text((x, top - 28), f"PAIR {frame} / {label}", font=small, fill="#F2B576" if col == 1 else "#B9C8D9")
            canvas.paste(panel, (x, top))
    canvas.save(output / "comparison.png")
    print(output / "comparison.png")


if __name__ == "__main__":
    main()
