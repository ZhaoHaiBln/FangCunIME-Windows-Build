#!/usr/bin/env python3
"""Rebuild the two external skins from the preserved 1.14.21 materials.

This is a format conversion, not a new skin design.  Pillow is only needed by
this authoring tool; the apps and the standard fcskin validator do not need it.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
RELEASE = ROOT / "skin-format/releases/file-only-v1"
SOURCE = RELEASE / "sources"
SNAPSHOT_COMMIT = "3626f9d"
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("fcskin_authoring", Path(__file__).with_name("fcskin.py"))
assert spec and spec.loader
fcskin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fcskin)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def rgba(value: str, alpha: int = 255) -> tuple[int, int, int, int]:
    return tuple(bytes.fromhex(value[1:])) + (alpha,)


def save_png(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.convert("RGBA").save(path, format="PNG", optimize=False, compress_level=9)


def nine_patch(path: Path) -> tuple[Image.Image, list[int]]:
    """Remove only the Android marker border, retaining every content pixel."""
    with Image.open(path) as source:
        source = source.convert("RGBA")
    width, height = source.size
    def extent(pixels):
        active = [i for i, value in enumerate(pixels) if value == (0, 0, 0, 255)]
        assert active and active == list(range(active[0], active[-1] + 1)), "Expected one stretch span"
        return active[0], active[-1] + 1
    left, end_x = extent([source.getpixel((x, 0)) for x in range(1, width - 1)])
    top, end_y = extent([source.getpixel((0, y)) for y in range(1, height - 1)])
    content = source.crop((1, 1, width - 1, height - 1))
    insets = [left, top, content.width - end_x, content.height - end_y]
    assert all(v >= 0 for v in insets)
    assert left + insets[2] < content.width and top + insets[3] < content.height
    return content, insets


def lighting(image: Image.Image, multiplier: str) -> Image.Image:
    """The existing LightingColorFilter RGB multiplication, with alpha unchanged."""
    channels = image.convert("RGBA").split()
    factors = bytes.fromhex(multiplier.removeprefix("#"))
    tinted = [channel.point([round(value * factor / 255) for value in range(256)])
              for channel, factor in zip(channels[:3], factors)]
    return Image.merge("RGBA", (*tinted, channels[3]))


def gradient(top: str, bottom: str, size: tuple[int, int]) -> Image.Image:
    a, b = rgba(top), rgba(bottom)
    line = Image.new("RGBA", (1, size[1]))
    line.putdata([tuple(round(x + (y - x) * row / max(1, size[1] - 1)) for x, y in zip(a, b))
                  for row in range(size[1])])
    return line.resize(size, Image.Resampling.BILINEAR)


def grass_selection(color: str) -> Image.Image:
    # Exact referenceCandidate parameters, sampled at assetScale=4: 2dp inset,
    # 4dp radius, alpha .94 -> 1 and a .6dp white stroke at alpha .45.
    scale, sample = 4, 5
    width, height = 64 * scale * sample, 32 * scale * sample
    image = Image.new("RGBA", (width, height))
    mask = Image.new("L", image.size)
    draw = ImageDraw.Draw(mask)
    inset = 2 * scale * sample
    draw.rounded_rectangle((inset, inset, width - inset - 1, height - inset - 1), radius=4 * scale * sample, fill=255)
    rgb = rgba(color)[:3]
    face = Image.new("RGBA", image.size)
    rows = ImageDraw.Draw(face)
    for y in range(height):
        factor = min(1, max(0, (y - inset) / max(1, height - inset * 2 - 1)))
        rows.line((0, y, width, y), fill=rgb + (round(255 * (.94 + .06 * factor)),))
    face.putalpha(ImageChops.multiply(face.getchannel("A"), mask))
    stroke = Image.new("RGBA", image.size, (255, 255, 255, 0))
    # Android Stroke is centered on the path; Pillow outlines are inside it.
    # Build the centered ring explicitly at 5x sampling instead.
    outer = Image.new("L", image.size)
    inner = Image.new("L", image.size)
    half = round(.3 * scale * sample)
    ImageDraw.Draw(outer).rounded_rectangle(
        (inset - half, inset - half, width - inset + half - 1, height - inset + half - 1),
        radius=4 * scale * sample + half, fill=255)
    ImageDraw.Draw(inner).rounded_rectangle(
        (inset + half, inset + half, width - inset - half - 1, height - inset - half - 1),
        radius=4 * scale * sample - half, fill=255)
    stroke.putalpha(ImageChops.subtract(outer, inner).point(lambda value: round(value * .45)))
    image = Image.alpha_composite(face, stroke)
    return image.resize((width // sample, height // sample), Image.Resampling.LANCZOS)


def role(file: str, insets=None, scale="nineSlice") -> dict:
    return {"file": file, "scale": scale, "insets": insets or [0, 0, 0, 0]}


def nine_slice(image: Image.Image, insets: list[int], size: tuple[int, int], density=2) -> Image.Image:
    # Same edge units as the runtime: source assetScale=4, preview density=2.
    left, top, right, bottom = insets
    factor = min(1, size[0] / max(1, (left + right) * density / 4),
                 size[1] / max(1, (top + bottom) * density / 4))
    xs = [0, left, image.width - right, image.width]
    ys = [0, top, image.height - bottom, image.height]
    dx = [0, round(left * density / 4 * factor), size[0] - round(right * density / 4 * factor), size[0]]
    dy = [0, round(top * density / 4 * factor), size[1] - round(bottom * density / 4 * factor), size[1]]
    target = Image.new("RGBA", size)
    for y in range(3):
        for x in range(3):
            if xs[x + 1] > xs[x] and ys[y + 1] > ys[y] and dx[x + 1] > dx[x] and dy[y + 1] > dy[y]:
                patch = image.crop((xs[x], ys[y], xs[x + 1], ys[y + 1]))
                patch = patch.resize((dx[x + 1] - dx[x], dy[y + 1] - dy[y]), Image.Resampling.BILINEAR)
                target.alpha_composite(patch, (dx[x], dy[y]))
    return target


def material_preview(manifest: dict, folder: Path) -> Image.Image:
    """Source-material contact sheet; deliberately not a fake device screenshot."""
    output = Image.new("RGBA", (760, 280), "#FFFFFF")
    # Preview text lives only in the declared preview, never in material roles.
    font_path = Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")
    if not font_path.is_file():
        raise RuntimeError("Authoring previews require fonts-noto-cjk; do not replace Chinese labels with missing glyph boxes")
    font = ImageFont.truetype(str(font_path), 22)
    small = ImageFont.truetype(str(font_path), 17)
    for panel, tone in enumerate(("light", "dark")):
        theme = manifest["themes"][tone]
        colors = theme["colors"]
        ox = panel * 380
        ImageDraw.Draw(output).rectangle((ox, 0, ox + 379, 279), fill=colors["background"])
        draw = ImageDraw.Draw(output)
        draw.text((ox + 16, 10), ("浅色" if tone == "light" else "深色") + " · " + manifest["name"], fill=colors["text"], font=small)
        draw.rectangle((ox + 12, 46, ox + 367, 90), fill=colors["toolbar"])
        draw.text((ox + 22, 53), "26键     九宫     符号", fill=colors["text"], font=font)
        for index, (name, title, fill) in enumerate((("keyNormal", "a", "surface"), ("keyFunction", "123", "function"), ("keyAction", "发送", "accent"))):
            width, height = 104, 132
            position = (ox + 16 + index * 120, 108)
            item = theme["images"].get(name)
            if item:
                with Image.open(folder / item["file"]) as source:
                    cap = nine_slice(source.convert("RGBA"), item["insets"], (width, height))
                output.alpha_composite(cap, position)
            else:
                # File-only Brutal drawing: 2dp shadow and 1.5dp stroke.  v1 has
                # no independent shadow color, so both use the package border.
                x, y = position
                draw.rectangle((x + 4, y + 4, x + width - 1, y + height - 1), fill=colors["border"])
                draw.rectangle((x, y, x + width - 5, y + height - 5), fill=colors[fill], outline=colors["border"], width=3)
            color = colors["onAccent"] if name == "keyAction" else colors["functionText"] if name == "keyFunction" else colors["keyText"]
            draw.text((position[0] + width / 2 - 2, position[1] + height / 2 - 2), title, fill=color, font=font, anchor="mm")
        draw.text((ox + 16, 250), "原素材预览 · 非设备截图", fill=colors["muted"], font=small)
    return output


def export(output: Path) -> dict:
    baselines = json.loads((SOURCE / "palettes-v1.json").read_text(encoding="utf-8"))
    records = []
    for key, file in {"keyNormal": "ime_key_white.9.png", "keyFunction": "ime_key_grass.9.png", "code": "ime_input_grass.9.png", "candidates": "ime_candidates_grass.9.png"}.items():
        p = SOURCE / "grass" / file
        image, insets = nine_patch(p)
        # Positive conversion checks: marker removal is the only light-mode edit.
        with Image.open(p) as original:
            assert image.tobytes() == original.convert("RGBA").crop((1, 1, original.width - 1, original.height - 1)).tobytes()
        records.append({"role": key, "source": str(p.relative_to(ROOT)), "sourceSha256": sha(p.read_bytes()), "contentSize": list(image.size), "insets": insets})
    font = Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")
    report = {"sourceCommit": SNAPSHOT_COMMIT, "sourcePaletteSha256": sha((SOURCE / "palettes-v1.json").read_bytes()),
              "previewFontSha256": sha(font.read_bytes()), "ninePatchSources": records, "packages": []}
    output.mkdir(parents=True, exist_ok=True)
    for key, baseline in baselines.items():
        identifier = "fangcun.grass-mechanical" if key == "grass" else "fangcun.geek-mechanical"
        manifest = {"format": "fangcun.skin", "schemaVersion": 1, "id": identifier, "version": 1,
                    "author": "方寸", "license": "方寸项目原素材；保留原有使用范围", "preview": "preview.png", **copy.deepcopy(baseline)}
        for theme in manifest["themes"].values():
            theme["images"] = {}
        with tempfile.TemporaryDirectory(prefix="fangcun-export-") as temporary:
            folder = Path(temporary)
            if key == "grass":
                for tone, theme in manifest["themes"].items():
                    for item in records:
                        name = item["role"]
                        image, insets = nine_patch(ROOT / item["source"])
                        if tone == "dark":
                            multiplier = "#3B403E" if name == "keyNormal" else "#B7C69C" if name == "keyFunction" else "#3C423E"
                            tinted = lighting(image, multiplier)
                            assert tinted.getchannel("A").tobytes() == image.getchannel("A").tobytes()
                            image = tinted
                        filename = tone + "/" + name.lower() + ".png"
                        save_png(image, folder / filename)
                        theme["images"][name] = role(filename, insets)
                    theme["images"]["keyAction"] = copy.deepcopy(theme["images"]["keyFunction"])
                    save_png(gradient(theme["colors"]["background"], theme["colors"]["toolbar"], (1, 256)), folder / tone / "deck.png")
                    theme["images"]["deck"] = role(tone + "/deck.png", scale="stretch")
                    save_png(grass_selection(theme["colors"]["selection"]), folder / tone / "selection.png")
                    theme["images"]["selection"] = role(tone + "/selection.png", [24, 24, 24, 24])
            (folder / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            preview = material_preview(manifest, folder)
            save_png(preview, folder / "preview.png")
            save_png(preview, output / (identifier + "-preview.png"))
            package = output / (identifier + ".fcskin")
            _, digest = fcskin.build(folder, package)
            actual, actual_digest = fcskin.validate_archive(package.read_bytes())
            assert actual == manifest and actual_digest == digest
            report["packages"].append({"file": package.name, "id": identifier, "bytes": package.stat().st_size, "sha256": digest,
                                       "imageRoles": {tone: sorted(theme["images"]) for tone, theme in manifest["themes"].items()},
                                       "normalPngOnly": True, "lightPixelIdentityAndDarkAlphaChecked": key == "grass"})
    (output / "export-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=RELEASE)
    options = parser.parse_args()
    print(json.dumps(export(options.output), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
