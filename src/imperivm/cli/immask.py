"""`immask` — inspect and export the GRID masks (`.pass` and friends).

Specification: docs/formats/pass.md
Usage guide:   docs/tools/immask.md

The magic is `DIRG`, the FourCC `GRID` stored little-endian, and it is a
general-purpose grid container addressed in world units, not a passability
format. A `.pass` file is one configuration of it — `cell_size 16`,
`bits_per_cell 1`, `extent 2048 x 2048`, so 128 x 128 bits in 2,068 bytes — and
`DATA\\RANDOM_MAP.TERRAIN.GRID` is the same container at `64, 8, 16384, 16384`.
This tool handles both.

Two facts drive the design:

* **Discovery is by content, not by name.** 546 masks are named `*.PASS`, but
  51 more sit in `MapObjects.pak` under a file called plainly `PASS` with no
  extension. Matching on the extension silently loses those, so every command
  here sniffs the `DIRG` magic instead.
* **One-bit rows are packed least-significant-bit first.** Getting that
  backwards does not produce noise, it produces plausible-looking shapes with
  diagonal tearing — which is why the export is worth looking at.

A set bit means the cell is blocked, so `export` paints set cells white.
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass
from pathlib import Path

from imperivm import png
from imperivm.formats.pak import PackFile, PackFileError
from imperivm.formats.pass_mask import MAGIC, Grid, parse
from imperivm.manifest import Manifest, MaskEntry, export_name

# The packs that hold grids in the retail install. `--all-packs` widens this to
# every pack that parses, at the cost of reading a few hundred megabytes.
DEFAULT_PACKS = ("Buildings.pak", "MapObjects.pak", "Units.pak", "data.pak")


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


# --------------------------------------------------------------------- discovery


@dataclass(frozen=True)
class Located:
    """One grid found by content, and where it came from."""

    pack: str
    member: str
    size: int


_OPEN_PACKS: dict[Path, PackFile] = {}


def open_pack(path: Path) -> PackFile:
    """Open a pack once; `PackFile` holds the whole archive in memory."""
    if path not in _OPEN_PACKS:
        _OPEN_PACKS[path] = PackFile(path)
    return _OPEN_PACKS[path]


def discover(packs: Path, names: list[str] | None, all_packs: bool) -> list[Located]:
    """Find every member whose first four bytes are `DIRG`."""
    if names:
        candidates = [packs / n for n in names]
    elif all_packs:
        candidates = sorted(packs.glob("*.pak"))
    else:
        candidates = [packs / n for n in DEFAULT_PACKS]

    found: list[Located] = []
    for path in candidates:
        if not path.exists():
            if names:
                raise SystemExit(f"{path}: no such pack")
            continue
        try:
            pack = open_pack(path)
        except PackFileError:
            # RandomMap.pak is LZIS-compressed, not a pack. Skip quietly unless
            # the user asked for it by name.
            if names:
                raise SystemExit(f"{path}: not a HMMSYS pack")
            continue
        for entry in pack.entries:
            if entry.size >= 20 and pack.read(entry.name)[:4] == MAGIC:
                found.append(Located(path.name, entry.name, entry.size))
    return found


def member_key(name: str) -> str:
    return name.replace("/", "\\").upper()


def resolve(found: list[Located], name: str) -> Located:
    """Match a user-supplied name against the discovered grids."""
    wanted = member_key(name)
    exact = [f for f in found if member_key(f.member) == wanted]
    if len(exact) == 1:
        return exact[0]

    variants = {wanted, wanted + ".PASS", wanted + ".GRID"}
    hits = [f for f in found if member_key(f.member) in variants]
    if not hits:
        hits = [
            f for f in found
            if any(member_key(f.member).endswith("\\" + v) for v in variants)
        ]
    if not hits:
        stem = wanted.split("\\")[-1].split(".")[0]
        hits = [
            f for f in found
            if member_key(f.member).split("\\")[-1].split(".")[0] == stem
        ]

    if len(hits) == 1:
        return hits[0]
    if hits:
        raise SystemExit(
            f"{name!r} is ambiguous, it matches:\n  "
            + "\n  ".join(sorted(f"{f.pack}  {f.member}" for f in hits))
        )

    near = sorted(f.member for f in found if wanted.split("\\")[-1] in member_key(f.member))
    if near:
        shown = "\n  ".join(near[:10])
        more = f"\n  ... and {len(near) - 10} more" if len(near) > 10 else ""
        raise SystemExit(f"{name!r} is not a grid; did you mean:\n  {shown}{more}")
    raise SystemExit(f"{name!r} is not a grid in any scanned pack")


def load(packs: Path, located: Located) -> Grid:
    return parse(open_pack(packs / located.pack).read(located.member), located.member)


# ---------------------------------------------------------------------- imaging


def to_png_image(grid: Grid, normalise: bool = False, scale: int = 1) -> png.Image:
    """A GRAY PNG of the grid.

    One-bit grids map set (blocked) to white and clear to black. Byte grids
    carry cell *values* — terrain indices in the one shipped example — so the
    default writes them unchanged and `--normalise` stretches them to 0..255
    only for looking at. `scale` magnifies by nearest neighbour, which is what
    makes a 128 x 128 footprint readable on screen.
    """
    if grid.bits_per_cell == 1:
        pixels = bytes(255 if value else 0 for row in grid.cells for value in row)
    elif normalise:
        peak = max((v for row in grid.cells for v in row), default=1) or 1
        pixels = bytes(v * 255 // peak for row in grid.cells for v in row)
    else:
        pixels = bytes(v & 0xFF for row in grid.cells for v in row)

    width, height = grid.width, grid.height
    if scale > 1:
        big = bytearray(width * scale * height * scale)
        for y in range(height):
            wide = bytes(b for v in pixels[y * width : (y + 1) * width] for b in (v,) * scale)
            for repeat in range(scale):
                start = ((y * scale) + repeat) * width * scale
                big[start : start + width * scale] = wide
        width, height, pixels = width * scale, height * scale, bytes(big)
    return png.Image(width, height, png.ColorType.GRAY, pixels)


def ascii_art(grid: Grid) -> list[str]:
    """The set cells, cropped to their bounding box."""
    box = grid.bounds()
    if box is None:
        return ["(every cell is clear)"]
    x0, y0, x1, y1 = box
    if grid.bits_per_cell == 1:
        return [
            "".join("#" if grid[x, y] else "." for x in range(x0, x1 + 1))
            for y in range(y0, y1 + 1)
        ]
    ramp = "0123456789abcdefghijklmnopqrstuvwxyz"
    return [
        "".join(ramp[grid[x, y]] if grid[x, y] < len(ramp) else "?" for x in range(x0, x1 + 1))
        for y in range(y0, y1 + 1)
    ]


def relative_path(member: str) -> str:
    """Export path for a grid, e.g. `buildings/bhouse01/bhouse01.png`."""
    name = member
    if name.upper().endswith(".GRID"):
        name = name[: -len(".GRID")]
    return export_name(name)


# --------------------------------------------------------------------- manifest


def merge_manifest(directory: Path, entries: list[MaskEntry]) -> Path:
    """Replace only the `masks` section of an existing manifest."""
    try:
        manifest = Manifest.read(directory)
    except FileNotFoundError:
        manifest = Manifest()
    manifest.masks = entries
    return manifest.write(directory)


# --------------------------------------------------------------------- commands


def cmd_list(args: argparse.Namespace) -> int:
    packs = packs_dir(args.game)
    found = discover(packs, args.pack, args.all_packs)
    named = sum(1 for f in found if f.member.upper().endswith(".PASS"))
    bare = sum(1 for f in found if f.member.upper().rsplit("\\", 1)[-1] == "PASS")

    print(f"{'pack':<16} {'member':<46} {'grid':>9} {'bits':>4} {'set':>7}  box")
    for located in found:
        grid = load(packs, located)
        box = grid.bounds()
        shape = "-" if box is None else f"{box[2] - box[0] + 1}x{box[3] - box[1] + 1}"
        size = f"{grid.width}x{grid.height}"
        print(f"{located.pack:<16} {located.member:<46} {size:>9} "
              f"{grid.bits_per_cell:>4} {grid.count():>7}  {shape}")
    print(f"\n{len(found)} grids: {named} named *.PASS, {bare} named plainly PASS, "
          f"{len(found) - named - bare} other")
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    packs = packs_dir(args.game)
    located = resolve(discover(packs, args.pack, args.all_packs), args.name)
    grid = load(packs, located)
    box = grid.bounds()

    print(f"{located.member}  ({located.pack}, {located.size} bytes)")
    print(f"  grid         {grid.width} x {grid.height} cells of "
          f"{grid.cell_size} world units")
    print(f"  storage      {grid.bits_per_cell} bit(s) per cell, stride {grid.stride} bytes")
    print(f"  extent       {grid.extent_x} x {grid.extent_y} world units")
    print(f"  non-zero     {grid.count()} of {grid.width * grid.height} cells")
    if box is None:
        print("  bounds       none, every cell is clear")
    else:
        x0, y0, x1, y1 = box
        w, h = x1 - x0 + 1, y1 - y0 + 1
        origin = grid.world_origin_of(x0, y0)
        print(f"  bounds       x {x0}..{x1}, y {y0}..{y1}  ({w} x {h} cells, "
              f"{w * grid.cell_size} x {h * grid.cell_size} world units)")
        print(f"  anchor       top-left of the box is {origin} from the entity origin")
    if args.ascii:
        print()
        for line in ascii_art(grid):
            print("  " + line)
    return 0


def cmd_export(args: argparse.Namespace) -> int:
    packs = packs_dir(args.game)
    located = resolve(discover(packs, args.pack, args.all_packs), args.name)
    grid = load(packs, located)

    stem = Path(located.member.replace("\\", "/")).stem.lower() or "grid"
    output = Path(args.output) if args.output else Path(stem + ".png")
    if output.is_dir():
        output = output / (stem + ".png")
    output.parent.mkdir(parents=True, exist_ok=True)
    png.write(output, to_png_image(grid, args.normalise, args.scale))
    print(f"{located.member}: {grid.width}x{grid.height} -> {output} "
          f"({grid.count()} cells set)")
    return 0


def cmd_export_all(args: argparse.Namespace) -> int:
    packs = packs_dir(args.game)
    found = discover(packs, args.pack, args.all_packs)
    directory = Path(args.output)

    entries: list[MaskEntry] = []
    empty = 0
    for located in found:
        grid = load(packs, located)
        relative = relative_path(located.member)
        target = directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        png.write(target, to_png_image(grid, args.normalise))
        entries.append(
            MaskEntry(
                path=relative,
                source=located.member,
                cell_size=grid.cell_size,
                bits_per_cell=grid.bits_per_cell,
                cells_x=grid.width,
                cells_y=grid.height,
            )
        )
        if grid.count() == 0:
            empty += 1
        if args.verbose:
            print(f"  {located.pack:<16} {located.member} -> {relative}")

    paths = {e.path for e in entries}
    if len(paths) != len(entries):
        print(f"immask: warning, {len(entries) - len(paths)} export paths collided",
              file=sys.stderr)
    manifest = merge_manifest(directory, entries)
    one_bit = sum(1 for e in entries if e.bits_per_cell == 1)
    print(f"{len(entries)} grids exported to {directory}")
    print(f"  {one_bit} one-bit passability masks ({empty} entirely clear)")
    print(f"  {len(entries) - one_bit} other grid(s)")
    print(f"{len(entries)} mask entries in {manifest}")
    return 0


# ------------------------------------------------------------------------- main


def add_scan_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--pack",
        action="append",
        help="scan only this pack (repeatable); default: "
        + ", ".join(DEFAULT_PACKS),
    )
    parser.add_argument(
        "--all-packs",
        action="store_true",
        help="scan every pack in Packs/ instead of the default four",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="immask",
        description="Inspect and export the GRID masks of Imperivm.",
    )
    parser.add_argument(
        "--game",
        type=Path,
        default=default_game_root(),
        help="the game installation directory (default: $IMPERIVM_ROOT or .)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    listing = subparsers.add_parser("list", help="find every grid by its DIRG magic")
    add_scan_options(listing)
    listing.set_defaults(func=cmd_list)

    info = subparsers.add_parser("info", help="describe one grid")
    info.add_argument("name", help="e.g. bhouse01, BUILDINGS\\BWALLS\\GATEE\\GATEE.PASS")
    info.add_argument("--ascii", action="store_true", help="print the footprint")
    add_scan_options(info)
    info.set_defaults(func=cmd_info)

    export = subparsers.add_parser(
        "export", help="export one grid as a GRAY PNG, set bit = white"
    )
    export.add_argument("name")
    export.add_argument("-o", "--output", help="PNG path (default: <name>.png)")
    export.add_argument(
        "--normalise",
        action="store_true",
        help="stretch byte-per-cell values to 0..255 for viewing (never for 1-bit)",
    )
    export.add_argument(
        "--scale", type=int, default=1, help="integer magnification, for looking at"
    )
    add_scan_options(export)
    export.set_defaults(func=cmd_export)

    export_all = subparsers.add_parser(
        "export-all", help="export every grid and update manifest.json"
    )
    export_all.add_argument("-o", "--output", required=True, help="export directory")
    export_all.add_argument(
        "--normalise",
        action="store_true",
        help="stretch byte-per-cell values to 0..255 (never for 1-bit)",
    )
    export_all.add_argument("-v", "--verbose", action="store_true")
    add_scan_options(export_all)
    export_all.set_defaults(func=cmd_export_all)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (OSError, KeyError, ValueError) as error:
        print(f"immask: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
