"""Reader for the contents of an Imperivm scenario container: the documents inside a `.bfhp`.

Specification: docs/formats/map.md

The container itself is decoded by :mod:`imperivm.formats.bfhp`; this module reads the
documents that live inside one — the game and player setup, the object list, the six
terrain layers, and the narrative furniture (labels, notes, conversations, territories).

Everything here is world space. World coordinates are unsigned and measured from the
top-left corner of the map; a map is a square of 8192, 16384 or 32768 world units.

Reference implementation: correctness and legibility over speed. The terrain layers are
decoded lazily, one cell or one row at a time, because a 32768-unit map's passability
layer is 2048 x 2048 cells and materialising every layer of every map as Python integers
is not worth the memory.
"""

from __future__ import annotations

import struct
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

from .bfhp import BlockFile
from .lzis import decompress

# Documents are byte strings within cp1252, like every other name in the engine.
TEXT_ENCODING = "cp1252"

GRID_MAGIC = b"DIRG"  # the FourCC 'GRID' stored little-endian
GRID_HEADER_SIZE = 20

#: The six terrain layers, in the order they are described in the specification.
LAYER_NAMES = ("pass", "height", "light", "terrain", "decor", "trans")

#: ``layer -> (cell_size, bits_per_cell)`` as authored by the retail editor. The terrain
#: layer is the one exception: authored maps store 8 bits per cell, but the five blank
#: template containers in ``Packs/`` store 4. Both are legal; see the specification.
LAYER_GEOMETRY = {
    "pass": (16, 1),
    "height": (32, 8),
    "light": (32, 8),
    "terrain": (64, 8),
    "decor": (64, 16),
    "trans": (64, 4),
}

#: Alternative bit depths a layer is known to appear in.
LAYER_ALT_DEPTHS = {"terrain": (4,)}

#: The world sizes the shipped maps use.
WORLD_SIZES = (8192, 16384, 32768)

#: Neutral value of the light layer. The layer is a brightness offset around this, not an
#: absolute intensity; deep water and the "Invalid" terrain are 16 everywhere.
LIGHT_NEUTRAL = 16

#: One decor sub-cell offset step, in world units. See :class:`DecorCell`.
DECOR_OFFSET_STEP = 4

#: Number of `<layer>` elements in ``DATA\\TERRAINS.XML``; terrain cell values index it.
TERRAIN_LAYER_COUNT = 41

#: Highest `type` in ``MAPOBJECTS\\DECORS\\DECORS.INI``; decor cell kinds index it, 1-based.
DECOR_TYPE_COUNT = 199

#: Player slots in a container. `player<i>.xml` exists for every `i` in this range.
PLAYER_SLOTS = 16

#: `relations` is this many bytes: one four-byte record per player slot.
RELATION_RECORD = 4
RELATION_BYTES = PLAYER_SLOTS * RELATION_RECORD

#: Byte within a relation record that actually carries the value. The other three are
#: always zero in every retail file.
RELATION_VALUE_BYTE = 3

#: Relation values seen in the retail data. Only `SELF` and `ALLIED` have a confirmed
#: meaning; see "What is still unknown" in the specification.
RELATION_NONE = 0x00
RELATION_ALLIED = 0x15
RELATION_SELF = 0x35

#: 0xCD, the MSVC uninitialised-heap fill, which fills the `relations` of the sixteen
#: placeholder records in `randommap.BFHP`'s `players.xml`.
RELATION_UNINITIALISED = 0xCD

#: 0xCDCDCDCD as a signed 32-bit int: the same MSVC uninitialised-heap fill, which leaked
#: into the `<start_pt>` of `randommap.BFHP`. `gamedata.py` records the same constant in
#: seven `<state>` offsets, so a loader has to expect it.
UNINITIALISED = -842150451

#: `<scriptobj flags>` bits whose meaning is established. See the specification.
FLAG_PRESENT = 1 << 31  # set on every object in every map
FLAG_UNIT = 1 << 22  # carries Level / UnitFlags / stamina
FLAG_IN_SETTLEMENT = 1 << 23  # the object is a `<settlement>` member
FLAG_HERO = 1 << 24  # the class is a CVXHero
#: Bits 0..15 are a one-hot owner mask: `1 << (player - 1)`.
FLAG_PLAYER_MASK = 0xFFFF

#: `<group type>` values.
GROUP_LIST = 1  # a multi-member object list, addressed by name from a script
GROUP_ALIAS = 0  # exactly one member; a name for a single object

#: `<scriptobj type>` on an area object.
AREA_RECT = 0
AREA_CIRCLE = 1


class MapError(Exception):
    """Raised when a document inside a container does not conform to the specification."""


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------


def _text(data: bytes) -> str:
    return data.decode(TEXT_ENCODING)


def _parse(data: bytes, source: str, expect: str) -> ET.Element:
    """Parse one document and check its root element."""
    try:
        root = ET.fromstring(_text(data))
    except ET.ParseError as exc:
        raise MapError(f"{source}: not well-formed XML ({exc})") from exc
    if root.tag != expect:
        raise MapError(f"{source}: root element is <{root.tag}>, expected <{expect}>")
    return root


def _int(element: ET.Element, key: str, default: int = 0) -> int:
    """Integer attribute, tolerating a missing or empty value. 0 is a real value."""
    raw = element.get(key)
    if raw is None or raw == "":
        return default
    return int(raw, 0)


def _opt_int(element: ET.Element, key: str) -> int | None:
    raw = element.get(key)
    if raw is None or raw == "":
        return None
    return int(raw, 0)


# ---------------------------------------------------------------------------
# terrain layers
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class MapGrid:
    """One `GRID` container: a rectangular grid covering a square patch of world space.

    This is the same container :mod:`imperivm.formats.pass_mask` reads, carrying the four
    bit depths the terrain layers use rather than only the 1 and 8 a `.pass` mask needs.
    Cells are not materialised: ``data`` is the file, and cells are decoded on demand.

    ``cell_size`` is the width of one cell in world units, so the grid spans ``extent_x``
    by ``extent_y`` world units. Rows run top to bottom, columns left to right.
    """

    name: str
    cell_size: int
    bits_per_cell: int
    extent_x: int
    extent_y: int
    data: bytes

    @property
    def width(self) -> int:
        return self.extent_x // self.cell_size

    @property
    def height(self) -> int:
        return self.extent_y // self.cell_size

    @property
    def stride(self) -> int:
        """Bytes per stored row."""
        return self.width * self.bits_per_cell // 8

    def __getitem__(self, position: tuple[int, int]) -> int:
        """Value of cell ``(x, y)``, in cell indices."""
        x, y = position
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError(f"{self.name}: cell ({x}, {y}) is outside {self.width}x{self.height}")
        base = GRID_HEADER_SIZE + y * self.stride
        bits = self.bits_per_cell
        if bits == 1:
            return (self.data[base + (x >> 3)] >> (x & 7)) & 1
        if bits == 4:
            return (self.data[base + (x >> 1)] >> ((x & 1) * 4)) & 0xF
        if bits == 8:
            return self.data[base + x]
        if bits == 16:
            return int.from_bytes(self.data[base + 2 * x : base + 2 * x + 2], "little")
        raise MapError(f"{self.name}: unsupported bits_per_cell {bits}")

    def at_world(self, x: int, y: int) -> int:
        """Value of the cell covering world position ``(x, y)``."""
        return self[x // self.cell_size, y // self.cell_size]

    def row(self, y: int) -> tuple[int, ...]:
        """One decoded row, left to right."""
        base = GRID_HEADER_SIZE + y * self.stride
        raw = self.data[base : base + self.stride]
        bits = self.bits_per_cell
        width = self.width
        if bits == 1:
            return tuple((raw[x >> 3] >> (x & 7)) & 1 for x in range(width))
        if bits == 4:
            return tuple((raw[x >> 1] >> ((x & 1) * 4)) & 0xF for x in range(width))
        if bits == 8:
            return tuple(raw)
        if bits == 16:
            return struct.unpack(f"<{width}H", raw)
        raise MapError(f"{self.name}: unsupported bits_per_cell {bits}")

    def rows(self):
        """Iterate the decoded rows, top to bottom."""
        for y in range(self.height):
            yield self.row(y)

    def histogram(self) -> dict[int, int]:
        """``{value: count}`` over every cell. Decodes the whole grid."""
        counts: dict[int, int] = {}
        for row in self.rows():
            for value in row:
                counts[value] = counts.get(value, 0) + 1
        return counts

    def validate(self) -> None:
        """Assert the structural invariants described in the specification."""
        if self.data[: len(GRID_MAGIC)] != GRID_MAGIC:
            raise MapError(f"{self.name}: not a HMMSYS grid (magic mismatch)")
        if self.cell_size <= 0:
            raise MapError(f"{self.name}: cell size {self.cell_size} is not positive")
        if self.extent_x % self.cell_size or self.extent_y % self.cell_size:
            raise MapError(f"{self.name}: extent is not a whole number of cells")
        if self.bits_per_cell not in (1, 4, 8, 16):
            raise MapError(f"{self.name}: unsupported bits_per_cell {self.bits_per_cell}")
        if self.width * self.bits_per_cell % 8:
            raise MapError(f"{self.name}: a row is not a whole number of bytes")
        expected = GRID_HEADER_SIZE + self.stride * self.height
        if len(self.data) != expected:
            raise MapError(f"{self.name}: {len(self.data)} bytes, expected {expected}")


def parse_grid(data: bytes, name: str = "<bytes>") -> MapGrid:
    """Decode a grid container's header. Cells stay in ``data`` until asked for."""
    if len(data) < GRID_HEADER_SIZE:
        raise MapError(f"{name}: shorter than a grid header")
    if data[: len(GRID_MAGIC)] != GRID_MAGIC:
        raise MapError(f"{name}: not a HMMSYS grid (magic mismatch)")
    cell_size, bits_per_cell, extent_x, extent_y = struct.unpack_from("<4I", data, 4)
    if cell_size == 0:
        raise MapError(f"{name}: zero cell size")
    grid = MapGrid(name, cell_size, bits_per_cell, extent_x, extent_y, data)
    grid.validate()
    return grid


@dataclass(frozen=True)
class DecorCell:
    """One non-empty cell of the decor layer, unpacked.

    ``kind`` is the ``type`` number of a section in ``MAPOBJECTS\\DECORS\\DECORS.INI``,
    which names the entity to stamp. ``offset_x`` and ``offset_y`` are the decoration's
    position inside its 64-unit cell, in world units, quantised to 4.
    """

    kind: int
    offset_x: int
    offset_y: int

    @classmethod
    def unpack(cls, value: int) -> DecorCell | None:
        """Decode one 16-bit decor cell. Returns None for an empty cell."""
        if value == 0:
            return None
        return cls(
            kind=value & 0xFF,
            offset_x=((value >> 8) & 0xF) * DECOR_OFFSET_STEP,
            offset_y=((value >> 12) & 0xF) * DECOR_OFFSET_STEP,
        )


@dataclass(frozen=True)
class TerrainLayers:
    """The six `Terrain.*.grid` layers of one map, all covering the same world square."""

    layers: dict[str, MapGrid]

    def __getitem__(self, name: str) -> MapGrid:
        return self.layers[name]

    @property
    def passability(self) -> MapGrid:
        """1 bit per 16 world units. A set bit means the cell is blocked."""
        return self.layers["pass"]

    @property
    def height(self) -> MapGrid:
        """8 bits per 32 world units. 0 is sea level; water is 0 everywhere."""
        return self.layers["height"]

    @property
    def light(self) -> MapGrid:
        """8 bits per 32 world units, a brightness offset around :data:`LIGHT_NEUTRAL`."""
        return self.layers["light"]

    @property
    def terrain(self) -> MapGrid:
        """Terrain type per 64 world units: the `z` of a `<layer>` in `DATA\\TERRAINS.XML`."""
        return self.layers["terrain"]

    @property
    def decor(self) -> MapGrid:
        """16 bits per 64 world units; unpack with :meth:`DecorCell.unpack`."""
        return self.layers["decor"]

    @property
    def transitions(self) -> MapGrid:
        """4 bits per 64 world units. Zero in every shipped map; meaning unknown."""
        return self.layers["trans"]

    def is_blocked(self, x: int, y: int) -> bool:
        """True when world position ``(x, y)`` is impassable."""
        return bool(self.passability.at_world(x, y))

    def decor_at(self, x: int, y: int) -> DecorCell | None:
        """The decoration stamped in the decor cell covering world position ``(x, y)``."""
        return DecorCell.unpack(self.decor.at_world(x, y))

    def decorations(self):
        """Iterate ``(world_x, world_y, DecorCell)`` for every non-empty decor cell."""
        grid = self.decor
        for y in range(grid.height):
            row = grid.row(y)
            for x, value in enumerate(row):
                cell = DecorCell.unpack(value)
                if cell is not None:
                    yield x * grid.cell_size + cell.offset_x, y * grid.cell_size + cell.offset_y, cell

    def validate(self, world_size: tuple[int, int] | None = None) -> None:
        """Check every layer's geometry, and optionally its agreement with the map size."""
        missing = [n for n in LAYER_NAMES if n not in self.layers]
        if missing:
            raise MapError(f"map is missing terrain layers: {', '.join(missing)}")
        for name in LAYER_NAMES:
            grid = self.layers[name]
            grid.validate()
            cell_size, bits = LAYER_GEOMETRY[name]
            if grid.cell_size != cell_size:
                raise MapError(
                    f"{grid.name}: cell size {grid.cell_size}, expected {cell_size}"
                )
            if grid.bits_per_cell != bits and grid.bits_per_cell not in LAYER_ALT_DEPTHS.get(
                name, ()
            ):
                raise MapError(
                    f"{grid.name}: {grid.bits_per_cell} bits per cell, expected {bits}"
                )
            if world_size is not None and (grid.extent_x, grid.extent_y) != world_size:
                raise MapError(
                    f"{grid.name}: extent {grid.extent_x}x{grid.extent_y} does not match "
                    f"the declared world size {world_size[0]}x{world_size[1]}"
                )


# ---------------------------------------------------------------------------
# the object list
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Area:
    """The trigger region of an area object: a circle or an axis-aligned rectangle.

    All values are world units.
    """

    shape: int  # AREA_CIRCLE | AREA_RECT
    x: int = 0
    y: int = 0
    radius: int = 0
    left: int = 0
    top: int = 0
    right: int = 0
    bottom: int = 0

    def contains(self, x: int, y: int) -> bool:
        """Whether `(x, y)` is inside the region.

        The circle test is **not** ``d2 <= r*r``. `gbr.exe` takes a *truncating*
        integer square root of the squared distance (0x0067c3d0) and compares the
        result with the radius, at two independent sites -- the point sampler at
        0x004d6e2e and ``AreaDistTo`` at 0x004d8b0e. ``floor(sqrt(d)) <= r`` is
        exactly ``d < (r+1)**2`` over the non-negative integers, which is what is
        written here: the same answer, and no square root. The difference is the
        one-unit shell at the rim, and it is not a rounding detail -- it is 1,300
        world units of circumference on a 200-unit area.

        ``engine/core/include/imperivm/core/sim/area.hpp`` says the same thing in
        C++, and the two must not drift.
        """
        if self.shape == AREA_CIRCLE:
            dx, dy = x - self.x, y - self.y
            reach = self.radius + 1
            return dx * dx + dy * dy < reach * reach
        return self.left <= x <= self.right and self.top <= y <= self.bottom

    def centre(self) -> tuple[int, int]:
        if self.shape == AREA_CIRCLE:
            return self.x, self.y
        return (self.left + self.right) // 2, (self.top + self.bottom) // 2


@dataclass
class ScriptObject:
    """One `<scriptobj>`: an instance of a class, placed in the world.

    ``class_name`` resolves against the class graph through
    :meth:`imperivm.formats.gamedata.GameData.lookup`, which accepts an `id` or an `altid`.
    ``num`` is the object's identity within its map: unique, and contiguous from 0 in
    document order. ``player`` is 1-based here, unlike the 0-based `id` of `player<i>.xml`;
    subtract one to index the player table.
    """

    class_name: str
    num: int
    x: int
    y: int
    flags: int
    dir_x: int = 0
    dir_y: int = 1
    player: int | None = None
    health_percent: int | None = None
    inventory_size: int | None = None
    stamina: int | None = None
    level: int | None = None
    unit_flags: int | None = None
    display_name: str | None = None
    icon: str | None = None
    items: tuple[str, ...] = ()
    hero_skills: dict[str, int] = field(default_factory=dict)
    area: Area | None = None
    next_map: str | None = None
    target_area: str | None = None
    cargo_type: int | None = None
    cargo_amount: int | None = None
    destination_set: int | None = None
    data: int | None = None
    extra: dict[str, str] = field(default_factory=dict)

    # -- flags -----------------------------------------------------------

    @property
    def owner_mask(self) -> int:
        """The one-hot owner field of ``flags``: ``1 << (player - 1)``."""
        return self.flags & FLAG_PLAYER_MASK

    @property
    def is_unit(self) -> bool:
        """True when the object carries the mobile-unit attribute group."""
        return bool(self.flags & FLAG_UNIT)

    @property
    def is_hero(self) -> bool:
        return bool(self.flags & FLAG_HERO)

    @property
    def in_settlement(self) -> bool:
        return bool(self.flags & FLAG_IN_SETTLEMENT)

    @property
    def position(self) -> tuple[int, int]:
        return self.x, self.y

    @classmethod
    def parse(cls, element: ET.Element) -> ScriptObject:
        attrib = dict(element.attrib)

        def take(key: str) -> str | None:
            return attrib.pop(key, None)

        class_name = take("class")
        if not class_name:
            raise MapError("<scriptobj> without a class")
        obj = cls(
            class_name=class_name,
            num=int(take("num") or 0),
            x=int(take("x") or 0),
            y=int(take("y") or 0),
            flags=int(take("flags") or "0", 0),
        )
        raw = take("dir.x")
        obj.dir_x = 0 if raw in (None, "") else int(raw)
        raw = take("dir.y")
        obj.dir_y = 1 if raw in (None, "") else int(raw)

        for key, name in (
            ("player", "player"),
            ("healthperc", "health_percent"),
            ("inventorysize", "inventory_size"),
            ("stamina", "stamina"),
            ("Level", "level"),
            ("UnitFlags", "unit_flags"),
            ("destination_set", "destination_set"),
            ("data", "data"),
        ):
            raw = take(key)
            if raw not in (None, ""):
                setattr(obj, name, int(raw, 0))

        obj.display_name = take("display_name")
        obj.icon = take("Icon")
        obj.next_map = take("nextmap")
        obj.target_area = take("targetarea")

        # Item slots are `slot0`, `slot1`, ... and are dense from 0.
        items: list[str] = []
        while f"slot{len(items)}" in attrib:
            items.append(attrib.pop(f"slot{len(items)}"))
        obj.items = tuple(items)

        # Hero skill ratings all share the `hs` prefix.
        for key in [k for k in attrib if k.startswith("hs")]:
            obj.hero_skills[key[2:]] = int(attrib.pop(key), 0)

        # `type` means an area shape on an area object and a cargo kind on a wagon; the
        # two are told apart by which other attributes came with it.
        shape = attrib.pop("type", None)
        if shape is not None:
            shape_value = int(shape, 0)
            if "r" in attrib or "ptx" in attrib:
                obj.area = Area(
                    AREA_CIRCLE,
                    x=int(attrib.pop("ptx", "0")),
                    y=int(attrib.pop("pty", "0")),
                    radius=int(attrib.pop("r", "0")),
                )
            elif "left" in attrib:
                obj.area = Area(
                    AREA_RECT,
                    left=int(attrib.pop("left", "0")),
                    top=int(attrib.pop("top", "0")),
                    right=int(attrib.pop("right", "0")),
                    bottom=int(attrib.pop("bottom", "0")),
                )
            else:
                obj.cargo_type = shape_value
            if obj.area is not None and obj.area.shape != shape_value:
                raise MapError(
                    f"object {obj.num}: type={shape_value} disagrees with the "
                    f"area attributes present"
                )
        raw = attrib.pop("amount", None)
        if raw not in (None, ""):
            obj.cargo_amount = int(raw, 0)

        obj.extra = attrib
        return obj

    def validate(self, world_size: tuple[int, int] | None = None) -> None:
        if not self.flags & FLAG_PRESENT:
            raise MapError(f"object {self.num} ({self.class_name}): bit 31 of flags is clear")
        if self.player is not None:
            if not 1 <= self.player <= PLAYER_SLOTS:
                raise MapError(f"object {self.num}: player {self.player} is out of range")
            if self.owner_mask != 1 << (self.player - 1):
                raise MapError(
                    f"object {self.num}: flags owner mask 0x{self.owner_mask:04X} does not "
                    f"match player {self.player}"
                )
        elif self.owner_mask:
            raise MapError(
                f"object {self.num}: flags carry an owner mask but no player attribute"
            )
        if self.is_unit != (self.level is not None):
            raise MapError(
                f"object {self.num}: unit flag bit 22 disagrees with the attributes present"
            )
        if self.area is not None and self.area.centre() != (self.x, self.y):
            raise MapError(
                f"object {self.num}: position {(self.x, self.y)} is not the centre "
                f"{self.area.centre()} of its area"
            )
        if world_size is not None:
            width, height = world_size
            if not (0 <= self.x < width and 0 <= self.y < height):
                raise MapError(
                    f"object {self.num} ({self.class_name}): position "
                    f"({self.x}, {self.y}) is outside the {width}x{height} world"
                )


@dataclass
class Settlement:
    """One `<settlement>`: a named town, its stores, and the buildings that make it up.

    Every member building carries the settlement's own `player`, and
    ``class_of_first_building`` names the class that defines the settlement.
    """

    id: int
    player: int
    name: str
    icon: str
    class_of_first_building: str | None
    population: int
    max_population: int
    gold: int
    max_gold: int
    food: int
    max_food: int
    buildings: list[ScriptObject] = field(default_factory=list)
    extra: dict[str, str] = field(default_factory=dict)

    @classmethod
    def parse(cls, element: ET.Element) -> Settlement:
        attrib = dict(element.attrib)
        settlement = cls(
            id=int(attrib.pop("id", "0")),
            player=int(attrib.pop("player", "0")),
            name=attrib.pop("name", ""),
            icon=attrib.pop("icon", ""),
            class_of_first_building=attrib.pop("classoffirstbuilding", None) or None,
            population=int(attrib.pop("population", "0")),
            max_population=int(attrib.pop("maxpopulation", "0")),
            gold=int(attrib.pop("gold", "0")),
            max_gold=int(attrib.pop("maxgold", "0")),
            food=int(attrib.pop("food", "0")),
            max_food=int(attrib.pop("maxfood", "0")),
        )
        attrib.pop("extrasentries", None)
        settlement.extra = attrib
        settlement.buildings = [ScriptObject.parse(e) for e in element.findall("scriptobj")]
        return settlement

    def validate(self) -> None:
        if not 1 <= self.player <= PLAYER_SLOTS:
            raise MapError(f"settlement {self.id}: player {self.player} is out of range")
        for building in self.buildings:
            if building.player != self.player:
                raise MapError(
                    f"settlement {self.id}: member {building.num} belongs to player "
                    f"{building.player}, not {self.player}"
                )
            if not building.in_settlement:
                raise MapError(
                    f"settlement {self.id}: member {building.num} does not carry bit 23"
                )


@dataclass(frozen=True)
class ObjectGroup:
    """One `<group>`: a script-visible name bound to a set of objects, by `num`.

    ``type`` is :data:`GROUP_LIST` for a multi-member list, which scripts address as an
    object list, or :data:`GROUP_ALIAS` for a group of exactly one, which is how a single
    object — an area, a named hero, a town hall — gets a name.
    """

    name: str
    type: int
    members: tuple[int, ...]

    @property
    def is_alias(self) -> bool:
        return self.type == GROUP_ALIAS

    @classmethod
    def parse(cls, element: ET.Element) -> ObjectGroup:
        return cls(
            name=element.get("name", ""),
            type=_int(element, "type"),
            members=tuple(_int(e, "num") for e in element.findall("obj")),
        )


@dataclass
class ObjectList:
    """A parsed `map.obj.xml`: every object placed on one map."""

    objects: list[ScriptObject] = field(default_factory=list)
    settlements: list[Settlement] = field(default_factory=list)
    groups: list[ObjectGroup] = field(default_factory=list)

    @classmethod
    def parse(cls, data: bytes, source: str = "map.obj.xml") -> ObjectList:
        root = _parse(data, source, "mapobject")
        result = cls()
        for element in root:
            if element.tag == "scriptobj":
                result.objects.append(ScriptObject.parse(element))
            elif element.tag == "settlement":
                settlement = Settlement.parse(element)
                result.settlements.append(settlement)
                result.objects.extend(settlement.buildings)
            elif element.tag == "group":
                result.groups.append(ObjectGroup.parse(element))
            else:
                raise MapError(f"{source}: unexpected element <{element.tag}> in <mapobject>")
        return result

    def by_num(self) -> dict[int, ScriptObject]:
        return {o.num: o for o in self.objects}

    def by_player(self) -> dict[int, list[ScriptObject]]:
        owned: dict[int, list[ScriptObject]] = {}
        for obj in self.objects:
            if obj.player is not None:
                owned.setdefault(obj.player, []).append(obj)
        return owned

    def areas(self) -> list[ScriptObject]:
        return [o for o in self.objects if o.area is not None]

    def class_names(self) -> set[str]:
        return {o.class_name for o in self.objects} | {
            s.class_of_first_building for s in self.settlements if s.class_of_first_building
        }

    def validate(self, world_size: tuple[int, int] | None = None) -> None:
        index = self.by_num()
        if len(index) != len(self.objects):
            raise MapError("map.obj.xml: two objects share a num")
        if sorted(index) != list(range(len(self.objects))):
            raise MapError("map.obj.xml: object nums are not contiguous from 0")
        for obj in self.objects:
            obj.validate(world_size)
        seen: set[int] = set()
        for settlement in self.settlements:
            if settlement.id in seen:
                raise MapError(f"map.obj.xml: two settlements share id {settlement.id}")
            seen.add(settlement.id)
            settlement.validate()
        for group in self.groups:
            if group.type == GROUP_ALIAS and len(group.members) != 1:
                raise MapError(
                    f"map.obj.xml: alias group {group.name!r} has "
                    f"{len(group.members)} members, expected 1"
                )
            for num in group.members:
                if num not in index:
                    raise MapError(
                        f"map.obj.xml: group {group.name!r} references object {num}, "
                        f"which does not exist"
                    )


# ---------------------------------------------------------------------------
# setup documents
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class GameInfo:
    """`game.xml`: what the whole container is, independent of any single map."""

    game_type: int  # 0 scenario, 1 adventure, 2 conquest
    name: str
    author: str
    description: str
    start_map: int
    last_edited_map: int
    victory_condition: str
    victory_threshold: str
    single_only: bool
    start_player: int
    season: str  # "" when the attribute is absent, which means spring
    user_interface: int | None
    extra: dict[str, str] = field(default_factory=dict)

    @classmethod
    def parse(cls, data: bytes, source: str = "game.xml") -> GameInfo:
        root = _parse(data, source, "game")
        element = root.find("properties")
        if element is None:
            raise MapError(f"{source}: <game> has no <properties>")
        attrib = dict(element.attrib)
        for key in (
            "game_type", "name", "author", "description", "start_map", "last_edited_map",
            "victory_condition", "victory_threshold", "single_only", "start_player",
            "season", "user_interface",
        ):
            attrib.pop(key, None)
        return cls(
            game_type=_int(element, "game_type"),
            name=element.get("name", ""),
            author=element.get("author", ""),
            description=element.get("description", ""),
            start_map=_int(element, "start_map", 1),
            last_edited_map=_int(element, "last_edited_map", 1),
            victory_condition=element.get("victory_condition", ""),
            victory_threshold=element.get("victory_threshold", ""),
            single_only=element.get("single_only", "0") != "0",
            start_player=_int(element, "start_player"),
            season=element.get("season", ""),
            user_interface=_opt_int(element, "user_interface"),
            extra=attrib,
        )


@dataclass(frozen=True)
class Player:
    """One `player<i>.xml` (or one `<playerdata>` of a bundled `players.xml`).

    ``id`` is 0-based. Objects refer to a player 1-based, so ``player`` on a
    :class:`ScriptObject` is ``id + 1``.
    """

    id: int
    name: str
    race: str
    allowed_races: str
    control: str  # Both | Computer | Disabled
    ai: str
    difficulty: int | None
    start_x: int
    start_y: int
    color: int
    allied: bool
    bonus: int | None
    relations: bytes
    extra: dict[str, str] = field(default_factory=dict)

    @property
    def rgb(self) -> tuple[int, int, int]:
        """``color`` as 8-bit RGB. The stored value is 5 bits per channel, R high."""
        r, g, b = (self.color >> 10) & 31, (self.color >> 5) & 31, self.color & 31
        return r * 255 // 31, g * 255 // 31, b * 255 // 31

    def relation_to(self, other: int) -> int:
        """The relation byte this player holds toward player ``other`` (0-based)."""
        return self.relations[other * RELATION_RECORD + RELATION_VALUE_BYTE]

    def allies(self) -> list[int]:
        """Player ids this player is recorded as allied with, excluding itself."""
        return [
            i
            for i in range(PLAYER_SLOTS)
            if i != self.id and self.relation_to(i) == RELATION_ALLIED
        ]

    @property
    def is_active(self) -> bool:
        return self.control != "Disabled"

    @classmethod
    def from_element(cls, element: ET.Element, source: str = "player.xml") -> Player:
        attrib = dict(element.attrib)
        for key in (
            "id", "name", "race", "AllowedRaces", "control", "AI", "difficulty",
            "startx", "starty", "color", "allied", "sharedvictory", "bonus", "relations",
        ):
            attrib.pop(key, None)
        raw = element.get("relations", "")
        try:
            relations = bytes.fromhex(raw)
        except ValueError as exc:
            raise MapError(f"{source}: relations is not hexadecimal ({exc})") from exc
        # `players.xml` calls the shared-victory flag `sharedvictory`; the per-player
        # files call the same thing `allied`.
        allied = element.get("allied", element.get("sharedvictory", "0")) != "0"
        return cls(
            id=_int(element, "id"),
            name=element.get("name", ""),
            race=element.get("race", ""),
            allowed_races=element.get("AllowedRaces", ""),
            control=element.get("control", ""),
            ai=element.get("AI", ""),
            difficulty=_opt_int(element, "difficulty"),
            start_x=_int(element, "startx"),
            start_y=_int(element, "starty"),
            color=_int(element, "color"),
            allied=allied,
            bonus=_opt_int(element, "bonus"),
            relations=relations,
            extra=attrib,
        )

    @classmethod
    def parse(cls, data: bytes, source: str = "player.xml") -> Player:
        return cls.from_element(_parse(data, source, "playerdata"), source)

    def validate(self, slot: int | None = None) -> None:
        if len(self.relations) != RELATION_BYTES:
            raise MapError(
                f"player {self.id}: relations is {len(self.relations)} bytes, "
                f"expected {RELATION_BYTES}"
            )
        if self.id < 0:
            return  # a `players.xml` placeholder: id -1, relations filled with 0xCD
        if slot is not None and self.id != slot:
            raise MapError(f"player{slot}.xml declares id {self.id}")
        if not 0 <= self.id < PLAYER_SLOTS:
            raise MapError(f"player id {self.id} is out of range")
        for i in range(PLAYER_SLOTS):
            record = self.relations[i * RELATION_RECORD : (i + 1) * RELATION_RECORD]
            if any(record[:RELATION_VALUE_BYTE]):
                raise MapError(
                    f"player {self.id}: relation record {i} has data outside its value byte"
                )
        if self.relation_to(self.id) != RELATION_SELF:
            raise MapError(
                f"player {self.id}: self-relation is "
                f"0x{self.relation_to(self.id):02X}, expected 0x{RELATION_SELF:02X}"
            )


@dataclass(frozen=True)
class MapInfo:
    """`map.xml`: the world square, the editor's camera bookmark, and the fog switches."""

    name: str
    display_name: str
    description: str
    size_x: int
    size_y: int
    start_x: int
    start_y: int
    explored_art: str
    no_fog: bool
    no_explore: bool
    persist_state: bool
    extra: dict[str, str] = field(default_factory=dict)

    @property
    def size(self) -> tuple[int, int]:
        return self.size_x, self.size_y

    @property
    def start_point_is_garbage(self) -> bool:
        """True for the one retail map whose start point is the uninitialised fill."""
        return self.start_x == UNINITIALISED or self.start_y == UNINITIALISED

    @classmethod
    def parse(cls, data: bytes, source: str = "map.xml") -> MapInfo:
        root = _parse(data, source, "map")
        size = root.find("size")
        if size is None:
            raise MapError(f"{source}: <map> has no <size>")
        start = root.find("start_pt")
        art = root.find("user_art")
        expl = root.find("expl")
        attrib = dict(root.attrib)
        for key in ("name", "displayname", "descr", "persist_state"):
            attrib.pop(key, None)
        return cls(
            name=root.get("name", ""),
            display_name=root.get("displayname", ""),
            description=root.get("descr", ""),
            size_x=_int(size, "x"),
            size_y=_int(size, "y"),
            start_x=_int(start, "x") if start is not None else 0,
            start_y=_int(start, "y") if start is not None else 0,
            explored_art=art.get("explored", "") if art is not None else "",
            no_fog=expl is not None and expl.get("NoFog", "0") != "0",
            no_explore=expl is not None and expl.get("NoExplore", "0") != "0",
            persist_state=root.get("persist_state", "0") != "0",
            extra=attrib,
        )

    def validate(self) -> None:
        if self.size_x != self.size_y:
            raise MapError(f"map.xml: world {self.size_x}x{self.size_y} is not square")
        if self.size_x not in WORLD_SIZES:
            raise MapError(f"map.xml: world size {self.size_x} is not one of {WORLD_SIZES}")
        if self.start_point_is_garbage:
            return
        if not (0 <= self.start_x < self.size_x and 0 <= self.start_y < self.size_y):
            raise MapError(
                f"map.xml: start point ({self.start_x}, {self.start_y}) is outside the world"
            )


# ---------------------------------------------------------------------------
# narrative furniture
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Label:
    """One `<label>` of `labels.xml`: a place name pinned at a world position."""

    text: str
    x: int
    y: int

    @staticmethod
    def parse_all(data: bytes, source: str = "labels.xml") -> list[Label]:
        root = _parse(data, source, "root")
        return [
            Label(e.get("text", ""), _int(e, "x"), _int(e, "y")) for e in root.findall("label")
        ]


@dataclass(frozen=True)
class Note:
    """One `<note>` of `Notes.xml`: a journal entry a script can award."""

    id: str
    title: str
    text: str
    icon: str
    map: str
    show_on_minimap: bool
    location_x: int
    location_y: int

    @property
    def has_location(self) -> bool:
        return self.location_x >= 0 and self.location_y >= 0

    @staticmethod
    def parse_all(data: bytes, source: str = "Notes.xml") -> list[Note]:
        root = _parse(data, source, "notes")
        return [
            Note(
                id=e.get("id", ""),
                title=e.get("title", ""),
                text=e.get("text", ""),
                icon=e.get("icon", ""),
                map=e.get("map", ""),
                show_on_minimap=e.get("show_on_minimap", "0") != "0",
                location_x=_int(e, "locationx", -1),
                location_y=_int(e, "locationy", -1),
            )
            for e in root.findall("note")
        ]


#: Virtual roots a `<sequence script>` path can start with. `CurrentGame/` is the
#: container root, `CurrentMap/` the map's own directory.
SCRIPT_ROOT_GAME = "CurrentGame"
SCRIPT_ROOT_MAP = "CurrentMap"


@dataclass(frozen=True)
class SequenceRef:
    """One `<sequence>` of a `sequences.xml`: a named trigger script.

    ``script`` is a virtual path rooted at :data:`SCRIPT_ROOT_GAME` or
    :data:`SCRIPT_ROOT_MAP`; :meth:`resolve` turns it into a container entry name.
    """

    name: str
    script: str
    wizard: str
    autorun_allowed: bool

    def resolve(self, base: str = "") -> str:
        """The container entry this reference names. ``base`` is the owning directory."""
        _root, _, tail = self.script.partition("/")
        return f"{base}/{tail}".lstrip("/")

    @staticmethod
    def parse_all(data: bytes, source: str = "sequences.xml") -> list[SequenceRef]:
        root = _parse(data, source, "sequences")
        return [
            SequenceRef(
                name=e.get("name", ""),
                script=e.get("script", ""),
                wizard=e.get("wizard", ""),
                autorun_allowed=e.get("autorunallowed", "yes") != "no",
            )
            for e in root.findall("sequence")
        ]


@dataclass(frozen=True)
class NoteRef:
    """One `<note>` of a `Notes.xml`: a mission objective a script can pin.

    Specification: docs/formats/adventure.md.

    A container carries two of these documents -- its own `Notes.xml` and each
    map's `Maps/<n>/Notes.xml` -- and the engine searches both. Declaring a note
    puts nothing on the player's list; `GiveNote(id)` does that, and **a note
    nothing declares cannot be given**.
    """

    id: str
    title: str
    text: str
    icon: str
    map: str
    show_on_minimap: bool
    location: tuple[int, int]

    @staticmethod
    def parse_all(data: bytes, source: str = "Notes.xml") -> list[NoteRef]:
        root = _parse(data, source, "notes")

        def number(element, name: str) -> int:
            try:
                return int(element.get(name, "-1"))
            except ValueError:
                return -1

        return [
            NoteRef(
                id=e.get("id", ""),
                title=e.get("title", ""),
                text=e.get("text", ""),
                icon=e.get("icon", ""),
                map=e.get("map", ""),
                show_on_minimap=e.get("show_on_minimap", "0") not in ("", "0"),
                location=(number(e, "locationx"), number(e, "locationy")),
            )
            for e in root.findall("note")
        ]


@dataclass(frozen=True)
class Phrase:
    """One `<phrase>` of a conversation: a line, and how the conversation continues."""

    actor: str
    text: str
    followup: str  # first | end | choice
    label: str | None
    followup_phrases: tuple[str, ...]
    choice_text: str | None
    condition: str | None  # inline .vs source
    action: str | None  # inline .vs source
    ret: str | None  # inline .vs source
    comments: str | None


@dataclass(frozen=True)
class Conversation:
    """One `cnv<k>.conv.xml`: a scripted dialogue between named actors.

    Actor names are the names of :data:`GROUP_ALIAS` groups in `map.obj.xml`, which is how
    a line is attached to the object that speaks it.
    """

    name: str
    startup: str
    startup_phrases: tuple[str, ...]
    restore_view: bool
    actors: tuple[str, ...]
    phrases: tuple[Phrase, ...]

    @classmethod
    def parse(cls, data: bytes, source: str = "cnv.conv.xml") -> Conversation:
        root = _parse(data, source, "conversation")
        return cls(
            name=root.get("name", ""),
            startup=root.get("startup", ""),
            startup_phrases=tuple(
                p for p in root.get("startup_phrases", "").split(";") if p
            ),
            restore_view=root.get("restore_view", "0") != "0",
            actors=tuple(e.get("name", "") for e in root.findall("actor")),
            phrases=tuple(
                Phrase(
                    actor=e.get("actor", ""),
                    text=e.get("text", ""),
                    followup=e.get("followup", ""),
                    label=e.get("label"),
                    followup_phrases=tuple(
                        p for p in e.get("followup_phrases", "").split(";") if p
                    ),
                    choice_text=e.get("choice_text"),
                    condition=e.get("condition"),
                    action=e.get("action"),
                    ret=e.get("return"),
                    comments=e.get("comments"),
                )
                for e in root.findall("phrase")
            ),
        )


@dataclass(frozen=True)
class CustomItem:
    """One `<item>` of `itemsCustom.xml`: a scenario-local inventory item.

    Objects refer to these by `id` through their `slot<n>` attributes.
    """

    id: str
    name: str
    image: str
    sound: str
    level: int
    description: str
    use_count: int
    important: bool
    cursed: bool
    bonus: dict[str, int]
    scripts: dict[str, str]  # inline .vs source, keyed on the attribute name

    @staticmethod
    def parse_all(data: bytes, source: str = "itemsCustom.xml") -> list[CustomItem]:
        root = _parse(data, source, "items")
        items = []
        for e in root.findall("item"):
            bonus = e.find("bonus")
            items.append(
                CustomItem(
                    id=e.get("id", ""),
                    name=e.get("name", e.get("id", "")),
                    image=e.get("image", ""),
                    sound=e.get("sound", ""),
                    level=_int(e, "level"),
                    description=e.get("description", ""),
                    use_count=_int(e, "usecount"),
                    important=e.get("important", "no") == "yes",
                    cursed=e.get("cursed", "0") != "0",
                    bonus={k: int(v) for k, v in (bonus.attrib if bonus is not None else {}).items()},
                    scripts={
                        k: v for k, v in e.attrib.items() if k.endswith("_script") and v
                    },
                )
            )
        return items


@dataclass(frozen=True)
class Territory:
    """One `<territory>` of `territories.xml`: a region of a conquest's campaign map."""

    id: str
    index: int
    state: int
    visual_name: str
    map_name: str
    description: str
    bonus: str
    bonus_description: str
    neighbours: tuple[str, ...]
    #: The territory's race, as the race enumeration numbers it (Gaul 0 ...
    #: Germany 7); -1 when absent or outside the eight races, which is the
    #: range 0x00506560 keeps. See ``adventure.md``.
    interface: int


def _race_index(element: ET.Element) -> int:
    value = _int(element, "interface", -1)
    return value if 0 <= value < 8 else -1


@dataclass(frozen=True)
class ConquestMap:
    """`territories.xml`: the campaign map a conquest is played on."""

    name: str
    data: str
    choose: bool
    conquered_order: str
    territories: tuple[Territory, ...]
    display: dict[str, int]  # the colorize / hue / sat knobs
    interface: int

    @classmethod
    def parse(cls, data: bytes, source: str = "territories.xml") -> ConquestMap:
        root = _parse(data, source, "conquestmap")
        return cls(
            name=root.get("name", ""),
            data=root.get("data", ""),
            choose=root.get("choose", "0") != "0",
            conquered_order=root.get("ConqueredOrder", ""),
            interface=_int(root, "interface", -1),
            display={
                k: _int(root, k)
                for k in root.attrib
                if k.endswith(("_colorize", "_hue", "_sat"))
            },
            territories=tuple(
                Territory(
                    id=e.get("id", ""),
                    index=_int(e, "index"),
                    state=_int(e, "state"),
                    visual_name=e.get("visualname", ""),
                    map_name=e.get("mapname", ""),
                    description=e.get("description", ""),
                    bonus=e.get("bonus", ""),
                    bonus_description=e.get("bonus_descr", ""),
                    neighbours=tuple(
                        n.strip() for n in e.get("neighbours", "").split(",") if n.strip()
                    ),
                    interface=_race_index(e),
                )
                for e in root.findall("territory")
            ),
        )

    def validate(self, sequence_names: set[str] | None = None) -> None:
        known = {t.id for t in self.territories}
        for territory in self.territories:
            for neighbour in territory.neighbours:
                if neighbour not in known:
                    raise MapError(
                        f"territories.xml: {territory.id} names neighbour {neighbour!r}, "
                        f"which is not a declared territory"
                    )
            # `bonus` is a trap of the same shape as `importsettlement`: it looks like a
            # class name and is not one. It names a `<sequence>` in the container-root
            # `Sequences/sequences.xml`.
            if sequence_names is not None and territory.bonus not in sequence_names:
                raise MapError(
                    f"territories.xml: {territory.id} claims bonus {territory.bonus!r}, "
                    f"which is not a container-root sequence"
                )


# ---------------------------------------------------------------------------
# one map
# ---------------------------------------------------------------------------


@dataclass
class MapDocument:
    """Everything under one `Maps/<n>/` directory."""

    number: int
    info: MapInfo
    objects: ObjectList
    terrain: TerrainLayers
    sequences: list[SequenceRef] = field(default_factory=list)
    conversations: list[Conversation] = field(default_factory=list)
    labels: list[Label] = field(default_factory=list)
    notes: list[Note] = field(default_factory=list)

    @property
    def size(self) -> tuple[int, int]:
        return self.info.size

    def unbound_actors(self) -> set[str]:
        """Conversation actor names that no object group provides.

        Not a fault: an actor can be bound at runtime by a script, and the retail data
        relies on that. Reported rather than raised.
        """
        names = {g.name for g in self.objects.groups}
        return {
            a for c in self.conversations for a in c.actors if a and a not in names
        }

    def validate(self) -> None:
        self.info.validate()
        self.objects.validate(self.info.size)
        self.terrain.validate(self.info.size)


# ---------------------------------------------------------------------------
# the whole container
# ---------------------------------------------------------------------------


class Scenario:
    """The documents inside one scenario, adventure or conquest container.

    ``maps`` is keyed on the directory number under `Maps/`, which is not necessarily
    contiguous: a conquest's maps are numbered after the territories they belong to.
    """

    def __init__(self, container: BlockFile, path: str | Path | None = None) -> None:
        self.container = container
        self.path = Path(path) if path is not None else container.path
        self._names = {e.name.upper(): e.name for e in container.files()}

        self.game: GameInfo | None = None
        if self._has("game.xml"):
            self.game = GameInfo.parse(self._read("game.xml"), "game.xml")

        self.players: list[Player] = []
        for slot in range(PLAYER_SLOTS):
            name = f"player{slot}.xml"
            if self._has(name):
                self.players.append(Player.parse(self._read(name), name))
        # `randommap.BFHP` additionally bundles all sixteen records in one document.
        self.player_template: list[Player] = []
        if self._has("players.xml"):
            root = _parse(self._read("players.xml"), "players.xml", "playersdatasection")
            self.player_template = [
                Player.from_element(e, "players.xml") for e in root.findall("playerdata")
            ]

        self.items: list[CustomItem] = []
        if self._has("itemsCustom.xml"):
            self.items = CustomItem.parse_all(self._read("itemsCustom.xml"), "itemsCustom.xml")

        self.notes: list[Note] = []
        if self._has("Notes.xml"):
            self.notes = Note.parse_all(self._read("Notes.xml"), "Notes.xml")

        self.sequences: list[SequenceRef] = []
        if self._has("Sequences/sequences.xml"):
            self.sequences = SequenceRef.parse_all(
                self._read("Sequences/sequences.xml"), "Sequences/sequences.xml"
            )

        self.conquest: ConquestMap | None = None
        if self._has("territories.xml"):
            self.conquest = ConquestMap.parse(self._read("territories.xml"), "territories.xml")

        self.languages: list[str] = sorted(
            {n.split("/")[1] for n in self._names.values() if n.startswith("Local/")}
        )

        self.maps: dict[int, MapDocument] = {}
        for number in self._map_numbers():
            self.maps[number] = self._load_map(number)

    # -- construction ----------------------------------------------------

    @classmethod
    def open(cls, path: str | Path) -> Scenario:
        """Open a container, unwrapping an LZIS-compressed one if necessary."""
        path = Path(path)
        data = path.read_bytes()
        if data[:4] != b"HPFS":
            data = decompress(data)
        return cls(BlockFile.from_bytes(data, str(path)), path)

    # -- container access ------------------------------------------------

    def _has(self, name: str) -> bool:
        return name.upper() in self._names

    def _read(self, name: str) -> bytes:
        return self.container.read(self._names[name.upper()])

    def _map_numbers(self) -> list[int]:
        numbers = set()
        for name in self._names.values():
            parts = name.split("/")
            if len(parts) > 2 and parts[0].lower() == "maps" and parts[1].isdigit():
                numbers.add(int(parts[1]))
        return sorted(numbers)

    def _load_map(self, number: int) -> MapDocument:
        base = f"Maps/{number}"
        info = MapInfo.parse(self._read(f"{base}/map.xml"), f"{base}/map.xml")
        objects = ObjectList.parse(
            self._read(f"{base}/map.obj.xml"), f"{base}/map.obj.xml"
        )
        layers = {}
        for layer in LAYER_NAMES:
            name = f"{base}/Terrain.{layer}.grid"
            layers[layer] = parse_grid(self._read(name), name)

        sequences: list[SequenceRef] = []
        if self._has(f"{base}/Sequences/sequences.xml"):
            sequences = SequenceRef.parse_all(
                self._read(f"{base}/Sequences/sequences.xml"),
                f"{base}/Sequences/sequences.xml",
            )
        labels: list[Label] = []
        if self._has(f"{base}/labels.xml"):
            labels = Label.parse_all(self._read(f"{base}/labels.xml"), f"{base}/labels.xml")
        notes: list[Note] = []
        if self._has(f"{base}/Notes.xml"):
            notes = Note.parse_all(self._read(f"{base}/Notes.xml"), f"{base}/Notes.xml")

        prefix = f"{base}/Conversations/".upper()
        conversations = [
            Conversation.parse(self.container.read(name), name)
            for key, name in sorted(self._names.items())
            if key.startswith(prefix) and key.endswith(".CONV.XML")
        ]

        return MapDocument(
            number=number,
            info=info,
            objects=objects,
            terrain=TerrainLayers(layers),
            sequences=sequences,
            conversations=conversations,
            labels=labels,
            notes=notes,
        )

    def script_paths(self) -> list[str]:
        """Every `.vs` trigger script stored in the container, in directory order."""
        return [n for n in self._names.values() if n.lower().endswith(".vs")]

    def class_names(self) -> set[str]:
        """Every class name any map in this container refers to."""
        names: set[str] = set()
        for document in self.maps.values():
            names |= document.objects.class_names()
        return names

    # -- validation ------------------------------------------------------

    def validate(self) -> None:
        """Assert the invariants described in the specification, for every map."""
        if self.players and len(self.players) != PLAYER_SLOTS:
            raise MapError(
                f"{self.path.name}: {len(self.players)} player documents, "
                f"expected {PLAYER_SLOTS}"
            )
        for slot, player in enumerate(self.players):
            player.validate(slot)
        for player in self.player_template:
            player.validate()
        if self.conquest is not None:
            self.conquest.validate({s.name for s in self.sequences})
        if self.game is not None and self.maps and self.game.start_map not in self.maps:
            raise MapError(
                f"{self.path.name}: game.xml starts on map {self.game.start_map}, "
                f"which is not in the container"
            )
        for sequence in self.sequences:
            if not self._has(sequence.resolve()):
                raise MapError(
                    f"{self.path.name}: sequence {sequence.name!r} names "
                    f"{sequence.script!r}, which is not in the container"
                )
        for number, document in self.maps.items():
            document.validate()
            for sequence in document.sequences:
                if not self._has(sequence.resolve(f"Maps/{number}")):
                    raise MapError(
                        f"{self.path.name}: Maps/{number} sequence {sequence.name!r} "
                        f"names {sequence.script!r}, which is not in the container"
                    )

    def __repr__(self) -> str:
        name = self.game.name if self.game else "?"
        return f"<Scenario {name!r} maps={sorted(self.maps)} players={len(self.players)}>"


def main() -> None:
    import argparse
    import collections

    parser = argparse.ArgumentParser(
        description="Inspect the contents of an Imperivm scenario container."
    )
    parser.add_argument("container", type=Path)
    parser.add_argument("--map", type=int, help="restrict output to one map number")
    parser.add_argument("--objects", action="store_true", help="list every placed object")
    parser.add_argument("--groups", action="store_true", help="list the object groups")
    parser.add_argument("--grid", choices=LAYER_NAMES, help="report one terrain layer")
    args = parser.parse_args()

    scenario = Scenario.open(args.container)
    scenario.validate()

    numbers = [args.map] if args.map is not None else sorted(scenario.maps)
    if args.map is not None and args.map not in scenario.maps:
        raise SystemExit(f"no map {args.map} in {args.container}")

    if scenario.game is not None:
        game = scenario.game
        kind = {0: "scenario", 1: "adventure", 2: "conquest"}.get(game.game_type, "?")
        print(f"{args.container}: {game.name!r} by {game.author!r} ({kind})")
        print(
            f"  season {game.season or 'spring (default)'}, "
            f"starts on map {game.start_map}, victory {game.victory_condition!r}"
        )
    else:
        print(f"{args.container}: no game.xml")

    active = [p for p in scenario.players if p.is_active]
    print(f"  {len(scenario.players)} player slots, {len(active)} active")
    for player in active:
        r, g, b = player.rgb
        allies = player.allies()
        print(
            f"    {player.id:>2}  {player.name:<12} {player.race:<14} {player.control:<9}"
            f" #{r:02X}{g:02X}{b:02X}  start ({player.start_x}, {player.start_y})"
            f"{'  allies ' + str(allies) if allies else ''}"
        )
    if scenario.conquest is not None:
        print(f"  conquest map {scenario.conquest.name!r}:")
        for territory in scenario.conquest.territories:
            print(
                f"    {territory.id:<10} map {territory.map_name:<10} "
                f"neighbours {', '.join(territory.neighbours)}"
            )
    if scenario.items:
        print(f"  {len(scenario.items)} custom items: "
              f"{', '.join(i.id for i in scenario.items)}")
    if scenario.languages:
        print(f"  localised into: {', '.join(scenario.languages)}")

    for number in numbers:
        document = scenario.maps[number]
        info = document.info
        objects = document.objects
        print(
            f"\n  Maps/{number}: {info.name!r} {info.size_x}x{info.size_y} world units"
            f"{' (no fog)' if info.no_fog else ''}"
        )
        census = collections.Counter(o.class_name for o in objects.objects)
        units = sum(1 for o in objects.objects if o.is_unit)
        heroes = sum(1 for o in objects.objects if o.is_hero)
        in_town = sum(1 for o in objects.objects if o.in_settlement)
        print(
            f"    {len(objects.objects)} objects of {len(census)} classes: "
            f"{units} units ({heroes} heroes), {in_town} settlement buildings, "
            f"{len(objects.areas())} areas"
        )
        print(
            f"    {len(objects.settlements)} settlements, {len(objects.groups)} groups, "
            f"{len(document.sequences)} sequences, "
            f"{len(document.conversations)} conversations, {len(document.labels)} labels"
        )
        for name in LAYER_NAMES:
            grid = document.terrain[name]
            print(
                f"    Terrain.{name:<8} {grid.width:>5}x{grid.height:<5} cells of "
                f"{grid.cell_size:>3} units, {grid.bits_per_cell:>2} bit"
            )

        if args.grid:
            grid = document.terrain[args.grid]
            histogram = grid.histogram()
            total = sum(histogram.values())
            print(f"    {args.grid}: {len(histogram)} distinct values over {total} cells")
            for value, count in sorted(histogram.items(), key=lambda kv: -kv[1])[:12]:
                print(f"      {value:>6} {count:>10}  {100 * count / total:5.1f}%")

        if args.groups:
            for group in objects.groups:
                kind = "alias" if group.is_alias else "list "
                print(f"    {kind} {group.name!r}: {len(group.members)} members")

        if args.objects:
            for obj in objects.objects:
                owner = f"p{obj.player}" if obj.player is not None else "--"
                extra = f" area={obj.area.shape}" if obj.area else ""
                name = f" {obj.display_name!r}" if obj.display_name else ""
                print(
                    f"    {obj.num:>5} {obj.class_name:<22} {owner:<4} "
                    f"({obj.x:>6}, {obj.y:>6}) flags=0x{obj.flags:08X}{extra}{name}"
                )


if __name__ == "__main__":
    main()
