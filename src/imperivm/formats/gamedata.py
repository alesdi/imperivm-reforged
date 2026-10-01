"""Loader for the Imperivm object model: classes (`.SC.XML`) and entities (`.ENT.XML`).

Specifications: docs/formats/sc-xml.md, docs/formats/ent-xml.md
Resolved inventory and integrity report: docs/data-model.md

Reads the class graph and entity definitions straight out of the retail packs, resolves
inheritance, and validates every cross-reference against the pack index.

Reference implementation: correctness and legibility over speed. Parsing the whole corpus
(845 classes, 889 entities) takes a couple of seconds and is not incremental.
"""

from __future__ import annotations

import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

from .pak import PackFile

# Packs that hold anything the object model refers to, in load order.
PACK_NAMES = (
    "data",
    "Units",
    "Buildings",
    "MapObjects",
    "Visuals",
    "UI",
    "Sounds",
    "Terrain",
    "Outlines",
    "Minimap",
    "AdditionalArt",
    "Fonts",
)

CLASS_DIR = "DATA\\CLASSES"
SOUND_ENTITY_DIR = "DATA\\SOUND ENTITIES"

#: Virtual resource root. `gameres/icons/x.bmp` is the pack entry `UI\ICONS\X.BMP`.
VIRTUAL_ROOTS = {"GAMERES\\": "UI\\"}

#: Seasons selectable through `entity_<season>`; `entity` is the fallback.
SEASONS = ("spring", "autumn", "winter")

#: `<state anim_idx>` / `<state anim_frame>` value meaning "no animation" (0x10000).
NO_ANIM = 65536

#: 0xCDCDCDCD as a signed 32-bit int: the MSVC uninitialised-heap fill pattern, which
#: leaked into seven `<state>` offsets in the retail data.
UNINITIALISED = -842150451


class GameDataError(Exception):
    """Raised when the class graph is structurally impossible to load."""


# ---------------------------------------------------------------------------
# resource index
# ---------------------------------------------------------------------------


class ResourceIndex:
    """Case- and separator-insensitive view over several packs.

    Handles the two resolution quirks the object model relies on: the ``gameres/``
    virtual root, and references that omit a file extension.
    """

    def __init__(self, packs_dir: str | Path, names: tuple[str, ...] = PACK_NAMES) -> None:
        self.packs_dir = Path(packs_dir)
        self.packs: dict[str, PackFile] = {}
        self._index: dict[str, str] = {}
        for name in names:
            path = self.packs_dir / f"{name}.pak"
            if not path.exists():
                continue
            pack = PackFile(path)
            self.packs[name] = pack
            for entry in pack.entries:
                self._index.setdefault(self.normalise(entry.name), name)

    @staticmethod
    def normalise(path: str) -> str:
        """Fold a reference to the form used as an index key."""
        key = path.strip().upper().replace("/", "\\")
        for prefix, replacement in VIRTUAL_ROOTS.items():
            if key.startswith(prefix):
                key = replacement + key[len(prefix) :]
        return key

    def __contains__(self, path: str) -> bool:
        return self.normalise(path) in self._index

    def __len__(self) -> int:
        return len(self._index)

    def resolve(self, *candidates: str) -> str | None:
        """Return the first candidate that exists, as a normalised pack path."""
        for candidate in candidates:
            key = self.normalise(candidate)
            if key in self._index:
                return key
        return None

    def read(self, path: str) -> bytes:
        key = self.normalise(path)
        try:
            pack = self.packs[self._index[key]]
        except KeyError:
            raise KeyError(f"{path!r} is not in any indexed pack") from None
        return pack.read(key)

    def text(self, path: str) -> str:
        return self.read(path).decode("cp1252")

    def glob(self, prefix: str, suffix: str) -> list[str]:
        """Every entry under `prefix` ending in `suffix`, sorted."""
        pre, suf = self.normalise(prefix), suffix.upper()
        return sorted(k for k in self._index if k.startswith(pre) and k.endswith(suf))


# ---------------------------------------------------------------------------
# class graph
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Method:
    """One `<method>`: a script bound to a name on a class."""

    sig: str
    vs: str
    verify: str | None = None
    onfinish: str | None = None


@dataclass(frozen=True)
class DefaultCmd:
    """One `<defaultcmd>`: the ordered commands offered against a target class."""

    target: str
    cmds: tuple[tuple[str, bool], ...]  # (command name, requires ctrl)


@dataclass(frozen=True)
class InfoValue:
    """One `<value0>`..`<value5>` info-bar slot."""

    slot: int
    icon: str | None = None
    script: str | None = None  # inline .vs source, not a path
    rollover: str | None = None
    help: str | None = None
    flags: str | None = None  # -1, 0 or 5; meaning unknown


@dataclass
class GameClass:
    """One `<class>` element, with its own (unresolved) contributions only."""

    id: str
    cpp_class: str
    parent: str | None
    source: str
    altid: str | None = None
    is_winter_variant: bool = False
    entities: dict[str, str] = field(default_factory=dict)  # "" | season -> path
    properties: dict[str, str] = field(default_factory=dict)
    methods: dict[str, Method] = field(default_factory=dict)
    behaviors: tuple[str, ...] = ()
    sounds: dict[str, str] = field(default_factory=dict)
    default_cmds: tuple[DefaultCmd, ...] = ()
    no_defcmd_inherit: bool = False
    values: dict[int, InfoValue] = field(default_factory=dict)

    @classmethod
    def parse(cls, xml: str, source: str) -> GameClass:
        root = ET.fromstring(xml)
        if root.tag != "class":
            raise GameDataError(f"{source}: root element is <{root.tag}>, expected <class>")
        cid = root.get("id")
        cpp = root.get("cpp_class")
        if not cid or not cpp:
            raise GameDataError(f"{source}: <class> needs both id and cpp_class")

        entities = {"": root.get("entity", "")}
        for season in SEASONS:
            value = root.get(f"entity_{season}")
            if value is not None:
                entities[season] = value

        obj = cls(
            id=cid,
            cpp_class=cpp,
            parent=(root.get("parent") or None),
            source=source,
            altid=root.get("altid"),
            is_winter_variant="\\WINTER\\" in source.upper(),
            entities={k: v for k, v in entities.items() if v},
        )

        # <properties> and <sounds> repeat and merge attribute-wise, last wins.
        for element in root.findall("properties"):
            obj.properties.update(element.attrib)
        for element in root.findall("sounds"):
            obj.sounds.update(element.attrib)

        # <method> is keyed on sig; duplicates within one file exist (14 of them) and
        # last wins here. See docs/formats/sc-xml.md for why that is not certain.
        for element in root.findall("method"):
            sig, vs = element.get("sig"), element.get("vs")
            if not sig or not vs:
                raise GameDataError(f"{source}: <method> needs both sig and vs")
            obj.methods[sig] = Method(sig, vs, element.get("verify"), element.get("onfinish"))

        obj.behaviors = tuple(
            e.get("script", "") for e in root.findall("behavior") if e.get("script")
        )
        obj.no_defcmd_inherit = root.find("nodefcmdinherit") is not None
        obj.default_cmds = tuple(
            DefaultCmd(
                target=e.get("target", ""),
                cmds=tuple(
                    (c.get("name", ""), c.get("ctrl") == "1") for c in e.findall("cmd")
                ),
            )
            for e in root.findall("defaultcmd")
        )
        for slot in range(6):
            element = root.find(f"value{slot}")
            if element is not None:
                obj.values[slot] = InfoValue(
                    slot,
                    element.get("icon"),
                    element.get("script"),
                    element.get("rollover"),
                    element.get("help"),
                    element.get("flags"),
                )
        return obj


# ---------------------------------------------------------------------------
# entities
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Image:
    """One `<image>`: a `rows` x `columns` sprite grid. Column = variation, row = frame.

    ``rows``, ``columns`` and ``drawmode`` are NOT authoritative -- 430 of the 4,033 resolvable
    images contradict their `.rle.mmp` frame table on the grid, and hundreds more on the draw
    mode. Read the geometry from the frame table; keep these only as a cross-check.
    """

    idx: int
    file: str
    drawmode: str  # player_color | shadow | normal | index | clouds
    remaping: str  # none | reverse | pingpong -- row playback order, NOT palette remapping
    rows: int
    columns: int


@dataclass(frozen=True)
class Point:
    """One `<point>`: a typed attachment marker. Type semantics are unknown."""

    idx: int
    type: int
    x: int
    y: int


@dataclass(frozen=True)
class Layer:
    """One `<layer>`: an image drawn at a z depth, see DATA\\ZBINS.XML for the sort bins."""

    idx: int
    name: str
    image: int
    z: int
    offsetx: int
    offsety: int
    sortoffsetx: int = 0
    sortoffsety: int = 0
    xray: int = 0
    nohighlight: int = 0
    percent: int | None = None


@dataclass(frozen=True)
class State:
    """One `<state>`: a resting pose, optionally looping an animation slot."""

    idx: int
    name: str
    image_idx: int = 0
    image_row: int = 0
    offsetx: int = 0
    offsety: int = 0
    anim_idx: int = NO_ANIM
    anim_frame: int = NO_ANIM
    anim_row: int | None = None

    @property
    def has_anim(self) -> bool:
        return self.anim_idx != NO_ANIM

    @property
    def offsets_are_garbage(self) -> bool:
        """True for the seven states holding 0xCDCDCDCD instead of a real offset."""
        return self.offsetx == UNINITIALISED or self.offsety == UNINITIALISED


@dataclass(frozen=True)
class Replace:
    """One `<replace>`: re-point a layer at another image for an animation's duration."""

    layer: int
    image: int
    offsetx: int
    offsety: int


@dataclass(frozen=True)
class Anim:
    """One `<anim>`: a state-machine edge, addressed by numeric slot `idx`."""

    idx: int
    name: str
    startstate: int
    endstate: int
    frames: int
    duration: int
    default_duration: int
    action_time: int
    step: int
    replaces: tuple[Replace, ...]
    frame_durations: tuple[int, ...]
    floating: bool = False

    @property
    def measured_duration(self) -> int:
        """Sum of the frame durations. Disagrees with `duration` in 127 retail anims."""
        return sum(self.frame_durations)

    @property
    def sprite_rows(self) -> int:
        """Real sprite rows, i.e. frames minus the two zero-duration bookends."""
        return max(0, self.frames - 2)


@dataclass
class Entity:
    """One `<entity>` document: the art contract for a class.

    Carries no footprint geometry -- ``pass_file`` is a link to an irregular bitmap mask and is
    the only obstruction information an entity has.
    """

    path: str  # normalised pack path
    name: str
    type: str
    variations: int
    pass_file: str
    radius: int | None = None
    selection_radius: int | None = None
    floating_turnspeed: int | None = None
    images: dict[int, Image] = field(default_factory=dict)
    points: tuple[Point, ...] = ()
    layers: dict[int, Layer] = field(default_factory=dict)
    states: dict[int, State] = field(default_factory=dict)
    anims: dict[int, Anim] = field(default_factory=dict)

    @property
    def directory(self) -> str:
        """Pack directory holding this entity; image and pass references are relative to it."""
        return self.path.rsplit("\\", 1)[0]

    @classmethod
    def parse(cls, xml: str, path: str) -> Entity:
        root = ET.fromstring(xml)
        if root.tag != "entity":
            raise GameDataError(f"{path}: root element is <{root.tag}>, expected <entity>")

        def num(element: ET.Element, key: str, default: int) -> int:
            """Integer attribute, tolerating a missing or empty value. 0 is a real value."""
            raw = element.get(key)
            return default if raw is None or raw == "" else int(raw)

        def opt(element: ET.Element, key: str) -> int | None:
            raw = element.get(key)
            return None if raw is None or raw == "" else int(raw)

        obj = cls(
            path=path,
            name=root.get("name", ""),
            type=root.get("type", ""),
            variations=num(root, "variations", 1),
            pass_file=root.get("pass_file", ""),
            radius=opt(root, "radius"),
            selection_radius=opt(root, "selection_radius"),
            floating_turnspeed=opt(root, "floating_turnspeed"),
        )

        for e in root.iter("image"):
            image = Image(
                idx=num(e, "idx", 0),
                file=e.get("file", ""),
                drawmode=e.get("drawmode", ""),
                remaping=e.get("remaping", "none"),
                rows=num(e, "rows", 1),
                columns=num(e, "columns", 1),
            )
            obj.images[image.idx] = image

        obj.points = tuple(
            Point(num(e, "idx", 0), num(e, "type", 0), num(e, "x", 0), num(e, "y", 0))
            for e in root.iter("point")
        )

        for e in root.iter("layer"):
            layer = Layer(
                idx=num(e, "idx", 0),
                name=e.get("name", ""),
                image=num(e, "image", 0),
                z=num(e, "z", 0),
                offsetx=num(e, "offsetx", 0),
                offsety=num(e, "offsety", 0),
                sortoffsetx=num(e, "sortoffsetx", 0),
                sortoffsety=num(e, "sortoffsety", 0),
                xray=num(e, "xray", 0),
                nohighlight=num(e, "nohighlight", 0),
                percent=opt(e, "percent"),
            )
            obj.layers[layer.idx] = layer

        for e in root.iter("state"):
            state = State(
                idx=num(e, "idx", 0),
                name=e.get("name", ""),
                image_idx=num(e, "image_idx", 0),
                image_row=num(e, "image_row", 0),
                offsetx=num(e, "offsetx", 0),
                offsety=num(e, "offsety", 0),
                anim_idx=num(e, "anim_idx", NO_ANIM),
                anim_frame=num(e, "anim_frame", NO_ANIM),
                anim_row=opt(e, "anim_row"),
            )
            obj.states[state.idx] = state

        for e in root.iter("anim"):
            anim = Anim(
                idx=num(e, "idx", 0),
                name=e.get("name", ""),
                startstate=num(e, "startstate", 0),
                endstate=num(e, "endstate", 0),
                frames=num(e, "frames", 0),
                duration=num(e, "duration", 0),
                default_duration=num(e, "default_duration", 0),
                action_time=num(e, "action_time", 0),
                step=num(e, "step", 0),
                replaces=tuple(
                    Replace(num(r, "layer", 0), num(r, "image", 0),
                            num(r, "offsetx", 0), num(r, "offsety", 0))
                    for r in e.iter("replace")
                ),
                frame_durations=tuple(num(f, "duration", 0) for f in e.iter("frame")),
                floating=e.get("floating") == "1",
            )
            obj.anims[anim.idx] = anim

        return obj


# ---------------------------------------------------------------------------
# validation
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Issue:
    """One validation finding."""

    kind: str
    source: str
    detail: str

    def __str__(self) -> str:
        return f"{self.kind}: {self.source}: {self.detail}"


# ---------------------------------------------------------------------------
# the model
# ---------------------------------------------------------------------------


class GameData:
    """The whole object model: 845 classes and 889 entities, resolved.

    ``classes`` is keyed on ``id``. ``lookup`` also accepts an ``altid``, which is how
    class-name properties such as ``importsettlement`` refer to classes; ``id`` wins on a
    collision (one exists: ``Inn``).
    """

    def __init__(self, packs_dir: str | Path) -> None:
        self.resources = ResourceIndex(packs_dir)
        self.classes: dict[str, GameClass] = {}
        self.entities: dict[str, Entity] = {}
        self._by_altid: dict[str, str] = {}

        for path in self.resources.glob(CLASS_DIR, ".SC.XML"):
            game_class = GameClass.parse(self.resources.text(path), path)
            if game_class.id in self.classes:
                raise GameDataError(
                    f"{path}: duplicate class id {game_class.id!r} "
                    f"(also in {self.classes[game_class.id].source})"
                )
            self.classes[game_class.id] = game_class
            if game_class.altid:
                self._by_altid.setdefault(game_class.altid, game_class.id)

        for path in self.resources.glob("", ".ENT.XML"):
            self.entities[path] = Entity.parse(self.resources.text(path), path)

    # -- graph -----------------------------------------------------------

    def lookup(self, name: str) -> GameClass | None:
        """Resolve a class reference by `id`, falling back to `altid`."""
        if name in self.classes:
            return self.classes[name]
        alias = self._by_altid.get(name)
        return self.classes[alias] if alias else None

    def ancestry(self, class_id: str) -> list[GameClass]:
        """The class and its ancestors, nearest first. Raises on a cycle."""
        chain: list[GameClass] = []
        seen: set[str] = set()
        current: str | None = class_id
        while current is not None:
            if current in seen:
                raise GameDataError(f"inheritance cycle through {current!r}")
            game_class = self.classes.get(current)
            if game_class is None:
                break
            seen.add(current)
            chain.append(game_class)
            current = game_class.parent
        return chain

    def roots(self) -> list[str]:
        return sorted(
            c.id for c in self.classes.values()
            if c.parent is None or c.parent not in self.classes
        )

    def children(self, class_id: str) -> list[str]:
        return sorted(c.id for c in self.classes.values() if c.parent == class_id)

    def depth(self, class_id: str) -> int:
        return len(self.ancestry(class_id))

    def subtree_size(self, class_id: str) -> int:
        return 1 + sum(self.subtree_size(k) for k in self.children(class_id))

    # -- resolution ------------------------------------------------------

    def resolve_properties(self, class_id: str) -> dict[str, str]:
        """Merged property bag: ancestors first, the class itself last."""
        resolved: dict[str, str] = {}
        for game_class in reversed(self.ancestry(class_id)):
            resolved.update(game_class.properties)
        return resolved

    def resolve_methods(self, class_id: str) -> dict[str, Method]:
        """Merged method table, keyed on `sig`."""
        resolved: dict[str, Method] = {}
        for game_class in reversed(self.ancestry(class_id)):
            resolved.update(game_class.methods)
        return resolved

    def resolve_sounds(self, class_id: str) -> dict[str, str]:
        resolved: dict[str, str] = {}
        for game_class in reversed(self.ancestry(class_id)):
            resolved.update(game_class.sounds)
        return resolved

    def resolve_behaviors(self, class_id: str) -> tuple[str, ...]:
        """The class's own behaviours, then its parent's list, duplicates kept, at most eight.

        What gbr.exe's class loader builds; see docs/formats/sc-xml.md.
        """
        scripts: list[str] = []
        for game_class in self.ancestry(class_id):
            scripts.extend(game_class.behaviors)
        return tuple(scripts[:8])

    def resolve_default_cmds(self, class_id: str) -> tuple[DefaultCmd, ...]:
        """Default-command blocks accumulate unless a class carries <nodefcmdinherit/>."""
        blocks: dict[str, DefaultCmd] = {}
        for game_class in reversed(self.ancestry(class_id)):
            if game_class.no_defcmd_inherit:
                blocks.clear()
            for block in game_class.default_cmds:
                blocks[block.target] = block
        return tuple(blocks.values())

    def resolve_values(self, class_id: str) -> dict[int, InfoValue]:
        resolved: dict[int, InfoValue] = {}
        for game_class in reversed(self.ancestry(class_id)):
            resolved.update(game_class.values)
        return resolved

    def entity_path(self, class_id: str, season: str = "") -> str | None:
        """The entity a class uses in a season: `entity_<season>`, else `entity`."""
        game_class = self.classes.get(class_id)
        if game_class is None:
            return None
        return game_class.entities.get(season) or game_class.entities.get("") or None

    def entity_for(self, class_id: str, season: str = "") -> Entity | None:
        path = self.entity_path(class_id, season)
        if path is None:
            return None
        return self.entities.get(self.resources.normalise(path))

    # -- reference resolution --------------------------------------------

    def resolve_image(self, entity: Entity, image: Image) -> str | None:
        """`.rle` names are directory-relative and often omit the extension."""
        stem = f"{entity.directory}\\{image.file}"
        return self.resources.resolve(stem, f"{stem}.MMP", f"{stem}.RLE.MMP")

    def resolve_pass_file(self, entity: Entity) -> str | None:
        """Passability masks are directory-relative; 51 are stored as a bare `PASS`.

        Name-level resolution only. The engine matches masks by content, so a miss here is not
        proof that the entity has no mask at runtime.
        """
        if not entity.pass_file:
            return None
        stem = f"{entity.directory}\\{entity.pass_file}"
        return self.resources.resolve(stem, f"{stem}.PASS")

    def resolve_sound(self, value: str) -> str | None:
        """A `<sounds>` value is a path, or a bare name under DATA\\SOUND ENTITIES."""
        if not value:
            return None
        if "/" in value or "\\" in value:
            return self.resources.resolve(value)
        return self.resources.resolve(f"{SOUND_ENTITY_DIR}\\{value}.XML")

    # -- validation ------------------------------------------------------

    def validate(self) -> list[Issue]:
        """Check every cross-reference. Structural faults raise; dangling refs are returned.

        The retail install yields 38 issues, all dangling resource references, enumerated
        in docs/data-model.md. Anything else means the install differs from the reference.
        """
        issues: list[Issue] = []

        # Structural: parents must resolve and the graph must be acyclic.
        for game_class in self.classes.values():
            if game_class.parent and game_class.parent not in self.classes:
                raise GameDataError(
                    f"{game_class.source}: parent {game_class.parent!r} is not a declared class"
                )
            self.ancestry(game_class.id)  # raises on a cycle

        # Properties whose value is a class name. `importsettlement` is deliberately absent:
        # its eight values name settlement templates, not classes (see docs/data-model.md).
        CLASS_REF_PROPERTIES = (
            "projectile_class",
            "projectile_explosion",
            "projectile_shadow",
            "projectile_fire",
            "building_projectile_class",
            "sentry_class_name",
            "select_class",
            "defender_cls_1",
            "defender_cls_2",
        )

        for game_class in self.classes.values():
            for season, path in game_class.entities.items():
                if path not in self.resources:
                    label = f"entity_{season}" if season else "entity"
                    issues.append(Issue("entity", game_class.source, f"{label}={path}"))

            for method in game_class.methods.values():
                for label, path in (
                    ("vs", method.vs),
                    ("verify", method.verify),
                    ("onfinish", method.onfinish),
                ):
                    if path and path not in self.resources:
                        issues.append(
                            Issue("script", game_class.source, f"{method.sig}/{label}={path}")
                        )
            for script in game_class.behaviors:
                if script not in self.resources:
                    issues.append(Issue("script", game_class.source, f"behavior={script}"))

            for channel, value in game_class.sounds.items():
                if value and self.resolve_sound(value) is None:
                    issues.append(Issue("sound", game_class.source, f"{channel}={value}"))

            icons = [game_class.properties.get("icon")]
            icons += [v.icon for v in game_class.values.values()]
            for icon in icons:
                if icon and icon not in self.resources:
                    issues.append(Issue("icon", game_class.source, f"icon={icon}"))

            for key in CLASS_REF_PROPERTIES:
                value = game_class.properties.get(key)
                if value and self.lookup(value) is None:
                    issues.append(Issue("class-ref", game_class.source, f"{key}={value}"))

        for entity in self.entities.values():
            for image in entity.images.values():
                if self.resolve_image(entity, image) is None:
                    issues.append(Issue("image", entity.path, f"file={image.file}"))
            if entity.pass_file and self.resolve_pass_file(entity) is None:
                issues.append(Issue("pass", entity.path, f"pass_file={entity.pass_file}"))

            for layer in entity.layers.values():
                if layer.image not in entity.images:
                    issues.append(
                        Issue("layer-image", entity.path, f"layer {layer.idx} -> image {layer.image}")
                    )
            for state in entity.states.values():
                if state.has_anim and state.anim_idx not in entity.anims:
                    issues.append(
                        Issue("state-anim", entity.path, f"state {state.idx} -> anim {state.anim_idx}")
                    )
            for anim in entity.anims.values():
                for label, idx in (("startstate", anim.startstate), ("endstate", anim.endstate)):
                    if idx not in entity.states:
                        issues.append(
                            Issue("anim-state", entity.path, f"anim {anim.idx} {label}={idx}")
                        )
                for replace in anim.replaces:
                    if replace.layer not in entity.layers:
                        issues.append(
                            Issue("replace-layer", entity.path, f"anim {anim.idx} layer {replace.layer}")
                        )
                    if replace.image not in entity.images:
                        issues.append(
                            Issue("replace-image", entity.path, f"anim {anim.idx} image {replace.image}")
                        )

        return issues

    # -- reporting -------------------------------------------------------

    def walk(self, class_id: str, depth: int = 0) -> Iterator[tuple[int, GameClass]]:
        yield depth, self.classes[class_id]
        for child in sorted(self.children(class_id), key=lambda c: -self.subtree_size(c)):
            yield from self.walk(child, depth + 1)


def main() -> None:
    import argparse
    import collections

    parser = argparse.ArgumentParser(description="Load and validate the Imperivm object model.")
    parser.add_argument("packs", type=Path, help="the game's Packs directory")
    parser.add_argument("--tree", action="store_true", help="print the full inheritance tree")
    parser.add_argument("--max-depth", type=int, default=3, help="tree depth to print (default 3)")
    parser.add_argument("--class", dest="class_id", help="dump one class, fully resolved")
    args = parser.parse_args()

    data = GameData(args.packs)
    print(
        f"{len(data.resources)} pack entries, "
        f"{len(data.classes)} classes, {len(data.entities)} entities"
    )

    if args.class_id:
        game_class = data.lookup(args.class_id)
        if game_class is None:
            raise SystemExit(f"no class {args.class_id!r}")
        chain = " <- ".join(c.id for c in data.ancestry(game_class.id))
        print(f"\n{game_class.id}  [{game_class.cpp_class}]  {game_class.source}")
        print(f"  ancestry: {chain}")
        for season, path in sorted(game_class.entities.items()):
            print(f"  entity[{season or 'default'}]: {path}")
        print("  properties:")
        for key, value in sorted(data.resolve_properties(game_class.id).items()):
            print(f"    {key} = {value}")
        methods = data.resolve_methods(game_class.id)
        print(f"  methods ({len(methods)}):")
        for sig in sorted(methods):
            method = methods[sig]
            extra = "".join(
                f" {label}={path}"
                for label, path in (("verify", method.verify), ("onfinish", method.onfinish))
                if path
            )
            print(f"    {sig} -> {method.vs}{extra}")
        for script in data.resolve_behaviors(game_class.id):
            print(f"  behavior: {script}")
        for block in data.resolve_default_cmds(game_class.id):
            names = ", ".join(n + ("+ctrl" if c else "") for n, c in block.cmds)
            print(f"  defaultcmd[{block.target or 'terrain'}]: {names}")
        return

    print("\n=== inheritance tree ===")
    for root in data.roots():
        for depth, game_class in data.walk(root):
            if depth > args.max_depth:
                continue
            size = data.subtree_size(game_class.id)
            if depth == args.max_depth and size == 1:
                continue
            print(f"{'  ' * depth}{game_class.id} [{game_class.cpp_class}] subtree={size}")

    print("\n=== cpp_class census ===")
    census = collections.Counter(c.cpp_class for c in data.classes.values())
    for name, count in census.most_common():
        print(f"  {count:4d}  {name}")

    print("\n=== race census (resolved) ===")
    races = collections.Counter(
        data.resolve_properties(cid).get("race", "-") for cid in data.classes
    )
    for name, count in races.most_common():
        print(f"  {count:4d}  {name}")

    print("\n=== integrity report ===")
    issues = data.validate()
    by_kind = collections.Counter(i.kind for i in issues)
    for kind, count in by_kind.most_common():
        print(f"  {count:4d}  {kind}")
    print(f"  {len(issues):4d}  TOTAL dangling references")
    for issue in issues:
        print(f"    {issue}")


if __name__ == "__main__":
    main()
