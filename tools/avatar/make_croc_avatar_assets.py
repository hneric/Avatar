#!/usr/bin/env python3
"""
Generate LVGL C image assets for the crocodile avatar.

The script keeps every layer on the same full-size transparent canvas. This
makes LVGL alignment simple: all image objects can share the same top-left
position and only the mouth/blink overlay source changes.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Iterable

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError as exc:
    raise SystemExit("Pillow is required: pip install pillow") from exc


CANONICAL_SIZE = 180
DEFAULT_TARGET_SIZE = 270

# Coordinates are normalized to a 180x180 canvas.
# The new head-only crocodile image is the preferred profile for the device UI.
PROFILES = {
    "head": {
        "mouth_patch": (36, 102, 148, 158),
        "mouth_center": (90, 128),
        "left_eye_box": (42, 58, 76, 98),
        "right_eye_box": (105, 58, 139, 98),
        "patch_fill": (138, 221, 39, 245),
        "patch_highlight": (176, 243, 59, 150),
        "patch_shadow": (56, 142, 35, 90),
        "mouth_scale": 1.22,
    },
    "full_body": {
        "mouth_patch": (50, 62, 130, 106),
        "mouth_center": (88, 84),
        "left_eye_box": (43, 31, 74, 63),
        "right_eye_box": (90, 31, 121, 63),
        "patch_fill": (251, 226, 105, 245),
        "patch_highlight": (255, 238, 139, 170),
        "patch_shadow": (132, 182, 49, 80),
        "mouth_scale": 1.0,
    },
}


def scale_box(box: tuple[int, int, int, int], scale: float) -> tuple[int, int, int, int]:
    return tuple(int(round(v * scale)) for v in box)  # type: ignore[return-value]


def scale_point(point: tuple[int, int], scale: float) -> tuple[int, int]:
    return int(round(point[0] * scale)), int(round(point[1] * scale))


def rounded_rect(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], radius: int, fill) -> None:
    draw.rounded_rectangle(box, radius=radius, fill=fill)


def ellipse(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], fill, outline=None, width: int = 1) -> None:
    draw.ellipse(box, fill=fill, outline=outline, width=width)


def make_base(src: Image.Image, scale: float, profile: dict) -> Image.Image:
    base = src.copy()
    draw = ImageDraw.Draw(base, "RGBA")

    # Cover the original open mouth with a soft cheek/chin patch. This is
    # intentionally simple and localized; the overlay mouths then define the
    # visible expression.
    patch = scale_box(profile["mouth_patch"], scale)
    fill = profile["patch_fill"]
    highlight = profile["patch_highlight"]
    shadow = profile["patch_shadow"]
    rounded_rect(draw, patch, int(18 * scale), fill)
    x0, y0, x1, y1 = patch
    ellipse(draw, (x0 + int(4 * scale), y0 + int(4 * scale), x1 - int(4 * scale), y1 + int(20 * scale)),
            highlight)
    draw.arc((x0 + int(10 * scale), y0 + int(8 * scale), x1 - int(10 * scale), y1 + int(24 * scale)),
             190, 340, fill=shadow, width=max(1, int(2 * scale)))
    return base


def transparent(size: int) -> Image.Image:
    return Image.new("RGBA", (size, size), (0, 0, 0, 0))


def draw_mouth(size: int, scale: float, profile: dict, variant: int) -> Image.Image:
    img = transparent(size)
    draw = ImageDraw.Draw(img, "RGBA")
    cx, cy = scale_point(profile["mouth_center"], scale)
    mouth_scale = scale * profile.get("mouth_scale", 1.0)

    dark = (83, 24, 26, 245)
    lip = (111, 45, 28, 220)
    tongue = (245, 104, 104, 235)
    tooth = (255, 255, 235, 235)

    if variant == 0:
        box = (cx - int(34 * mouth_scale), cy - int(4 * mouth_scale),
               cx + int(34 * mouth_scale), cy + int(8 * mouth_scale))
        draw.arc(box, 8, 172, fill=lip, width=max(2, int(3 * mouth_scale)))
        return img

    shapes = {
        1: (46, 18, 16, False),
        2: (58, 28, 18, True),
        3: (66, 42, 22, True),
        4: (78, 34, 24, True),
    }
    w, h, radius, show_tongue = shapes[variant]
    box = (
        cx - int(w * mouth_scale / 2),
        cy - int(h * mouth_scale / 2),
        cx + int(w * mouth_scale / 2),
        cy + int(h * mouth_scale / 2),
    )
    rounded_rect(draw, box, int(radius * mouth_scale), dark)
    draw.arc((box[0], box[1] - int(4 * mouth_scale), box[2], box[3] + int(5 * mouth_scale)),
             184, 356, fill=(255, 231, 132, 180), width=max(1, int(2 * mouth_scale)))
    if show_tongue:
        tongue_box = (
            cx - int(w * mouth_scale * 0.26),
            cy + int(h * mouth_scale * 0.10),
            cx + int(w * mouth_scale * 0.26),
            cy + int(h * mouth_scale * 0.48),
        )
        rounded_rect(draw, tongue_box, int(10 * mouth_scale), tongue)
    if variant >= 3:
        ellipse(draw, (cx - int(24 * mouth_scale), cy - int(16 * mouth_scale),
                       cx - int(14 * mouth_scale), cy - int(4 * mouth_scale)), tooth)
        ellipse(draw, (cx + int(14 * mouth_scale), cy - int(16 * mouth_scale),
                       cx + int(24 * mouth_scale), cy - int(4 * mouth_scale)), tooth)
    return img


def draw_blink(size: int, scale: float, profile: dict) -> Image.Image:
    img = transparent(size)
    draw = ImageDraw.Draw(img, "RGBA")
    green = (127, 209, 47, 245)
    line = (22, 79, 26, 230)
    for box in (profile["left_eye_box"], profile["right_eye_box"]):
        x0, y0, x1, y1 = scale_box(box, scale)
        ellipse(draw, (x0 - int(2 * scale), y0, x1 + int(2 * scale), y1), green)
        draw.arc((x0, y0 + int(8 * scale), x1, y1 - int(3 * scale)), 12, 168, fill=line, width=max(2, int(3 * scale)))
    return img


def draw_thinking(size: int, scale: float, profile: dict) -> Image.Image:
    img = transparent(size)
    draw = ImageDraw.Draw(img, "RGBA")
    brow = (20, 74, 23, 235)
    lx0, ly0, lx1, ly1 = scale_box(profile["left_eye_box"], scale)
    rx0, ry0, rx1, ry1 = scale_box(profile["right_eye_box"], scale)
    draw.line((lx0, ly0 - int(4 * scale), lx1, ly0 + int(2 * scale)), fill=brow, width=max(2, int(4 * scale)))
    draw.line((rx0, ry0 + int(2 * scale), rx1, ry0 - int(4 * scale)), fill=brow, width=max(2, int(4 * scale)))
    return img


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xF8) << 3) | (b >> 3)


def image_to_rgb565a8_bytes(img: Image.Image) -> list[int]:
    data: list[int] = []
    alpha: list[int] = []
    rgba = img.convert("RGBA")
    for r, g, b, a in rgba.getdata():
        c = rgb565(r, g, b)
        data.append(c & 0xFF)
        data.append((c >> 8) & 0xFF)
        alpha.append(a)
    return data + alpha


def c_array(values: Iterable[int], name: str) -> str:
    out = [f"static const uint8_t {name}_map[] = {{"]
    line: list[str] = []
    for i, value in enumerate(values):
        line.append(f"0x{value:02x}")
        if len(line) == 16:
            out.append("    " + ",".join(line) + ",")
            line = []
    if line:
        out.append("    " + ",".join(line) + ",")
    out.append("};")
    return "\n".join(out)


def image_dsc(name: str, size: int) -> str:
    return f"""
const lv_image_dsc_t {name} = {{
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_RGB565A8,
  .header.flags = 0,
  .header.w = {size},
  .header.h = {size},
  .header.stride = {size * 2},
  .data_size = sizeof({name}_map),
  .data = {name}_map,
}};
""".strip()


def write_c_assets(layers: dict[str, Image.Image], out_dir: Path, size: int) -> None:
    h_path = out_dir / "croc_avatar_assets.h"
    c_path = out_dir / "croc_avatar_assets.c"
    names = list(layers.keys())

    h_lines = [
        "#pragma once",
        '#include "lvgl.h"',
        "",
    ]
    for name in names:
        h_lines.append(f"extern const lv_image_dsc_t {name};")
    h_path.write_text("\n".join(h_lines) + "\n", encoding="utf-8")

    c_lines = [
        '#include "croc_avatar_assets.h"',
        "",
    ]
    for name, img in layers.items():
        c_lines.append(c_array(image_to_rgb565a8_bytes(img), name))
        c_lines.append("")
        c_lines.append(image_dsc(name, size))
        c_lines.append("")
    c_path.write_text("\n".join(c_lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", default="tools/avatar/croc_source.png", help="source crocodile PNG")
    parser.add_argument("--out-dir", default="firmware/main/avatar_assets", help="output directory")
    parser.add_argument("--size", type=int, default=DEFAULT_TARGET_SIZE, help="square output size")
    parser.add_argument("--profile", choices=sorted(PROFILES.keys()), default="head", help="coordinate profile")
    args = parser.parse_args()

    src_path = Path(args.src)
    out_dir = Path(args.out_dir)
    preview_dir = out_dir / "preview"
    if not src_path.exists():
        raise SystemExit(f"source image not found: {src_path}")

    out_dir.mkdir(parents=True, exist_ok=True)
    preview_dir.mkdir(parents=True, exist_ok=True)

    scale = args.size / CANONICAL_SIZE
    profile = PROFILES[args.profile]
    src = Image.open(src_path).convert("RGBA").resize((args.size, args.size), Image.Resampling.LANCZOS)

    layers = {
        "croc_avatar_base": make_base(src, scale, profile),
        "croc_avatar_mouth_0": draw_mouth(args.size, scale, profile, 0),
        "croc_avatar_mouth_1": draw_mouth(args.size, scale, profile, 1),
        "croc_avatar_mouth_2": draw_mouth(args.size, scale, profile, 2),
        "croc_avatar_mouth_3": draw_mouth(args.size, scale, profile, 3),
        "croc_avatar_mouth_4": draw_mouth(args.size, scale, profile, 4),
        "croc_avatar_blink": draw_blink(args.size, scale, profile),
        "croc_avatar_thinking": draw_thinking(args.size, scale, profile),
    }

    for name, img in layers.items():
        img.save(preview_dir / f"{name}.png")
    write_c_assets(layers, out_dir, args.size)
    print(f"generated {len(layers)} layers in {out_dir}")


if __name__ == "__main__":
    main()
