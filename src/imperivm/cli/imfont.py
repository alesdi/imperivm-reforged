"""`imfont` — inspect, export and render the bitmap fonts (`.apf`).

Specification: docs/formats/apf.md
Usage guide:   docs/tools/imfont.md

`Packs/Fonts.pak` holds six fonts, each with an `.ini` companion that records
the source typeface, the point size and the code point ranges that were baked.
Every one carries the same 1,633 glyphs across ten ranges — Latin, Greek and
Cyrillic.

A glyph is greyscale coverage, not colour: three bits of alpha that the engine
multiplies against whatever ink colour the caller asks for. So everything this
tool writes is a `GRAY` PNG where the value *is* the coverage, and white on
black is the honest rendering. `--invert` exists for reading on paper.

Two things make correct layout non-obvious, and both are handled here:

* Advance is the Win32 ABC sum `A + B + C`, and `A` is negative for 36 to 116
  glyphs per font. A layout that pins the first glyph's ink to x=0 clips it, so
  this tool measures the true ink bounds of the whole string first.
* `TAHOMA13.APF` ships 184 kerning pairs; the other five ship none. Kerning is
  applied before the advance, per pair.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

from imperivm import png
from imperivm.formats.apf import ALPHA_LEVELS, ApfFont, Glyph
from imperivm.formats.pak import PackFile
from imperivm.manifest import FontEntry, Manifest, export_name

FONT_PACK = "Fonts.pak"


# ---------------------------------------------------------------- installation


def default_game_root() -> Path:
    return Path(os.environ.get("IMPERIVM_ROOT", "."))


def packs_dir(game: Path) -> Path:
    game = Path(game)
    if (game / "Packs").is_dir():
        return game / "Packs"
    if game.is_dir() and game.name.lower() == "packs":
        return game
    raise SystemExit(f"{game}: no Packs/ directory here, pass --game <install dir>")


def font_members(pack: PackFile) -> list[str]:
    return [e.name for e in pack.entries if e.name.upper().endswith(".APF")]


def resolve(pack: PackFile, name: str) -> str:
    """Turn a user-supplied name into a pack member name (`tahoma13` works)."""
    wanted = name.replace("/", "\\").upper()
    candidates = [wanted]
    if not wanted.endswith(".APF"):
        candidates.append(wanted + ".APF")
    candidates += ["FONTS\\" + c for c in list(candidates)]
    keys = {m.upper(): m for m in font_members(pack)}
    for candidate in candidates:
        if candidate in keys:
            return keys[candidate]
    raise SystemExit(
        f"{name!r} is not a font in {FONT_PACK}; try one of:\n  "
        + "\n  ".join(sorted(keys.values()))
    )


def load(pack: PackFile, member: str) -> ApfFont:
    font = ApfFont(pack.read(member), member)
    font.validate()
    return font


def companion_ini(pack: PackFile, member: str) -> str | None:
    """The `.ini` stored next to a font, if it is there."""
    ini = member[: -len(".APF")] + ".INI"
    if ini in pack:
        return pack.read(ini).decode("cp1252")
    return None


# ----------------------------------------------------------------------- layout


def _scaled(value: int) -> int:
    """3-bit coverage to 8-bit."""
    return value * 255 // (ALPHA_LEVELS - 1)


def _placements(font: ApfFont, text: str) -> list[tuple[Glyph, int]]:
    """Pen positions for one line, in font units, starting from pen = 0."""
    out: list[tuple[Glyph, int]] = []
    pen = 0
    previous: int | None = None
    for character in text:
        code = ord(character)
        if code not in font:
            raise SystemExit(f"U+{code:04X} ({character!r}) is not in {font.name}")
        if previous is not None:
            pen += font.kern(previous, code)
        out.append((font.glyphs[code], pen))
        pen += font.glyphs[code].advance
        previous = code
    return out


def layout(font: ApfFont, text: str, line_gap: int = 0) -> tuple[int, int, bytearray]:
    """Rasterise `text` (`\\n` splits lines) into an 8-bit coverage buffer.

    Unlike a naive layout this measures first, so a leading glyph with a
    negative `A` bearing is shifted into view instead of being clipped. The
    baseline of line `n` sits at `n * (height + line_gap) + ascent`.
    """
    lines = text.split("\n")
    placed = [_placements(font, line) for line in lines]

    left = 0
    right = 0
    for glyphs in placed:
        for glyph, pen in glyphs:
            left = min(left, pen + glyph.abc_a)
            right = max(right, pen + glyph.abc_a + glyph.width)
        if glyphs:
            last_glyph, last_pen = glyphs[-1]
            right = max(right, last_pen + last_glyph.advance)

    line_height = font.metrics.height + line_gap
    width = max(1, right - left)
    height = max(1, line_height * len(lines) - line_gap)
    buffer = bytearray(width * height)

    for row, glyphs in enumerate(placed):
        top = row * line_height
        for glyph, pen in glyphs:
            if glyph.is_blank:
                continue
            alpha = font.coverage(glyph.code)
            for y in range(glyph.height):
                target = top + glyph.top + y
                if not 0 <= target < height:
                    continue
                base = target * width + (pen + glyph.abc_a - left)
                source = y * glyph.width
                for x in range(glyph.width):
                    value = alpha[source + x]
                    if value:
                        index = base + x
                        buffer[index] = max(buffer[index], _scaled(value))
    return width, height, buffer


def finish(
    width: int, height: int, buffer: bytearray, *, invert: bool, scale: int
) -> png.Image:
    """Apply `--invert` and integer nearest-neighbour `--scale`."""
    pixels = bytes(255 - v for v in buffer) if invert else bytes(buffer)
    if scale > 1:
        big = bytearray(width * scale * height * scale)
        for y in range(height):
            row = pixels[y * width : (y + 1) * width]
            wide = bytes(b for v in row for b in (v,) * scale)
            for repeat in range(scale):
                start = ((y * scale) + repeat) * width * scale
                big[start : start + width * scale] = wide
        width, height, pixels = width * scale, height * scale, bytes(big)
    return png.Image(width, height, png.ColorType.GRAY, pixels)


# ------------------------------------------------------------------ glyph sheet


def sheet(
    font: ApfFont, codes: list[int], columns: int, padding: int
) -> tuple[png.Image, dict]:
    """Draw a fixed-cell glyph sheet and return it with its metrics record."""
    glyphs = [font.glyphs[code] for code in codes]
    columns = max(1, min(columns, len(codes)))
    cell_w = max((max(0, g.abc_a) + g.width for g in glyphs), default=1) + padding
    cell_h = max(
        [font.metrics.height] + [g.top + g.height for g in glyphs if not g.is_blank]
    ) + padding
    rows = (len(codes) + columns - 1) // columns
    width = max(1, cell_w * columns)
    height = max(1, cell_h * rows)
    buffer = bytearray(width * height)

    records = []
    for i, glyph in enumerate(glyphs):
        column, row = i % columns, i // columns
        origin_x = column * cell_w + max(0, glyph.abc_a)
        origin_y = row * cell_h + glyph.top
        if not glyph.is_blank:
            alpha = font.coverage(glyph.code)
            for y in range(glyph.height):
                base = (origin_y + y) * width + origin_x
                source = y * glyph.width
                for x in range(glyph.width):
                    value = alpha[source + x]
                    if value:
                        buffer[base + x] = _scaled(value)
        records.append(
            {
                "code": glyph.code,
                "char": chr(glyph.code) if glyph.code >= 32 else "",
                "row": row,
                "column": column,
                "x": origin_x,
                "y": origin_y,
                "width": glyph.width,
                "height": glyph.height,
                "abc_a": glyph.abc_a,
                "abc_b": glyph.abc_b,
                "abc_c": glyph.abc_c,
                "top": glyph.top,
                "bottom": glyph.bottom,
                "advance": glyph.advance,
                "blank": glyph.is_blank,
            }
        )

    metrics = {
        "font": font.name,
        "face": font.face_name,
        "family": font.family_name,
        "point_size": font.point_size,
        "bold": bool(font.bold),
        "italic": bool(font.italic),
        "cell_height": font.metrics.height,
        "ascent": font.metrics.ascent,
        "descent": font.metrics.descent,
        "max_char_width": font.metrics.max_char_width,
        "ave_char_width": font.metrics.ave_char_width,
        "internal_leading": font.metrics.internal_leading,
        "external_leading": font.metrics.external_leading,
        "sheet": {
            "columns": columns,
            "rows": rows,
            "cell_width": cell_w,
            "cell_height": cell_h,
            "padding": padding,
            "width": width,
            "height": height,
        },
        "ranges": [
            {"first": r.first, "last": r.last, "count": r.count} for r in font.ranges
        ],
        "glyphs": records,
        "kerning": [
            {"left": left, "right": right, "amount": amount}
            for (left, right), amount in sorted(font.kerning.items())
        ],
    }
    return png.Image(width, height, png.ColorType.GRAY, bytes(buffer)), metrics


def parse_codes(font: ApfFont, spec: str | None) -> list[int]:
    """`--codes 32-126,0x400-0x40F` selects a subset; the default is everything."""
    if not spec:
        return sorted(font.glyphs)
    wanted: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part[1:]:
            head, _, tail = part[1:].partition("-")
            first, last = int(part[0] + head, 0), int(tail, 0)
        else:
            first = last = int(part, 0)
        wanted += [c for c in range(first, last + 1) if c in font]
    if not wanted:
        raise SystemExit(f"{spec!r} selects no glyph present in {font.name}")
    return wanted


# --------------------------------------------------------------------- manifest


def entry_for(font: ApfFont, member: str, path: str) -> FontEntry:
    return FontEntry(
        path=path,
        source=member,
        face=font.face_name,
        point_size=font.point_size,
        height=font.metrics.height,
        ascent=font.metrics.ascent,
        descent=font.metrics.descent,
        glyphs=len(font),
    )


def merge_manifest(directory: Path, entries: list[FontEntry]) -> Path:
    """Replace only the `fonts` section of an existing manifest."""
    try:
        manifest = Manifest.read(directory)
    except FileNotFoundError:
        manifest = Manifest()
    manifest.fonts = entries
    return manifest.write(directory)


# --------------------------------------------------------------------- commands


def cmd_list(args: argparse.Namespace) -> int:
    pack = PackFile(packs_dir(args.game) / FONT_PACK)
    print(f"{'member':<26} {'face':<14} {'pt':>3} {'cell':>5} {'asc':>4} {'desc':>5} "
          f"{'glyphs':>7} {'kern':>5}")
    members = font_members(pack)
    for member in members:
        font = load(pack, member)
        m = font.metrics
        print(f"{member:<26} {font.face_name:<14} {font.point_size:>3} {m.height:>5} "
              f"{m.ascent:>4} {m.descent:>5} {len(font):>7} {len(font.kerning):>5}")
    print(f"\n{len(members)} fonts in {FONT_PACK}")
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    pack = PackFile(packs_dir(args.game) / FONT_PACK)
    member = resolve(pack, args.name)
    font = load(pack, member)
    m = font.metrics
    blank = sum(1 for g in font.glyphs.values() if g.is_blank)

    print(f"{member}: face {font.face_name!r} family {font.family_name!r}")
    print(f"  request      {font.point_size}pt bold={font.bold} italic={font.italic}")
    print(f"  cell         {m.height}px  ascent {m.ascent}  descent {m.descent}")
    print(f"  widths       max {m.max_char_width}  average {m.ave_char_width}")
    print(f"  leading      internal {m.internal_leading}  external {m.external_leading}")
    print(f"  glyphs       {len(font)} in {len(font.ranges)} ranges "
          f"({blank} blank), {len(font.kerning)} kerning pairs")
    for char_range in font.ranges:
        print(f"    U+{char_range.first:04X}..U+{char_range.last:04X}"
              f"  {char_range.count:>5} glyphs  {char_range.size:>8} bytes")
    ini = companion_ini(pack, member)
    if ini:
        print(f"  {member[: -len('.APF')] + '.INI'}:")
        for line in ini.splitlines():
            if line.strip():
                print(f"    {line.rstrip()}")
    return 0


def cmd_export(args: argparse.Namespace) -> int:
    pack = PackFile(packs_dir(args.game) / FONT_PACK)
    member = resolve(pack, args.name)
    font = load(pack, member)
    codes = parse_codes(font, args.codes)
    image, metrics = sheet(font, codes, args.columns, args.padding)

    stem = Path(member.replace("\\", "/")).stem.lower()
    output = Path(args.output) if args.output else Path(stem + ".png")
    if output.is_dir():
        output = output / (stem + ".png")
    output.parent.mkdir(parents=True, exist_ok=True)
    if args.invert or args.scale > 1:
        image = finish(
            image.width, image.height, bytearray(image.pixels),
            invert=args.invert, scale=args.scale,
        )
    png.write(output, image)

    sidecar = Path(args.json) if args.json else output.with_suffix(".glyphs.json")
    sidecar.write_text(json.dumps(metrics, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"{member}: {len(codes)} glyphs, {image.width}x{image.height} -> {output}")
    print(f"  metrics -> {sidecar}")
    return 0


def cmd_render(args: argparse.Namespace) -> int:
    pack = PackFile(packs_dir(args.game) / FONT_PACK)
    member = resolve(pack, args.name)
    font = load(pack, member)
    text = args.text.replace("\\n", "\n")
    width, height, buffer = layout(font, text, args.line_gap)

    if not args.output:
        ramp = " .:-=+*#%@"
        for y in range(height):
            row = buffer[y * width : (y + 1) * width]
            print("".join(ramp[v * (len(ramp) - 1) // 255] for v in row))
        return 0

    image = finish(width, height, buffer, invert=args.invert, scale=args.scale)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    png.write(output, image)
    print(f"{member}: {text!r} -> {output} ({image.width}x{image.height}, "
          f"advance width {width}px)")
    return 0


def cmd_export_all(args: argparse.Namespace) -> int:
    pack = PackFile(packs_dir(args.game) / FONT_PACK)
    directory = Path(args.output)
    entries: list[FontEntry] = []
    for member in font_members(pack):
        font = load(pack, member)
        codes = sorted(font.glyphs)
        image, metrics = sheet(font, codes, args.columns, args.padding)
        relative = export_name(member)
        target = directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        png.write(target, image)
        target.with_suffix(".glyphs.json").write_text(
            json.dumps(metrics, indent=2, ensure_ascii=False), encoding="utf-8"
        )
        entries.append(entry_for(font, member, relative))
        if args.verbose:
            print(f"  {member} -> {relative} ({len(codes)} glyphs, "
                  f"{image.width}x{image.height})")

    manifest = merge_manifest(directory, entries)
    print(f"{len(entries)} fonts exported to {directory}")
    print(f"{len(entries)} font entries in {manifest}")
    return 0


# ------------------------------------------------------------------------- main


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="imfont",
        description="Inspect, export and render the bitmap fonts of Imperivm.",
    )
    parser.add_argument(
        "--game",
        type=Path,
        default=default_game_root(),
        help="the game installation directory (default: $IMPERIVM_ROOT or .)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    listing = subparsers.add_parser("list", help="list the fonts in Fonts.pak")
    listing.set_defaults(func=cmd_list)

    info = subparsers.add_parser("info", help="face, size, ranges and glyph count")
    info.add_argument("name", help="e.g. tahoma13, FONTS\\TAHOMA20B.APF")
    info.set_defaults(func=cmd_info)

    export = subparsers.add_parser(
        "export", help="write a glyph sheet as a GRAY PNG plus a metrics JSON"
    )
    export.add_argument("name")
    export.add_argument("-o", "--output", help="PNG path (default: <font>.png)")
    export.add_argument("--json", help="metrics path (default: alongside the PNG)")
    export.add_argument("--columns", type=int, default=32, help="glyphs per row")
    export.add_argument("--padding", type=int, default=1, help="pixels between cells")
    export.add_argument("--codes", help="subset, e.g. '32-126,0x410-0x44F'")
    export.add_argument("--invert", action="store_true", help="black ink on white")
    export.add_argument("--scale", type=int, default=1, help="integer magnification")
    export.set_defaults(func=cmd_export)

    render = subparsers.add_parser("render", help="rasterise a string")
    render.add_argument("name")
    render.add_argument("--text", required=True, help="the string; \\n starts a new line")
    render.add_argument("-o", "--output", help="PNG path (default: ASCII art on stdout)")
    render.add_argument("--line-gap", type=int, default=0, help="extra pixels per line")
    render.add_argument("--invert", action="store_true", help="black ink on white")
    render.add_argument("--scale", type=int, default=1, help="integer magnification")
    render.set_defaults(func=cmd_render)

    export_all = subparsers.add_parser(
        "export-all", help="export every font and update manifest.json"
    )
    export_all.add_argument("-o", "--output", required=True, help="export directory")
    export_all.add_argument("--columns", type=int, default=32)
    export_all.add_argument("--padding", type=int, default=1)
    export_all.add_argument("-v", "--verbose", action="store_true")
    export_all.set_defaults(func=cmd_export_all)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (OSError, KeyError, ValueError) as error:
        print(f"imfont: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
