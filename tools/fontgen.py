#!/usr/bin/env python3

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


DEFAULT_TTF = Path("tools/Cantarell-Regular.ttf")
DEFAULT_OUTPUT = Path("kernel/user_programs/generated_font.zig")
DEFAULT_FIRST = 0x20
DEFAULT_LAST = 0x7E


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate a grayscale Zig bitmap font from a TTF file.")
    parser.add_argument("--ttf", type=Path, default=DEFAULT_TTF, help="Path to the source TTF file.")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Output Zig file path.")
    parser.add_argument("--cell-width", type=int, default=16, help="Per-glyph cell width in pixels.")
    parser.add_argument("--cell-height", type=int, default=24, help="Per-glyph cell height in pixels.")
    parser.add_argument("--supersample", type=int, default=3, help="Render glyphs at N times target size before downsampling.")
    parser.add_argument("--first", type=lambda value: int(value, 0), default=DEFAULT_FIRST, help="First codepoint to include.")
    parser.add_argument("--last", type=lambda value: int(value, 0), default=DEFAULT_LAST, help="Last codepoint to include.")
    return parser.parse_args()


def load_font_that_fits(
    ttf_path: Path,
    cell_width: int,
    cell_height: int,
    supersample: int,
    codepoints: list[int],
) -> tuple[ImageFont.FreeTypeFont, int, tuple[int, int, int, int]]:
    hi_cell_width = cell_width * supersample
    hi_cell_height = cell_height * supersample
    for size in range(hi_cell_height, 3, -1):
        font = ImageFont.truetype(str(ttf_path), size=size, layout_engine=ImageFont.Layout.BASIC)
        min_left = hi_cell_width
        min_top = hi_cell_height
        max_right = 0
        max_bottom = 0
        for codepoint in codepoints:
            ch = chr(codepoint)
            left, top, right, bottom = font.getbbox(ch, anchor="ls")
            min_left = min(min_left, left)
            min_top = min(min_top, top)
            max_right = max(max_right, right)
            max_bottom = max(max_bottom, bottom)
        if (max_right - min_left) <= hi_cell_width and (max_bottom - min_top) <= hi_cell_height:
            return font, size, (min_left, min_top, max_right, max_bottom)
    raise RuntimeError(f"Could not fit {ttf_path} into {cell_width}x{cell_height} cells")


def rasterize_glyphs(
    font: ImageFont.FreeTypeFont,
    cell_width: int,
    cell_height: int,
    supersample: int,
    union_bbox: tuple[int, int, int, int],
    codepoints: list[int],
) -> list[dict[str, int | list[int]]]:
    min_left, min_top, max_right, max_bottom = union_bbox
    ascent_hi = -min_top
    line_height_lo = math.ceil((max_bottom - min_top) / supersample)
    glyphs: list[dict[str, int | list[int]]] = []

    for codepoint in codepoints:
        ch = chr(codepoint)
        left, top, right, bottom = font.getbbox(ch, anchor="ls")
        width_hi = max(0, right - left)
        height_hi = max(0, bottom - top)
        advance_lo = math.ceil(font.getlength(ch) / supersample)
        bearing_x_lo = math.floor(left / supersample)
        top_offset_lo = max(0, math.floor((top + ascent_hi) / supersample))

        if width_hi == 0 or height_hi == 0:
            glyphs.append(
                {
                    "codepoint": codepoint,
                    "advance": advance_lo,
                    "bearing_x": bearing_x_lo,
                    "top_offset": top_offset_lo,
                    "width": 0,
                    "height": 0,
                    "mask": [],
                }
            )
            continue

        glyph_hi = Image.new("L", (width_hi, height_hi), 0)
        draw = ImageDraw.Draw(glyph_hi)
        draw.text((-left, -top), ch, font=font, fill=255, anchor="ls")
        width_lo = max(1, math.ceil(width_hi / supersample))
        height_lo = max(1, math.ceil(height_hi / supersample))
        glyph = glyph_hi.resize((width_lo, height_lo), Image.Resampling.LANCZOS)
        bbox = glyph.getbbox()
        if bbox is None:
            glyphs.append(
                {
                    "codepoint": codepoint,
                    "advance": advance_lo,
                    "bearing_x": bearing_x_lo,
                    "top_offset": top_offset_lo,
                    "width": 0,
                    "height": 0,
                    "mask": [],
                }
            )
            continue

        crop_left, crop_top, crop_right, crop_bottom = bbox
        glyph = glyph.crop(bbox)
        glyphs.append(
            {
                "codepoint": codepoint,
                "advance": advance_lo,
                "bearing_x": bearing_x_lo + crop_left,
                "top_offset": min(line_height_lo, top_offset_lo + crop_top),
                "width": crop_right - crop_left,
                "height": crop_bottom - crop_top,
                "mask": list(glyph.tobytes()),
            }
        )

    return glyphs


def emit_zig(
    output_path: Path,
    source_ttf: Path,
    cell_width: int,
    cell_height: int,
    supersample: int,
    font_size: int,
    line_height: int,
    console_advance: int,
    glyphs: list[dict[str, int | list[int]]],
) -> None:
    fallback_index = next(index for index, glyph in enumerate(glyphs) if glyph["codepoint"] == ord("?"))
    mask_bytes: list[int] = []
    lines: list[str] = []
    lines.append("// Generated by tools/fontgen.py. Do not edit manually.")
    lines.append(f'pub const source_ttf = "{source_ttf.as_posix()}";')
    lines.append(f"pub const source_font_size: usize = {font_size};")
    lines.append(f"pub const cell_width: usize = {cell_width};")
    lines.append(f"pub const cell_height: usize = {cell_height};")
    lines.append(f"pub const supersample: usize = {supersample};")
    lines.append(f"pub const line_height: usize = {line_height};")
    lines.append(f"pub const console_advance: usize = {console_advance};")
    lines.append(f"pub const fallback_index: usize = {fallback_index};")
    lines.append("")
    lines.append("pub const Glyph = struct {")
    lines.append("    codepoint: u21,")
    lines.append("    advance: u8,")
    lines.append("    bearing_x: i8,")
    lines.append("    top_offset: u8,")
    lines.append("    width: u8,")
    lines.append("    height: u8,")
    lines.append("    mask_offset: u32,")
    lines.append("};")
    lines.append("")
    lines.append("pub const glyphs = [_]Glyph{")
    for glyph in glyphs:
        offset = len(mask_bytes)
        mask = glyph["mask"]
        mask_bytes.extend(mask)
        lines.append(
            "    .{ "
            f".codepoint = 0x{glyph['codepoint']:04X}, "
            f".advance = {glyph['advance']}, "
            f".bearing_x = {glyph['bearing_x']}, "
            f".top_offset = {glyph['top_offset']}, "
            f".width = {glyph['width']}, "
            f".height = {glyph['height']}, "
            f".mask_offset = {offset} "
            "},"
        )
    lines.append("};")
    lines.append("")
    lines.append("pub const glyph_mask_bytes = [_]u8{")
    for index, byte in enumerate(mask_bytes):
        if index % 16 == 0:
            lines.append("    ")
        lines[-1] += f"{byte}, "
    if lines[-1] == "    ":
        lines.pop()
    lines.append("};")
    lines.append("")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    if not args.ttf.is_file():
        raise FileNotFoundError(f"TTF not found: {args.ttf}")
    if args.first > args.last:
        raise ValueError("--first must be <= --last")

    codepoints = list(range(args.first, args.last + 1))
    if ord("?") not in codepoints:
        raise ValueError("The selected codepoint range must include '?' for fallback glyphs")

    if args.supersample <= 0:
        raise ValueError("--supersample must be >= 1")

    font, font_size, union_bbox = load_font_that_fits(
        args.ttf,
        args.cell_width,
        args.cell_height,
        args.supersample,
        codepoints,
    )
    glyphs = rasterize_glyphs(
        font,
        args.cell_width,
        args.cell_height,
        args.supersample,
        union_bbox,
        codepoints,
    )
    min_left, min_top, max_right, max_bottom = union_bbox
    line_height = math.ceil((max_bottom - min_top) / args.supersample)
    sorted_advances = sorted(int(glyph["advance"]) for glyph in glyphs)
    console_advance = sorted_advances[(len(sorted_advances) * 9) // 10]
    emit_zig(
        args.output,
        args.ttf,
        args.cell_width,
        args.cell_height,
        args.supersample,
        font_size,
        line_height,
        console_advance,
        glyphs,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
