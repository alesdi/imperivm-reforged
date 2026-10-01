"""The export manifest: the contract between the exporters and their consumers.

Every tool that writes assets out of the game also writes a `manifest.json`
next to them. The manifest is what makes an export self-describing, so that the
asset viewer, a modder's editor, and the import path can all work from the
files on disk without reaching back into the original installation.

The rule that shapes this format: **a PNG alone is lossy for this game.** A
sprite sheet loses the frame grid, the per-frame bounding boxes, the draw
anchor, and which palette entries are team colour. A terrain texture loses its
animation period. The manifest carries exactly the parts that the image format
cannot, and nothing that can be recomputed from the image itself.

Layout on disk::

    export/
      manifest.json
      units/bbowman/attack.png
      units/bbowman/attack_shadow.png
      terrain/autumn/grass1024.png

Paths inside the manifest are relative to the manifest, use forward slashes,
and are lowercase. The original pack path is kept alongside so an export can be
traced back to its source.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

FORMAT_VERSION = 1


@dataclass
class FrameEntry:
    """One frame of a sprite sheet.

    `x` and `y` locate the frame's top-left inside the exported sheet. The
    bounding box is the frame's placement on the entity's shared canvas, which
    is what the engine composites against; two frames of the same animation
    have different boxes and that difference is the motion. Dropping it and
    re-centring each frame would visibly break animation.

    `empty` frames occupy no space in the sheet and carry no pixels. They are
    listed anyway so that frame indices stay aligned with the original grid.
    """

    row: int
    column: int
    x: int
    y: int
    width: int
    height: int
    left: int
    top: int
    empty: bool = False


@dataclass
class SpriteEntry:
    """A sprite sheet exported from one `.rle.mmp` image."""

    path: str
    source: str
    rows: int
    columns: int
    canvas_width: int
    canvas_height: int
    pixel_format: str
    indexed: bool
    player_color: bool = False
    player_color_slots: int = 0
    shadow: bool = False
    frames: list[FrameEntry] = field(default_factory=list)


@dataclass
class TerrainEntry:
    """A terrain texture exported from one `.vq` file."""

    path: str
    source: str
    width: int
    height: int
    frames: int = 1
    frame_height: int = 0


@dataclass
class FontEntry:
    """A bitmap font exported from one `.apf` file."""

    path: str
    source: str
    face: str
    point_size: int
    height: int
    ascent: int
    descent: int
    glyphs: int


@dataclass
class MaskEntry:
    """A passability mask exported from one GRID file."""

    path: str
    source: str
    cell_size: int
    bits_per_cell: int
    cells_x: int
    cells_y: int


@dataclass
class Manifest:
    """The root document written as `manifest.json`."""

    version: int = FORMAT_VERSION
    game: str = "Imperivm: Great Battles of Rome"
    tool: str = "imperivm-reforged"
    sprites: list[SpriteEntry] = field(default_factory=list)
    terrain: list[TerrainEntry] = field(default_factory=list)
    fonts: list[FontEntry] = field(default_factory=list)
    masks: list[MaskEntry] = field(default_factory=list)

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2, sort_keys=False)

    def write(self, directory) -> Path:
        """Write `manifest.json` into `directory` and return its path."""
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "manifest.json"
        path.write_text(self.to_json(), encoding="utf-8")
        return path

    @classmethod
    def read(cls, directory) -> "Manifest":
        """Load the manifest from `directory`."""
        raw = json.loads((Path(directory) / "manifest.json").read_text(encoding="utf-8"))
        if raw.get("version") != FORMAT_VERSION:
            raise ValueError(
                f"manifest version {raw.get('version')} is not supported, "
                f"this tool writes version {FORMAT_VERSION}"
            )
        return cls(
            version=raw["version"],
            game=raw.get("game", ""),
            tool=raw.get("tool", ""),
            sprites=[
                SpriteEntry(**{**s, "frames": [FrameEntry(**f) for f in s.get("frames", [])]})
                for s in raw.get("sprites", [])
            ],
            terrain=[TerrainEntry(**t) for t in raw.get("terrain", [])],
            fonts=[FontEntry(**f) for f in raw.get("fonts", [])],
            masks=[MaskEntry(**m) for m in raw.get("masks", [])],
        )


def export_name(pack_path: str) -> str:
    """Turn a pack path into a relative export path.

    ``UNITS\\BBOWMAN\\ATTACK.RLE.MMP`` becomes ``units/bbowman/attack.png``.
    """
    name = pack_path.replace("\\", "/").lower()
    for suffix in (".rle.mmp", ".ent.xml", ".vq", ".apf", ".pass", ".bmp"):
        if name.endswith(suffix):
            name = name[: -len(suffix)]
            break
    return name + ".png"
