from pathlib import Path
import json
import sys

from PIL import Image, ImageDraw, ImageFont


def render(directory):
    directory = Path(directory)
    report = json.loads((directory / "interpolation-probe.json").read_text(encoding="utf-8"))
    sample = report["frames"][-1]
    canvas = Image.new("RGB", (1960, 450), "#080b13")
    draw = ImageDraw.Draw(canvas)
    font_path = Path("C:/Windows/Fonts/segoeui.ttf")
    title_font = ImageFont.truetype(str(font_path), 25)
    detail_font = ImageFont.truetype(str(font_path), 18)
    labels = (
        ("Previous real frame", "previous", sample["previous_center_x"]),
        ("AMD GPU-generated intermediate", "generated", sample["generated_center_x"]),
        ("Current real frame", "current", sample["current_center_x"]),
    )
    for index, (title, filename, center) in enumerate(labels):
        source = Image.open(directory / f"{filename}.ppm").convert("RGB")
        source.save(directory / f"{filename}.png")
        x = 10 + index * 650
        draw.text((x + 10, 12), title, font=title_font, fill="#f1f4fc")
        draw.text((x + 10, 45), f"Object center: {center:.1f} px", font=detail_font, fill="#99afd0")
        canvas.paste(source, (x, 80))
    canvas.save(directory / "comparison.png")
    print(directory / "comparison.png")


if __name__ == "__main__":
    render(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parent / "results/interpolation")
