"""`imterrain` — inspect and export the terrain textures (`.vq`).

Specification: docs/formats/vq.md
Usage guide:   docs/tools/imterrain.md

The 55 textures live in `Packs/Terrain.pak`, but the `.vq` header does not say
everything a consumer needs. Two facts only exist in `DATA\\TERRAINS.XML`
(inside `Packs/data.pak`):

* the terrain index `z`, which is the number map files store per tile, and
* `frames`, the animation frame count. `DWATER.VQ` and `SWATER.VQ` are 440x4800
  vertical filmstrips of 15 frames of 440x320; nothing in the `.vq` header says
  so.

This tool therefore always reads the XML alongside the pack, prints the layer
information in `list` and `info`, and records `frames`/`frame_height` in the
manifest so that an exported filmstrip can still be animated.
"""

from __future__ import annotations

import argparse
import os
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

from imperivm import png
from imperivm.formats.pak import PackFile
from imperivm.formats.vq import VQImage
from imperivm.manifest import Manifest, TerrainEntry, export_name

TERRAIN_PACK = "Terrain.pak"
DATA_PACK = "data.pak"
TERRAINS_XML = "DATA\\TERRAINS.XML"

# The three directories `%season%` expands to inside Terrain.pak.
SEASONS = ("SPRING", "AUTUMN", "WINTER")


# ---------------------------------------------------------------- installation


def default_game_root() -> Path:
    """Where to look for the installation when `--game` is not given."""
    return Path(os.environ.get("IMPERIVM_ROOT", "."))


def packs_dir(game: Path) -> Path:
    game = Path(game)
    if (game / "Packs").is_dir():
        return game / "Packs"
    if game.is_dir() and game.name.lower() == "packs":
        return game
    raise SystemExit(f"{game}: no Packs/ directory here, pass --game <install dir>")


def member_key(name: str) -> str:
    """Normalise a pack member name for comparison."""
    return name.replace("/", "\\").upper()


# ------------------------------------------------------------------- terrain xml


@dataclass(frozen=True)
class Layer:
    """One `<layer>` element of `DATA\\TERRAINS.XML`."""

    z: int
    type: int
    display: str
    image: str
    frames: int
    passable: bool
    attrib: dict[str, str]

    @property
    def members(self) -> list[str]:
        """The pack members this layer's `image` resolves to.

        `%season%` expands to all three season directories, because the
        substitution is made per map and every season ships the full set.
        """
        path = member_key(self.image)
        if "%SEASON%" in path:
            return [path.replace("%SEASON%", season) for season in SEASONS]
        return [path]


def load_layers(packs: Path) -> list[Layer]:
    """Parse the 41 `<layer>` elements of `DATA\\TERRAINS.XML`."""
    raw = PackFile(packs / DATA_PACK).read(TERRAINS_XML).decode("cp1252")
    root = ET.fromstring(raw)
    layers: list[Layer] = []
    for element in root.iter("layer"):
        a = element.attrib
        layers.append(
            Layer(
                z=int(a.get("z", "-1")),
                type=int(a.get("type", "-1")),
                display=a.get("display", ""),
                image=a.get("image", ""),
                frames=int(a.get("frames", "1")),
                passable=a.get("passable", "1") != "0",
                attrib=dict(a),
            )
        )
    return layers


def layers_by_member(layers: list[Layer]) -> dict[str, list[Layer]]:
    index: dict[str, list[Layer]] = {}
    for layer in layers:
        for member in layer.members:
            index.setdefault(member, []).append(layer)
    return index


def frames_of(member: str, index: dict[str, list[Layer]]) -> int:
    """Frame count declared for `member`, or 1 when no layer animates it."""
    return max((layer.frames for layer in index.get(member_key(member), [])), default=1)


# --------------------------------------------------------------------- textures


def texture_members(pack: PackFile) -> list[str]:
    return [e.name for e in pack.entries if e.name.upper().endswith(".VQ")]


def resolve(pack: PackFile, name: str) -> str:
    """Turn a user-supplied name into a pack member name.

    Accepts the full member name, a season-relative path, or a bare stem:
    `TERRAIN\\AUTUMN\\GRASS1024.VQ`, `autumn/grass1024`, `invalid`.
    """
    members = texture_members(pack)
    wanted = member_key(name)
    candidates = [wanted]
    if not wanted.endswith(".VQ"):
        candidates.append(wanted + ".VQ")
    candidates += ["TERRAIN\\" + c for c in list(candidates)]

    keys = {member_key(m): m for m in members}
    for candidate in candidates:
        if candidate in keys:
            return keys[candidate]

    # Last resort: unique suffix match, so `grass1024` alone is an error but
    # `spring/grass1024` is not.
    suffix_hits = [m for k, m in keys.items() if k.endswith("\\" + candidates[-1].split("\\")[-1])]
    if len(suffix_hits) == 1:
        return suffix_hits[0]
    if suffix_hits:
        raise SystemExit(
            f"{name!r} is ambiguous, it matches:\n  " + "\n  ".join(sorted(suffix_hits))
        )
    raise SystemExit(f"{name!r} is not a texture in {TERRAIN_PACK}")


def load(pack: PackFile, member: str) -> VQImage:
    image = VQImage(pack.read(member), member)
    image.validate()
    return image


def to_png_image(image: VQImage, y0: int = 0, height: int | None = None) -> png.Image:
    """Wrap a decoded texture, or a horizontal slice of it, as an RGB image."""
    height = image.height if height is None else height
    rgb = bytes(image.rgb())
    stride = image.width * 3
    return png.Image(
        image.width,
        height,
        png.ColorType.RGB,
        rgb[y0 * stride : (y0 + height) * stride],
    )


def frame_slices(image: VQImage, frames: int) -> tuple[int, int]:
    """Validate a filmstrip and return `(frames, frame_height)`."""
    if frames <= 1:
        return 1, image.height
    if image.height % frames:
        raise SystemExit(
            f"{image.name}: {image.height} rows do not divide into {frames} frames"
        )
    return frames, image.height // frames


# --------------------------------------------------------------------- manifest


def merge_manifest(directory: Path, entries: list[TerrainEntry]) -> Path:
    """Replace only the `terrain` section of an existing manifest."""
    try:
        manifest = Manifest.read(directory)
    except FileNotFoundError:
        manifest = Manifest()
    manifest.terrain = entries
    return manifest.write(directory)


# --------------------------------------------------------------------- commands


def cmd_list(args: argparse.Namespace) -> int:
    packs = packs_dir(args.game)
    pack = PackFile(packs / TERRAIN_PACK)
    index = layers_by_member(load_layers(packs))

    print(f"{'member':<34} {'size':>11}  {'codebook':>8} {'idx':>3}  frames  layers")
    for member in texture_members(pack):
        image = load(pack, member)
        hits = index.get(member_key(member), [])
        frames = frames_of(member, index)
        zs = ",".join(str(layer.z) for layer in hits) or "-"
        size = f"{image.width}x{image.height}"
        print(
            f"{member:<34} {size:>11}  "
            f"{image.header.codebook_len:>8} {image.header.index_size:>3}"
            f"  {frames:>6}  z={zs}"
        )
    print(f"\n{len(texture_members(pack))} textures in {TERRAIN_PACK}")
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    packs = packs_dir(args.game)
    pack = PackFile(packs / TERRAIN_PACK)
    member = resolve(pack, args.name)
    image = load(pack, member)
    index = layers_by_member(load_layers(packs))
    header = image.header
    frames, frame_height = frame_slices(image, frames_of(member, index))

    print(f"{member}: {image.width}x{image.height} X1R5G5B5")
    print(f"  blocks       {header.blocks_x} x {header.blocks_y} "
          f"of {header.block_width}x{header.block_height}")
    print(f"  codebook     {header.codebook_len} entries, {header.codebook_bytes} bytes")
    print(f"  indices      {header.index_count} x {header.index_size} byte")
    print(f"  distinct     {len(set(image.indices))} codebook entries referenced")
    print(f"  file         {header.expected_size} bytes")
    if frames > 1:
        print(f"  animation    {frames} frames of {image.width}x{frame_height} "
              f"(frames= in TERRAINS.XML)")
    else:
        print("  animation    none")
    for layer in index.get(member_key(member), []):
        extras = " ".join(
            f"{k}={v}" for k, v in layer.attrib.items()
            if k not in ("z", "type", "display", "image", "minimap")
        )
        print(f"  layer        z={layer.z} type={layer.type} "
              f"{layer.display or '(unnamed)'!r} {extras}".rstrip())
    if member_key(member) not in index:
        print("  layer        none — no <layer> in TERRAINS.XML references this file")
    return 0


def _write_frames(image: VQImage, frames: int, frame_height: int, directory: Path) -> list[Path]:
    directory.mkdir(parents=True, exist_ok=True)
    written = []
    for i in range(frames):
        path = directory / f"{i:02d}.png"
        png.write(path, to_png_image(image, i * frame_height, frame_height))
        written.append(path)
    return written


def cmd_export(args: argparse.Namespace) -> int:
    packs = packs_dir(args.game)
    pack = PackFile(packs / TERRAIN_PACK)
    member = resolve(pack, args.name)
    image = load(pack, member)
    index = layers_by_member(load_layers(packs))
    frames, frame_height = frame_slices(image, frames_of(member, index))

    stem = Path(member.replace("\\", "/")).stem.lower()
    if args.split_frames and frames > 1:
        directory = Path(args.output) if args.output else Path(stem)
        written = _write_frames(image, frames, frame_height, directory)
        print(f"{member}: {frames} frames of {image.width}x{frame_height} -> {directory}/")
        for path in written:
            print(f"  {path}")
        return 0

    output = Path(args.output) if args.output else Path(stem + ".png")
    if output.is_dir():
        output = output / (stem + ".png")
    output.parent.mkdir(parents=True, exist_ok=True)
    png.write(output, to_png_image(image))
    note = f"  ({frames} frames of {frame_height} rows)" if frames > 1 else ""
    print(f"{member}: {image.width}x{image.height} -> {output}{note}")
    return 0


def cmd_export_all(args: argparse.Namespace) -> int:
    packs = packs_dir(args.game)
    pack = PackFile(packs / TERRAIN_PACK)
    index = layers_by_member(load_layers(packs))
    directory = Path(args.output)

    entries: list[TerrainEntry] = []
    count = 0
    for member in texture_members(pack):
        image = load(pack, member)
        frames, frame_height = frame_slices(image, frames_of(member, index))
        relative = export_name(member)

        if args.split_frames and frames > 1:
            folder = directory / relative[: -len(".png")]
            _write_frames(image, frames, frame_height, folder)
            base = relative[: -len(".png")]
            for i in range(frames):
                entries.append(
                    TerrainEntry(
                        path=f"{base}/{i:02d}.png",
                        source=member,
                        width=image.width,
                        height=frame_height,
                        frames=frames,
                        frame_height=frame_height,
                    )
                )
            count += 1
            if args.verbose:
                print(f"  {member} -> {base}/ ({frames} frames)")
            continue

        target = directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        png.write(target, to_png_image(image))
        entries.append(
            TerrainEntry(
                path=relative,
                source=member,
                width=image.width,
                height=image.height,
                frames=frames,
                frame_height=frame_height if frames > 1 else 0,
            )
        )
        count += 1
        if args.verbose:
            print(f"  {member} -> {relative}")

    manifest = merge_manifest(directory, entries)
    animated = sum(1 for e in entries if e.frames > 1)
    print(f"{count} textures exported to {directory}")
    print(f"{len(entries)} terrain entries in {manifest} ({animated} animated)")
    return 0


# ------------------------------------------------------------------------- main


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="imterrain",
        description="Inspect and export the terrain textures of Imperivm.",
    )
    parser.add_argument(
        "--game",
        type=Path,
        default=default_game_root(),
        help="the game installation directory (default: $IMPERIVM_ROOT or .)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    listing = subparsers.add_parser("list", help="list every texture with its layers")
    listing.set_defaults(func=cmd_list)

    info = subparsers.add_parser("info", help="describe one texture")
    info.add_argument("name", help="e.g. autumn/grass1024, TERRAIN\\SWATER.VQ, invalid")
    info.set_defaults(func=cmd_info)

    export = subparsers.add_parser("export", help="export one texture as an RGB PNG")
    export.add_argument("name")
    export.add_argument("-o", "--output", help="PNG path, or directory with --split-frames")
    export.add_argument(
        "--split-frames",
        action="store_true",
        help="write animated water as numbered frames instead of one filmstrip",
    )
    export.set_defaults(func=cmd_export)

    export_all = subparsers.add_parser(
        "export-all", help="export every texture and update manifest.json"
    )
    export_all.add_argument("-o", "--output", required=True, help="export directory")
    export_all.add_argument("--split-frames", action="store_true")
    export_all.add_argument("-v", "--verbose", action="store_true")
    export_all.set_defaults(func=cmd_export_all)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (OSError, KeyError, ValueError) as error:
        print(f"imterrain: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
