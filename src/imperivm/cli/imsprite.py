"""`imsprite` — inspect, export and re-encode the game's sprites.

Documentation: docs/tools/imsprite.md
Format: docs/formats/rle.md

Every sprite in Imperivm is a `*.rle.mmp` frame table inside a pack plus a slice
of the single 400 MB `rle.mmp` pixel store at the install root. This tool turns
that pair into PNGs a human or an editor can work with, and turns them back.

Subcommands::

    imsprite info      PACK ENTRY            what the frame table says
    imsprite export    PACK ENTRY --out DIR  one sheet PNG plus a manifest
    imsprite export-all --game DIR --out DIR every sprite in the install
    imsprite sheet     PACK ENTRY --out FILE a contact sheet to look at
    imsprite import    PNG MANIFEST --out F  a sheet back to the frame format

Run as ``PYTHONPATH=src python3 -m imperivm.cli.imsprite ...``.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from dataclasses import asdict
from pathlib import Path

from imperivm import png, sprite
from imperivm.formats.pak import PackFile, PackFileError
from imperivm.formats.rle import (
    HEADER_SIZE,
    ImageClass,
    PixelFormat,
    PixelStore,
    RleError,
    RleImage,
)
from imperivm.manifest import FrameEntry, Manifest, SpriteEntry, export_name

SPRITE_PACKS = ("Units", "Buildings", "MapObjects", "Visuals")
RLE_SUFFIX = ".RLE.MMP"


# -- resolving the two halves of a sprite ---------------------------------


def find_store(pack_path: Path, explicit: Path | None, game: Path | None) -> Path:
    """Locate the root-level `rle.mmp` that holds the pixels."""
    for candidate in (
        explicit,
        game / "rle.mmp" if game else None,
        pack_path.parent.parent / "rle.mmp",
        pack_path.parent / "rle.mmp",
    ):
        if candidate and candidate.is_file():
            return candidate
    raise SystemExit(
        "cannot find the rle.mmp pixel store; pass --mmp or --game. "
        "It is the ~400 MB file at the root of the installation."
    )


def resolve_entry(pack: PackFile, name: str) -> str:
    """Accept a pack path with either separator, any case, extension optional."""
    for candidate in (name, name + ".MMP", name + RLE_SUFFIX):
        if candidate in pack:
            return candidate
    raise SystemExit(f"{name!r} is not in {pack.path.name}")


def sprite_entries(pack: PackFile) -> list[str]:
    return [e.name for e in pack.entries if e.name.upper().endswith(RLE_SUFFIX)]


def load_image(pack: PackFile, name: str) -> RleImage:
    image = RleImage(pack.read(name))
    image.validate()
    return image


def class_name(image: RleImage) -> str:
    try:
        return ImageClass(image.image_class).name.lower()
    except ValueError:
        return f"unknown({image.image_class})"


def sprite_format(image: RleImage) -> PixelFormat | None:
    """The one pixel format every populated frame of the image shares."""
    for frame in image.frames:
        if not frame.empty:
            return frame.pixel_format
    return None


# 44 images in the retail data have no populated frame at all, so they carry no
# pixel format of their own. The image class still implies one — the mapping is
# one-to-one in the specification — and using it keeps an all-empty shadow
# recognisable as a shadow instead of defaulting to truecolour.
_CLASS_FORMAT = {
    ImageClass.TRUECOLOR: PixelFormat.RGB555,
    ImageClass.INDEXED: PixelFormat.INDEXED8,
    ImageClass.PLAYER_COLOR: PixelFormat.INDEXED8,
    ImageClass.SHADOW: PixelFormat.MASK,
    ImageClass.CLOUDS: PixelFormat.RGB555,
}


def implied_format(image: RleImage) -> PixelFormat | None:
    fmt = sprite_format(image)
    if fmt is not None:
        return fmt
    try:
        return _CLASS_FORMAT.get(ImageClass(image.image_class))
    except ValueError:
        return None


def format_label(fmt: PixelFormat | None) -> str:
    return fmt.name.lower() if fmt else "none"


def shadow_candidates(name: str) -> list[str]:
    """Names a body sheet's shadow companion might be stored under.

    The format does not link the two — the entity XML does, by giving both
    images the same layer offsets. Within one directory the naming is
    consistent enough to guess from: `X` pairs with `X_SHADOW`, and buildings
    commonly keep one `SHADOW` or `<DIR>_SHADOW` beside several body layers.
    """
    upper = name.upper()
    if not upper.endswith(RLE_SUFFIX) or "SHADOW" in upper:
        return []
    stem = name[: -len(RLE_SUFFIX)]
    directory, _, leaf = stem.rpartition("\\")
    prefix = f"{directory}\\" if directory else ""
    folder = directory.rpartition("\\")[2]
    names = [f"{stem}_SHADOW", f"{prefix}SHADOW"]
    if folder:
        names.append(f"{prefix}{folder}_SHADOW")
    if "_" in leaf:
        names.append(f"{prefix}{leaf.partition('_')[0]}_SHADOW")
    return [n + RLE_SUFFIX for n in dict.fromkeys(names)]


# -- info -----------------------------------------------------------------


def cmd_info(args: argparse.Namespace) -> int:
    pack = PackFile(args.pack)
    name = resolve_entry(pack, args.entry)
    image = load_image(pack, name)
    fmt = sprite_format(image)
    frames = sprite.populated(image)
    empty = len(image.frames) - len(frames)

    print(f"{name}")
    print(f"  image class    {image.image_class} ({class_name(image)})")
    print(f"  grid           {image.columns} columns x {image.rows} rows "
          f"= {len(image.frames)} frames ({empty} empty)")
    print(f"  pixel format   {format_label(fmt)}"
          + (f" ({fmt.bytes_per_pixel} bytes/pixel)" if fmt else ""))
    canvas_w, canvas_h = image.canvas_size
    print(f"  canvas         {canvas_w} x {canvas_h}")
    if frames:
        left, top, right, bottom = sprite.union_box(image)
        print(f"  bounding union ({left}, {top})-({right}, {bottom})")
        print(f"  largest frame  {max(f.width for f in frames)} x "
              f"{max(f.height for f in frames)}")
        print(f"  payload        {sum(f.data_size for f in frames)} bytes in rle.mmp")
        wide = sum(1 for f in frames if f.wide_row_table)
        if wide:
            print(f"  wide row table {wide} frame(s) use 32-bit row offsets")

    player = image.image_class == ImageClass.PLAYER_COLOR
    print(f"  player colour  {'yes' if player else 'no'}"
          + (f", palette slots 0..{sprite.PLAYER_COLOR_SLOTS - 1} are the team block"
             if player else ""))
    if image.palette:
        mirrored = (
            image.palette[: sprite.PLAYER_COLOR_SLOTS]
            == image.palette[sprite.PLAYER_COLOR_SLOTS : 2 * sprite.PLAYER_COLOR_SLOTS]
        )
        print(f"  palette        {len(image.palette)} entries, "
              f"15-bit lut {'present' if image.lut else 'absent'}")
        print(f"                 0..63 mirror 64..127: {'yes' if mirrored else 'no'}")
        keys = sorted({f.color_key for f in frames})
        for key in keys:
            rgb = image.palette[key] if key < len(image.palette) else None
            print(f"  colour key     index {key}" + (f" = rgb{rgb}" if rgb else ""))
    else:
        print("  palette        none (truecolour or coverage mask)")

    if args.frames:
        store_path = find_store(Path(args.pack), args.mmp, args.game)
        with PixelStore(store_path) as store:
            print(f"  {'idx':>5} {'row':>4} {'col':>4} {'size':>11} "
                  f"{'box':>19} {'covered':>9}")
            for frame in image.frames:
                row, column = divmod(frame.index, image.columns)
                if frame.empty:
                    print(f"  {frame.index:>5} {row:>4} {column:>4} "
                          f"{'empty':>11}")
                    continue
                covered = sprite.frame_coverage(frame, store.blob(frame))
                box = f"({frame.left},{frame.top})-({frame.right},{frame.bottom})"
                print(f"  {frame.index:>5} {row:>4} {column:>4} "
                      f"{frame.width}x{frame.height:<8} {box:>19} {covered:>9}")
    return 0


# -- export ---------------------------------------------------------------


def build_sheet(
    image: RleImage,
    store: PixelStore,
    *,
    rgba: bool,
    team: tuple[int, int, int] | None,
    layout: sprite.Layout | None = None,
) -> tuple[sprite.Sheet, bool]:
    """Compose one sprite. Returns the sheet and whether it stayed indexed.

    Indexed sources stay indexed unless `--rgba` was asked for, or unless the
    palette has no index free to carry transparency — which does not happen in
    the retail data, but the fallback is here so the tool cannot silently write
    a sheet with no transparency at all.
    """
    if layout is None:
        layout = sprite.tight_layout(image)
    fmt = sprite_format(image)

    if fmt is PixelFormat.MASK:
        return sprite.build_mask_sheet(image, store, layout), False
    if fmt is PixelFormat.INDEXED8 and not rgba:
        used = sprite.used_indices(image, store)
        transparent = sprite.choose_transparent_index(image, used)
        if transparent is not None:
            return (
                sprite.build_indexed_sheet(
                    image, store, layout, team=team, transparent=transparent
                ),
                True,
            )
    if fmt is None:
        # 44 images have no populated frame at all. They still need a file so
        # that the manifest entry points somewhere; a 1x1 transparent pixel is
        # the smallest honest stand-in.
        return (
            sprite.Sheet(
                png.Image(1, 1, png.ColorType.RGBA, b"\x00\x00\x00\x00"), layout
            ),
            False,
        )
    return sprite.build_rgba_sheet(image, store, layout, team=team), False


def make_entry(
    image: RleImage,
    sheet: sprite.Sheet,
    *,
    path: str,
    source: str,
    indexed: bool,
) -> SpriteEntry:
    canvas_w, canvas_h = image.canvas_size
    fmt = implied_format(image)
    entry = SpriteEntry(
        path=path,
        source=source,
        rows=image.rows,
        columns=image.columns,
        canvas_width=canvas_w,
        canvas_height=canvas_h,
        pixel_format=format_label(fmt),
        indexed=indexed,
        player_color=image.image_class == ImageClass.PLAYER_COLOR,
        player_color_slots=(
            sprite.PLAYER_COLOR_SLOTS
            if image.image_class == ImageClass.PLAYER_COLOR
            else 0
        ),
        shadow=fmt is PixelFormat.MASK,
    )
    for frame in image.frames:
        row, column = divmod(frame.index, image.columns)
        if frame.empty:
            entry.frames.append(FrameEntry(row, column, 0, 0, 0, 0, 0, 0, empty=True))
            continue
        x, y = sheet.layout.place(frame)
        entry.frames.append(
            FrameEntry(
                row, column, x, y, frame.width, frame.height, frame.left, frame.top
            )
        )
    return entry


def cmd_export(args: argparse.Namespace) -> int:
    pack = PackFile(args.pack)
    name = resolve_entry(pack, args.entry)
    image = load_image(pack, name)
    store_path = find_store(Path(args.pack), args.mmp, args.game)
    team = sprite.parse_color(args.team) if args.team else None

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    relative = export_name(name)
    target = out / relative
    target.parent.mkdir(parents=True, exist_ok=True)

    with PixelStore(store_path) as store:
        sheet, indexed = build_sheet(image, store, rgba=args.rgba, team=team)
    png.write(target, sheet.image)

    entry = make_entry(image, sheet, path=relative, source=name, indexed=indexed)
    manifest_path = out / "manifest.json"
    manifest = Manifest()
    if manifest_path.is_file() and not args.replace_manifest:
        try:
            manifest = Manifest.read(out)
        except Exception:  # a manifest we cannot read is a manifest we replace
            manifest = Manifest()
    manifest.sprites = [s for s in manifest.sprites if s.path != relative]
    manifest.sprites.append(entry)
    manifest.write(out)

    kind = "indexed" if indexed else sheet.image.color_type.name.lower()
    print(f"{target}: {sheet.image.width}x{sheet.image.height} {kind}, "
          f"{image.columns}x{image.rows} frames")
    if sheet.transparent_index is not None:
        print(f"  transparent palette index {sheet.transparent_index} (tRNS)")
    print(f"  manifest {manifest_path}")
    return 0


# -- export-all -----------------------------------------------------------


def cmd_export_all(args: argparse.Namespace) -> int:
    game = Path(args.game)
    packs_dir = game / "Packs"
    if not packs_dir.is_dir():
        packs_dir = game
    store_path = find_store(packs_dir / "x.pak", args.mmp, game)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    team = sprite.parse_color(args.team) if args.team else None
    wanted = args.packs or list(SPRITE_PACKS)

    manifest = Manifest()
    failures: list[tuple[str, str]] = []
    done = pixels = payload = 0
    reencoded = reencode_bad = 0
    total = 0
    started = time.time()

    jobs: list[tuple[Path, list[str]]] = []
    for pack_name in wanted:
        path = packs_dir / f"{pack_name}.pak"
        if not path.is_file():
            print(f"skipping {path}: not found", file=sys.stderr)
            continue
        try:
            pack = PackFile(path)
        except PackFileError as error:
            print(f"skipping {path}: {error}", file=sys.stderr)
            continue
        names = sprite_entries(pack)
        total += len(names)
        jobs.append((path, names))

    with PixelStore(store_path) as store:
        for path, names in jobs:
            pack = PackFile(path)
            for name in names:
                try:
                    image = load_image(pack, name)
                    relative = export_name(name)
                    target = out / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    sheet, indexed = build_sheet(
                        image, store, rgba=args.rgba, team=team
                    )
                    # Level 6 rather than 9: a bulk export writes about a
                    # gigabyte of intermediate PNGs and the last three levels
                    # buy a couple of percent for several times the time.
                    target.write_bytes(png.encode(sheet.image, compression=args.compress))
                    manifest.sprites.append(
                        make_entry(
                            image, sheet, path=relative, source=name, indexed=indexed
                        )
                    )
                    for frame in image.frames:
                        if frame.empty:
                            continue
                        payload += frame.data_size
                        pixels += frame.width * frame.height
                        if args.verify:
                            blob = store.blob(frame)
                            reencoded += 1
                            if sprite.reencode_frame(frame, blob) != blob:
                                reencode_bad += 1
                except Exception as error:  # keep going; report the file
                    failures.append((name, f"{type(error).__name__}: {error}"))
                done += 1
                if done % 100 == 0 or done == total:
                    elapsed = time.time() - started
                    rate = done / elapsed if elapsed else 0
                    print(
                        f"\r{done}/{total} sprites  {len(failures)} failed  "
                        f"{rate:5.1f}/s  {elapsed:6.1f}s",
                        end="",
                        file=sys.stderr,
                        flush=True,
                    )
                # The sheet, its buffers and the frame blobs all go out of scope
                # here. Nothing accumulates but the manifest, which is text.
    print(file=sys.stderr)

    manifest_path = manifest.write(out)
    ok = len(manifest.sprites)
    print(f"exported {ok}/{total} sprites to {out}")
    print(f"  {pixels:,} frame pixels from {payload:,} bytes of rle.mmp")
    print(f"  manifest {manifest_path}")
    if args.verify:
        print(f"  re-encode check: {reencoded - reencode_bad:,}/{reencoded:,} "
              f"frames byte-identical")
    if failures:
        print(f"  {len(failures)} failures:")
        for name, error in failures:
            print(f"    {name}: {error}")
    return 1 if failures else 0


# -- contact sheet --------------------------------------------------------


def cmd_sheet(args: argparse.Namespace) -> int:
    pack = PackFile(args.pack)
    name = resolve_entry(pack, args.entry)
    image = load_image(pack, name)
    store_path = find_store(Path(args.pack), args.mmp, args.game)
    team = sprite.parse_color(args.team) if args.team else None

    shadow: RleImage | None = None
    shadow_ref: str | None = None
    if not args.no_shadow:
        candidates = [args.shadow] if args.shadow else shadow_candidates(name)
        for candidate in candidates:
            for form in (candidate, candidate + ".MMP", candidate + RLE_SUFFIX):
                if form in pack:
                    shadow_ref = form
                    break
            if shadow_ref:
                break
        if args.shadow and not shadow_ref:
            raise SystemExit(f"{args.shadow!r} is not in {pack.path.name}")
        if shadow_ref:
            shadow = load_image(pack, shadow_ref)

    # Both sheets are laid out against one box so the shadow lands under the
    # body without any further arithmetic — which is exactly the property the
    # format gives us by sharing a canvas between the two images.
    left, top, right, bottom = sprite.union_box(
        *([image] + ([shadow] if shadow else []))
    )
    cell_w, cell_h = right - left + 1, bottom - top + 1
    rows = max(image.rows, shadow.rows if shadow else 0)
    columns = max(image.columns, shadow.columns if shadow else 0)
    layout = sprite.Layout(
        rows, columns, cell_w, cell_h, canvas_relative=True, origin_x=left, origin_y=top
    )

    background = sprite.parse_color(args.background) if args.background else None
    width, height = layout.width, layout.height
    canvas = bytearray(width * height * 4)
    if background:
        canvas[:] = (bytes(background) + b"\xff") * (width * height)

    with PixelStore(store_path) as store:
        if shadow:
            shadow_layout = sprite.Layout(
                shadow.rows,
                shadow.columns,
                cell_w,
                cell_h,
                canvas_relative=True,
                origin_x=left,
                origin_y=top,
            )
            shade = sprite.build_rgba_sheet(shadow, store, shadow_layout)
            _over(canvas, shade.image.pixels, alpha=args.shadow_alpha)
        body = sprite.build_rgba_sheet(image, store, layout, team=team)
        _over(canvas, body.image.pixels)

    if args.grid:
        _draw_grid(canvas, width, height, layout, sprite.parse_color(args.grid))

    target = Path(args.out)
    target.parent.mkdir(parents=True, exist_ok=True)
    png.write(target, png.Image(width, height, png.ColorType.RGBA, bytes(canvas)))
    print(f"{target}: {width}x{height}, {columns}x{rows} cells of {cell_w}x{cell_h}")
    if shadow_ref:
        print(f"  shadow {shadow_ref} composited underneath "
              f"at alpha {args.shadow_alpha}")
    elif not args.no_shadow:
        print("  no shadow sheet found next to this one")
    return 0


def _over(base: bytearray, top: bytes, alpha: int = 255) -> None:
    """Alpha-composite `top` over `base`, scaling the source alpha."""
    for i in range(3, len(top), 4):
        a = top[i]
        if not a:
            continue
        a = a * alpha // 255
        if a == 255:
            base[i - 3 : i + 1] = top[i - 3 : i + 1]
            continue
        inv = 255 - a
        for c in range(i - 3, i):
            base[c] = (top[c] * a + base[c] * inv) // 255
        base[i] = a + base[i] * inv // 255


def _draw_grid(
    canvas: bytearray,
    width: int,
    height: int,
    layout: sprite.Layout,
    color: tuple[int, int, int],
) -> None:
    line = bytes(color) + b"\xff"
    for column in range(1, layout.columns):
        x = column * layout.cell_width
        for y in range(height):
            at = (y * width + x) * 4
            canvas[at : at + 4] = line
    for row in range(1, layout.rows):
        y = row * layout.cell_height
        at = y * width * 4
        canvas[at : at + width * 4] = line * width


# -- import ---------------------------------------------------------------
#
# The encoder reproduces the retail payload bytes exactly (see the measured
# result in docs/tools/imsprite.md). What it cannot reproduce is the handful of
# fields a PNG plus manifest does not carry: the degenerate bounding boxes of
# empty frames, the colour key, and the position of the payloads inside the
# shared global store. Those are named in the docs rather than guessed at.


def _frame_bytes(
    image: png.Image, entry: FrameEntry, transparent: int | None
) -> tuple[bytearray, bytes, int]:
    """Cut one frame out of a sheet: `(coverage, pixels, bytes_per_pixel)`."""
    width, height = entry.width, entry.height
    coverage = bytearray(width * height)
    channels = image.channels

    if image.color_type is png.ColorType.INDEXED:
        pixels = bytearray(width * height)
        for y in range(height):
            src = ((entry.y + y) * image.width + entry.x)
            row = image.pixels[src : src + width]
            pixels[y * width : (y + 1) * width] = row
            for x, value in enumerate(row):
                coverage[y * width + x] = 0 if value == transparent else 1
        return coverage, bytes(pixels), 1

    if image.color_type is png.ColorType.GRAY:
        for y in range(height):
            src = (entry.y + y) * image.width + entry.x
            row = image.pixels[src : src + width]
            for x, value in enumerate(row):
                coverage[y * width + x] = 1 if value else 0
        return coverage, b"", 0

    if image.color_type is png.ColorType.RGBA:
        pixels = bytearray(width * height * 2)
        for y in range(height):
            src = ((entry.y + y) * image.width + entry.x) * 4
            for x in range(width):
                at = src + 4 * x
                r, g, b, a = image.pixels[at : at + 4]
                if not a:
                    continue
                coverage[y * width + x] = 1
                packed = (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)
                struct.pack_into("<H", pixels, (y * width + x) * 2, packed)
        return coverage, bytes(pixels), 2

    raise SystemExit(
        f"cannot re-encode a {image.color_type.name} sheet; "
        "export indexed, greyscale or RGBA"
    )


def _lookup_table(palette: list[tuple[int, int, int]], player_color: bool) -> bytes:
    """Rebuild the 512-byte 15-bit table that class 2 files carry.

    Entries 64..255 are the palette in RGB555. Entries 0..63 are the filler the
    engine overwrites with the owning player's ramp; 1,651 of the 1,668 retail
    class 2 files leave them at 0x6666, so that is what is written back.
    """
    values = []
    for index in range(256):
        if player_color and index < sprite.PLAYER_COLOR_SLOTS:
            values.append(0x6666)
            continue
        if index < len(palette):
            r, g, b = palette[index]
            values.append((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3))
        else:
            values.append(0)
    return struct.pack("<256H", *values)


def cmd_import(args: argparse.Namespace) -> int:
    image = png.read(args.png)
    manifest_path = Path(args.manifest)
    directory = manifest_path.parent if manifest_path.is_file() else manifest_path
    manifest = Manifest.read(directory)

    wanted = args.entry
    if wanted is None:
        try:
            wanted = str(Path(args.png).resolve().relative_to(directory.resolve()))
        except ValueError:
            wanted = Path(args.png).name
        wanted = wanted.replace("\\", "/")
    matches = [
        s
        for s in manifest.sprites
        if s.path == wanted or Path(s.path).name == Path(wanted).name
    ]
    if not matches:
        raise SystemExit(
            f"no sprite in {directory / 'manifest.json'} has path {wanted!r}; "
            "pass --entry with the manifest path"
        )
    entry = matches[0]

    transparent: int | None = None
    if image.color_type is png.ColorType.INDEXED:
        if image.transparency is None:
            raise SystemExit("indexed sheet has no tRNS chunk, so nothing is transparent")
        zeroes = [i for i, a in enumerate(image.transparency) if a == 0]
        if len(zeroes) != 1:
            raise SystemExit(f"expected exactly one transparent index, found {zeroes}")
        transparent = zeroes[0]

    image_class = args.image_class
    if image_class is None:
        if entry.shadow:
            image_class = int(ImageClass.SHADOW)
        elif entry.indexed and entry.player_color:
            image_class = int(ImageClass.PLAYER_COLOR)
        elif entry.indexed:
            image_class = int(ImageClass.INDEXED)
        else:
            image_class = int(ImageClass.TRUECOLOR)

    color_key = args.color_key
    if color_key is None:
        color_key = {
            int(ImageClass.SHADOW): 0x00FF00,
            int(ImageClass.TRUECOLOR): 0x03E0,
            int(ImageClass.CLOUDS): 0x0000,
        }.get(image_class, transparent if transparent is not None else 0)

    out = Path(args.out)
    store_path = Path(args.store) if args.store else out.with_suffix(out.suffix + ".store")

    records = bytearray()
    payloads: list[bytes] = []
    offset = args.base_offset
    populated = 0

    for item in entry.frames:
        if item.empty:
            # The retail encoder leaves an untouched, degenerate box here whose
            # value varies per file and is not recoverable from a PNG. Any box
            # with right < left reads back as empty, which is what matters.
            records += struct.pack("<IIII", 1, 1, 0, 0)
            continue
        coverage, pixels, stride = _frame_bytes(image, item, transparent)
        blob = sprite.encode_frame(
            coverage, pixels, item.width, item.height, stride
        )
        wide = 1 if len(blob) > 0xFFFF else 0
        if wide:  # the table must be rebuilt with 32-bit entries
            blob = sprite.encode_frame(
                coverage, pixels, item.width, item.height, stride, wide_rows=True
            )
        fmt = {1: PixelFormat.INDEXED8, 2: PixelFormat.RGB555, 0: PixelFormat.MASK}[
            stride
        ]
        records += struct.pack(
            "<IIII", item.left, item.top,
            item.left + item.width - 1, item.top + item.height - 1,
        )
        records += b"RLE2"
        records += struct.pack(
            "<IIHIIII",
            item.width, item.height, int(fmt), len(blob), 0, wide, color_key,
        )
        records += b"pamm"
        records += struct.pack("<I", offset)
        payloads.append(blob)
        offset += len(blob)
        populated += 1

    palette = image.palette or []
    if image.color_type is not png.ColorType.INDEXED:
        palette = []
    body = bytearray(b"IMGRLE")
    body += struct.pack("<IIIII", image_class, 0, 0, entry.columns, entry.rows)
    body += records
    body += struct.pack("<I", len(palette))
    for r, g, b in palette:
        body += bytes((b, g, r, 0))
    if image_class == int(ImageClass.PLAYER_COLOR):
        body += _lookup_table(palette, player_color=True)

    if len(body) != HEADER_SIZE + len(records) + 4 + 4 * len(palette) + (
        512 if image_class == int(ImageClass.PLAYER_COLOR) else 0
    ):  # pragma: no cover - arithmetic guard
        raise SystemExit("internal error: frame table size arithmetic does not close")

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(body))
    store_bytes = b"".join(payloads)
    store_path.write_bytes(store_bytes)

    check = RleImage(bytes(body))
    check.validate()

    print(f"{out}: {len(body)} bytes, {entry.columns}x{entry.rows} frames "
          f"({populated} populated), class {image_class}")
    print(f"{store_path}: {len(store_bytes)} bytes of payload, "
          f"offsets from {args.base_offset}")
    print("  re-read and validated as a frame table")
    print("  note: data_offset addresses the companion store, not the game's "
          "shared rle.mmp")
    return 0


# -- command line ---------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="imsprite",
        description="Inspect, export and re-encode Imperivm sprites.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    def add_store_options(p: argparse.ArgumentParser) -> None:
        p.add_argument("--mmp", type=Path, help="path to the root-level rle.mmp")
        p.add_argument("--game", type=Path, help="the game install directory")

    p_info = sub.add_parser("info", help="describe a sprite's frame table")
    p_info.add_argument("pack", type=Path)
    p_info.add_argument("entry", help=r"e.g. UNITS\BBOWMAN\ATTACK.RLE.MMP")
    p_info.add_argument("--frames", action="store_true", help="list every frame")
    add_store_options(p_info)
    p_info.set_defaults(func=cmd_info)

    p_export = sub.add_parser("export", help="write one sheet PNG and a manifest")
    p_export.add_argument("pack", type=Path)
    p_export.add_argument("entry")
    p_export.add_argument("--out", required=True, help="output directory")
    p_export.add_argument(
        "--rgba", action="store_true", help="flatten instead of keeping the palette"
    )
    p_export.add_argument("--team", help="bake a team ramp: a name, #rrggbb or r,g,b")
    p_export.add_argument(
        "--replace-manifest", action="store_true", help="do not merge into an existing one"
    )
    add_store_options(p_export)
    p_export.set_defaults(func=cmd_export)

    p_all = sub.add_parser("export-all", help="export every sprite in the install")
    p_all.add_argument("--game", type=Path, required=True)
    p_all.add_argument("--out", required=True)
    p_all.add_argument("--rgba", action="store_true")
    p_all.add_argument("--team")
    p_all.add_argument("--packs", nargs="+", help=f"default: {' '.join(SPRITE_PACKS)}")
    p_all.add_argument(
        "--verify",
        action="store_true",
        help="also re-encode every frame and compare with the original bytes",
    )
    p_all.add_argument(
        "--compress", type=int, default=6, help="zlib level for the PNGs, 0..9"
    )
    p_all.add_argument("--mmp", type=Path)
    p_all.set_defaults(func=cmd_export_all)

    p_sheet = sub.add_parser("sheet", help="a contact sheet for visual inspection")
    p_sheet.add_argument("pack", type=Path)
    p_sheet.add_argument("entry")
    p_sheet.add_argument("--out", required=True, help="output PNG")
    p_sheet.add_argument("--shadow", help="shadow sheet to composite underneath")
    p_sheet.add_argument("--no-shadow", action="store_true")
    p_sheet.add_argument("--shadow-alpha", type=int, default=110)
    p_sheet.add_argument("--team")
    p_sheet.add_argument("--background", help="fill colour behind the sprites")
    p_sheet.add_argument("--grid", help="draw cell separators in this colour")
    add_store_options(p_sheet)
    p_sheet.set_defaults(func=cmd_sheet)

    p_import = sub.add_parser("import", help="turn a sheet back into a frame table")
    p_import.add_argument("png")
    p_import.add_argument("manifest", help="manifest.json or the directory holding it")
    p_import.add_argument("--out", required=True, help="frame table to write")
    p_import.add_argument("--store", help="payload file (default: OUT.store)")
    p_import.add_argument("--entry", help="manifest path, if it is not the PNG's")
    p_import.add_argument("--base-offset", type=int, default=0)
    p_import.add_argument("--image-class", type=int, help="override the inferred class")
    p_import.add_argument("--color-key", type=int, help="override the colour key")
    p_import.set_defaults(func=cmd_import)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (RleError, PackFileError, png.PngError, sprite.SpriteError) as error:
        print(f"imsprite: {error}", file=sys.stderr)
        return 2
    except KeyError as error:
        print(f"imsprite: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
