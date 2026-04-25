#!/usr/bin/env python3

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


DEFAULT_TTF = Path("tools/Cantarell-Regular.ttf")
DEFAULT_OUTPUT = Path(".artifacts/font-atlas/font_atlas.rs")
DEFAULT_FIRST = 0x20
DEFAULT_LAST = 0x7E


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate a fixed-cell alpha font atlas as a Rust module.")
    parser.add_argument("--ttf", type=Path, default=DEFAULT_TTF, help="Path to the source TTF file.")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Output Rust file path.")
    parser.add_argument("--font-name", default=None, help="Logical font name to write into the generated module.")
    parser.add_argument("--cell-width", type=int, default=16, help="Per-glyph cell width in pixels.")
    parser.add_argument("--cell-height", type=int, default=24, help="Per-glyph cell height in pixels.")
    parser.add_argument("--columns", type=int, default=16, help="Atlas cell columns.")
    parser.add_argument("--supersample", type=int, default=3, help="Render glyphs at N times target size before downsampling.")
    parser.add_argument("--first", type=lambda value: int(value, 0), default=DEFAULT_FIRST, help="First codepoint to include.")
    parser.add_argument("--last", type=lambda value: int(value, 0), default=DEFAULT_LAST, help="Last codepoint to include.")
    parser.add_argument("--chars", default=None, help="Explicit characters to include instead of a contiguous codepoint range.")
    return parser.parse_args()


def rust_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


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
            left, top, right, bottom = font.getbbox(chr(codepoint), anchor="ls")
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
) -> tuple[list[dict[str, int | list[int]]], int]:
    min_left, min_top, _, max_bottom = union_bbox
    ascent_hi = -min_top
    line_height = math.ceil((max_bottom - min_top) / supersample)
    glyphs: list[dict[str, int | list[int]]] = []

    for codepoint in codepoints:
        ch = chr(codepoint)
        left, top, right, bottom = font.getbbox(ch, anchor="ls")
        width_hi = max(0, right - left)
        height_hi = max(0, bottom - top)
        advance = math.ceil(font.getlength(ch) / supersample)
        bearing_x = math.floor(left / supersample)
        line_bottom = max(0, min(line_height, math.ceil((bottom + ascent_hi) / supersample)))

        glyph_mask: list[int] = []
        glyph_width = 0
        glyph_height = 0
        top_offset = line_bottom
        final_bearing_x = bearing_x

        if width_hi != 0 and height_hi != 0:
            glyph_hi = Image.new("L", (width_hi, height_hi), 0)
            draw = ImageDraw.Draw(glyph_hi)
            draw.text((-left, -top), ch, font=font, fill=255, anchor="ls")
            width_lo = max(1, math.ceil(width_hi / supersample))
            height_lo = max(1, math.ceil(height_hi / supersample))
            glyph = glyph_hi.resize((width_lo, height_lo), Image.Resampling.LANCZOS)
            bbox = glyph.getbbox()
            if bbox is not None:
                crop_left, crop_top, crop_right, crop_bottom = bbox
                glyph = glyph.crop(bbox)
                glyph_width = crop_right - crop_left
                glyph_height = crop_bottom - crop_top
                final_bearing_x = bearing_x + crop_left
                top_offset = max(0, min(line_height, line_bottom - glyph_height))
                glyph_mask = list(glyph.tobytes())

        glyphs.append(
            {
                "codepoint": codepoint,
                "advance": advance,
                "bearing_x": final_bearing_x,
                "top_offset": top_offset,
                "width": glyph_width,
                "height": glyph_height,
                "mask": glyph_mask,
            }
        )

    return glyphs, line_height


def build_atlas(
    glyphs: list[dict[str, int | list[int]]],
    cell_width: int,
    cell_height: int,
    columns: int,
) -> tuple[list[int], int, int]:
    rows = math.ceil(len(glyphs) / columns)
    atlas_width = cell_width * columns
    atlas_height = cell_height * rows
    atlas = [0] * (atlas_width * atlas_height)

    for index, glyph in enumerate(glyphs):
        cell_x = (index % columns) * cell_width
        cell_y = (index // columns) * cell_height
        glyph["atlas_x"] = cell_x
        glyph["atlas_y"] = cell_y

        width = int(glyph["width"])
        height = int(glyph["height"])
        if width == 0 or height == 0:
            continue

        dst_x = cell_x + max(0, min(cell_width - width, int(glyph["bearing_x"])))
        dst_y = cell_y + max(0, min(cell_height - height, int(glyph["top_offset"])))
        mask = glyph["mask"]
        for row in range(height):
            src_offset = row * width
            dst_offset = (dst_y + row) * atlas_width + dst_x
            atlas[dst_offset : dst_offset + width] = mask[src_offset : src_offset + width]

    return atlas, atlas_width, atlas_height


def emit_rust(
    output_path: Path,
    source_ttf: Path,
    font_name: str,
    cell_width: int,
    cell_height: int,
    columns: int,
    supersample: int,
    font_size: int,
    line_height: int,
    glyphs: list[dict[str, int | list[int]]],
    atlas: list[int],
    atlas_width: int,
    atlas_height: int,
) -> None:
    fallback_index = next(index for index, glyph in enumerate(glyphs) if glyph["codepoint"] == ord("?"))
    advances = sorted(int(glyph["advance"]) for glyph in glyphs)
    cell_advance = advances[(len(advances) * 9) // 10]

    lines: list[str] = []
    lines.append("// Generated by tools/font_atlas.py. Do not edit manually.")
    lines.append(f'pub const FONT_NAME: &str = "{rust_string(font_name)}";')
    lines.append(f'pub const SOURCE_TTF: &str = "{rust_string(source_ttf.as_posix())}";')
    lines.append(f"pub const SOURCE_FONT_SIZE: u32 = {font_size};")
    lines.append(f"pub const CELL_WIDTH: u32 = {cell_width};")
    lines.append(f"pub const CELL_HEIGHT: u32 = {cell_height};")
    lines.append(f"pub const CELL_ADVANCE: u32 = {cell_advance};")
    lines.append(f"pub const LINE_HEIGHT: u32 = {line_height};")
    lines.append(f"pub const COLUMNS: u32 = {columns};")
    lines.append(f"pub const SUPERSAMPLE: u32 = {supersample};")
    lines.append(f"pub const ATLAS_WIDTH: u32 = {atlas_width};")
    lines.append(f"pub const ATLAS_HEIGHT: u32 = {atlas_height};")
    lines.append(f"pub const FALLBACK_INDEX: usize = {fallback_index};")
    lines.append("")
    lines.append("#[derive(Clone, Copy, Debug)]")
    lines.append("#[repr(C)]")
    lines.append("pub struct Glyph {")
    lines.append("    pub codepoint: u32,")
    lines.append("    pub advance: u16,")
    lines.append("    pub bearing_x: i16,")
    lines.append("    pub top_offset: i16,")
    lines.append("    pub width: u16,")
    lines.append("    pub height: u16,")
    lines.append("    pub atlas_x: u16,")
    lines.append("    pub atlas_y: u16,")
    lines.append("}")
    lines.append("")
    lines.append(f"pub const GLYPHS: [Glyph; {len(glyphs)}] = [")
    for glyph in glyphs:
        lines.append(
            "    Glyph { "
            f"codepoint: 0x{int(glyph['codepoint']):04X}, "
            f"advance: {int(glyph['advance'])}, "
            f"bearing_x: {int(glyph['bearing_x'])}, "
            f"top_offset: {int(glyph['top_offset'])}, "
            f"width: {int(glyph['width'])}, "
            f"height: {int(glyph['height'])}, "
            f"atlas_x: {int(glyph['atlas_x'])}, "
            f"atlas_y: {int(glyph['atlas_y'])} "
            "},"
        )
    lines.append("];")
    lines.append("")
    lines.append(f"pub const ATLAS_ALPHA: [u8; {len(atlas)}] = [")
    for offset in range(0, len(atlas), 24):
        chunk = atlas[offset : offset + 24]
        lines.append("    " + ", ".join(str(byte) for byte in chunk) + ",")
    lines.append("];")
    lines.append("")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines), encoding="utf-8")


def selected_codepoints(args: argparse.Namespace) -> list[int]:
    if args.chars is None:
        if args.first > args.last:
            raise ValueError("--first must be <= --last")
        codepoints = list(range(args.first, args.last + 1))
    else:
        codepoints = []
        seen: set[int] = set()
        for ch in args.chars:
            codepoint = ord(ch)
            if codepoint in seen:
                continue
            seen.add(codepoint)
            codepoints.append(codepoint)
    if ord("?") not in codepoints:
        codepoints.append(ord("?"))
    codepoints.sort()
    return codepoints


def main() -> int:
    args = parse_args()
    if not args.ttf.is_file():
        raise FileNotFoundError(f"TTF not found: {args.ttf}")
    if args.supersample <= 0:
        raise ValueError("--supersample must be >= 1")
    if args.columns <= 0:
        raise ValueError("--columns must be >= 1")

    codepoints = selected_codepoints(args)

    font, font_size, union_bbox = load_font_that_fits(
        args.ttf,
        args.cell_width,
        args.cell_height,
        args.supersample,
        codepoints,
    )
    glyphs, line_height = rasterize_glyphs(
        font,
        args.cell_width,
        args.cell_height,
        args.supersample,
        union_bbox,
        codepoints,
    )
    atlas, atlas_width, atlas_height = build_atlas(glyphs, args.cell_width, args.cell_height, args.columns)
    font_name = args.font_name if args.font_name is not None else args.ttf.stem
    emit_rust(
        args.output,
        args.ttf,
        font_name,
        args.cell_width,
        args.cell_height,
        args.columns,
        args.supersample,
        font_size,
        line_height,
        glyphs,
        atlas,
        atlas_width,
        atlas_height,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
