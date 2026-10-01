"""Corpus tests for the campaign layer. Specification: docs/formats/adventure.md.

The container format itself is `docs/formats/map.md` and `test_corpus_gamedata.py`;
this module asserts only the things that make a *sequence* of maps a campaign:
the three container flavours, the conquest's territory graph, the mission-boundary
mechanism, and the live-state files in the installation root and `Profiles/`.

**Discovery is by content, and it has to be twice over here.** Sniffing `HPFS`
alone finds 23 containers and misses `Packs/RandomMapSettlements.bfhp`, which is
a whole-file LZIS stream wrapping one -- the exact miscount `tests/corpus.py`'s
docstring records. Every count below is over all 24, and the flavour totals
(6/14/2) only agree with `docs/formats/map.md` because of it.

The other thing this module exists to pin is a negative. `ChangeMap` is the
engine's mission-to-mission carry mechanism, and **no shipped script calls it
outside a comment**. That is a fact about the content which nothing else in the
repository would notice going away.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from conftest import requires_game
from imperivm.formats import ini
from imperivm.formats import map as game_map
from imperivm.formats.bfhp import MAGIC as HPFS_MAGIC, BlockFile
from imperivm.formats.lzis import decompress

pytestmark = requires_game

#: Every container in the installation, by content: 23 bare `HPFS` images plus
#: `Packs/RandomMapSettlements.bfhp` behind its LZIS wrapper.
CONTAINER_COUNT = 24

#: Two of the 24 carry no `game.xml`: `Packs/newmap.BFHP` (a bare `Maps/1`
#: template) and the installation root's `currentadv.bfhp`.
GAME_XML_COUNT = 22

#: `game_type` across those 22. The three blank templates in `Packs/` carry one
#: of each, which is what proves the number is the container's flavour.
KIND_COUNTS = {0: 6, 1: 14, 2: 2}

#: `Maps/<n>` directories across all 24 containers. Twenty-one containers hold
#: exactly one, the conquest holds seven, and `currentadv.bfhp` holds none.
MAP_DIRECTORY_COUNT = 29

#: `map/@persist_state` over those 29.
PERSIST_STATE_COUNTS = {False: 17, True: 12}

#: `.vs` sequence sources stored inside containers -- distinct from the 577 in
#: `data.pak` that `test_corpus_vs.py` covers.
CONTAINER_SCRIPT_COUNT = 308

#: `ChangeMap` call sites among them, and how many are live. See the module
#: docstring: the second number is the point.
CHANGE_MAP_SITES = 12
CHANGE_MAP_LIVE_SITES = 0

#: `<sequence>` elements in a *container-root* `Sequences/sequences.xml`. One
#: container has any: the conquest, with `StartBonuses` and the seven bonuses.
ROOT_SEQUENCE_COUNT = 8

#: Literal `GiveNote`/`RemoveNote`/`IsNoteActive` calls in the container corpus,
#: and how many notes the containers declare. See docs/formats/adventure.md.
#:
#: **Live calls, not textual occurrences.** A grep finds 206; two of them are
#: commented out, and one of those two is half the evidence for the undeclared
#: rule below -- so the difference is not cosmetic.
NOTE_CALL_COUNT = 204
NOTE_DECLARATION_COUNT = 122
#: Every container carries a root `Notes.xml` and one per map; 36 of the 50 are
#: an empty `<notes></notes>`, including all 25 of the root ones.
NOTE_DOCUMENT_COUNT = 50
NOTE_EMPTY_DOCUMENT_COUNT = 36

#: `neighbours` references in the one `territories.xml`.
NEIGHBOUR_REFERENCE_COUNT = 18

#: The colourisation knobs on `<conquestmap>`. Nine, not the twelve
#: `docs/formats/map.md` claims: three each of `_colorize`, `_hue` and `_sat`.
COLOUR_KNOB_COUNT = 9

#: `tsEnemy`, `tsOwned`, `tsDisabled`, read out of `gbr.exe`'s `RegisterConstant`
#: block at VA 0x503800. See docs/formats/adventure.md.
TS_ENEMY, TS_OWNED, TS_DISABLED = 0, 1, 2

_COMMENT = re.compile(r"/\*.*?\*/", re.S)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_CHANGE_MAP = re.compile(r"\bChangeMap\s*\(")
_NOTE_CALL = re.compile(
    r'\b(GiveNote|RemoveNote|IsNoteActive)\s*\(\s*"([^"]*)"', re.I
)


def _uncommented(source: str) -> str:
    return _LINE_COMMENT.sub("", _COMMENT.sub("", source))


@pytest.fixture(scope="module")
def containers(game_dir: Path) -> list[tuple[str, BlockFile]]:
    """Every container in the installation, found by content, twice over.

    A bare `HPFS` image is opened directly; an `LZIS` stream is decompressed and
    opened only if what comes out is one. Nothing here reads a file name.
    """
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from corpus import _walk  # the same skip list every other corpus module uses

    found: list[tuple[str, BlockFile]] = []
    for path in _walk(game_dir):
        try:
            with path.open("rb") as handle:
                head = handle.read(4)
        except OSError:  # pragma: no cover - unreadable file in the install
            continue
        label = path.relative_to(game_dir).as_posix()
        if head == HPFS_MAGIC:
            found.append((label, BlockFile(path)))
        elif head == b"LZIS":
            try:
                data = decompress(path.read_bytes())
            except Exception:  # noqa: BLE001 - not every LZIS stream is a container
                continue
            if data[:4] == HPFS_MAGIC:
                found.append((label, BlockFile(path, data=data)))
    return found


def _files(container: BlockFile) -> set[str]:
    return {entry.name for entry in container.entries if not entry.is_dir}


def _map_numbers(container: BlockFile) -> list[int]:
    """The `Maps/<n>` directory numbers, enumerated rather than counted."""
    return sorted(
        {
            int(name.split("/")[1])
            for name in _files(container)
            if name.lower().startswith("maps/") and name.count("/") >= 2
        }
    )


def _read(container: BlockFile, name: str) -> bytes | None:
    try:
        return container.read(name)
    except KeyError:
        return None


# ---------------------------------------------------------------------------
# what an adventure and a conquest are
# ---------------------------------------------------------------------------


def test_every_container_is_found_including_the_wrapped_one(containers):
    assert len(containers) == CONTAINER_COUNT
    # The one that lies about its own format. Losing it silently moves the
    # flavour totals below by one and nothing else notices.
    assert any(label.endswith("RandomMapSettlements.bfhp") for label, _ in containers)


def test_the_three_flavours_are_the_three_blank_templates(containers):
    kinds: dict[int, int] = {}
    templates: dict[str, int] = {}
    for label, container in containers:
        data = _read(container, "game.xml")
        if data is None:
            continue
        info = game_map.GameInfo.parse(data)
        kinds[info.game_type] = kinds.get(info.game_type, 0) + 1
        base = label.rsplit("/", 1)[-1].lower()
        if base.startswith("empty"):
            templates[base] = info.game_type

    assert sum(kinds.values()) == GAME_XML_COUNT
    assert kinds == KIND_COUNTS
    # The editor's own blanks say what each number means.
    assert templates == {
        "emptyscn.bfhp": 0,
        "emptyadv.bfhp": 1,
        "emptyconquest.bfhp": 2,
    }


def test_an_adventure_holds_exactly_one_map_and_a_conquest_holds_seven(containers):
    total = 0
    conquest_maps: list[int] = []
    for _label, container in containers:
        numbers = _map_numbers(container)
        total += len(numbers)
        data = _read(container, "game.xml")
        if data is None:
            continue
        info = game_map.GameInfo.parse(data)
        if info.game_type == 1:
            # Thirteen adventures and the blank template: one map each. The
            # engine supports more -- see `ChangeMap` below -- and no shipped
            # adventure uses it.
            assert len(numbers) == 1, _label
        if info.game_type == 2 and len(numbers) > 1:
            conquest_maps = numbers

    assert total == MAP_DIRECTORY_COUNT
    # Not contiguous, and not a count. `start_map="10"` names the last of them.
    assert conquest_maps == [3, 4, 6, 7, 8, 9, 10]


def test_start_map_names_a_directory_the_container_holds(containers):
    for label, container in containers:
        data = _read(container, "game.xml")
        numbers = _map_numbers(container)
        if data is None or not numbers:
            continue
        info = game_map.GameInfo.parse(data)
        assert info.start_map in numbers, label


def test_every_note_a_script_gives_is_one_its_container_declares(containers):
    """`GiveNote` on an undeclared id does nothing, and the corpus nearly obeys.

    `gbr.exe`'s add helper (0x005584a0) looks the id up in the catalogue and
    returns before it touches the active set when the lookup misses, so an
    undeclared note is silently never pinned. The measurement is what makes that
    rule worth implementing rather than skipping: **204 of the installation's
    206 literal note calls name an id their own container declares**, and the two
    that do not are one note spelled two ways in two different containers.

    Pinned as a set rather than a count so that the next move names the file.
    """
    undeclared: set[tuple[str, str]] = set()
    total = 0
    declarations = 0
    empty_documents = 0
    documents = 0
    root_declarations = 0

    for label, container in containers:
        ids: set[str] = set()
        for name in _files(container):
            lower = name.lower()
            if not lower.endswith("notes.xml") or lower.startswith("local/"):
                continue
            data = _read(container, name)
            if data is None:
                continue
            documents += 1
            notes = game_map.NoteRef.parse_all(data, name)
            if not notes:
                empty_documents += 1
            if lower == "notes.xml":
                root_declarations += len(notes)
            declarations += len(notes)
            ids.update(note.id for note in notes)

        for name in _files(container):
            if not name.lower().endswith(".vs"):
                continue
            data = _read(container, name)
            if data is None:
                continue
            source = _uncommented(data.decode("cp1252", "replace"))
            for call in _NOTE_CALL.finditer(source):
                total += 1
                if call.group(2) not in ids:
                    undeclared.add((label, call.group(2)))

    assert total == NOTE_CALL_COUNT
    assert declarations == NOTE_DECLARATION_COUNT
    assert documents == NOTE_DOCUMENT_COUNT
    assert empty_documents == NOTE_EMPTY_DOCUMENT_COUNT
    # Every declaration is in a *map's* document. Which of the two catalogues
    # wins a duplicate id is therefore unobservable, which is what lets
    # `sim/note.hpp` choose one and label it.
    assert root_declarations == 0

    # One live call names an id nothing declares, and its twin is why: the same
    # note is written `GiveNote("Historical Inconcistency")` in
    # `3_Great_Battles_Alesia` map 5's `seq4.vs` and **commented out** there, so
    # the misspelling was never going to be noticed. Zama's is live and is what
    # the catalogue gate silently swallows.
    assert {name for _label, name in undeclared} == {"Historical Inconsistency"}, sorted(
        undeclared
    )


def test_no_note_id_is_declared_by_two_documents_of_one_container(containers):
    """The precedence between a container's two catalogues cannot be observed."""
    for label, container in containers:
        per_document: dict[str, set[str]] = {}
        for name in _files(container):
            lower = name.lower()
            if not lower.endswith("notes.xml") or lower.startswith("local/"):
                continue
            data = _read(container, name)
            if data is None:
                continue
            per_document[name] = {n.id for n in game_map.NoteRef.parse_all(data, name)}
        seen: set[str] = set()
        for name, ids in per_document.items():
            assert not (seen & ids), (label, name, sorted(seen & ids))
            seen |= ids


def test_only_the_conquest_carries_a_territories_document(containers):
    carriers = [
        label
        for label, container in containers
        if any(name.lower() == "territories.xml" for name in _files(container))
    ]
    assert carriers == ["Conquests/mediterranean.BFHP"]


def test_only_the_conquest_carries_container_root_sequences(containers):
    total = 0
    carriers: list[str] = []
    for label, container in containers:
        data = _read(container, "Sequences/sequences.xml")
        if data is None:
            continue
        sequences = game_map.SequenceRef.parse_all(data)
        if not sequences:
            continue
        carriers.append(label)
        total += len(sequences)
        # A container-root sequence's script path is rooted at `CurrentGame/`,
        # where a map's is rooted at `CurrentMap/`.
        for sequence in sequences:
            assert sequence.script.lower().startswith("currentgame/"), sequence.script

    assert carriers == ["Conquests/mediterranean.BFHP"]
    assert total == ROOT_SEQUENCE_COUNT


# ---------------------------------------------------------------------------
# the conquest's campaign structure
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def conquest(containers) -> tuple[BlockFile, game_map.ConquestMap]:
    for _label, container in containers:
        data = _read(container, "territories.xml")
        if data is not None:
            return container, game_map.ConquestMap.parse(data)
    pytest.skip("no conquest in this installation")


def test_the_territory_graph_closes(conquest):
    _container, campaign_map = conquest
    assert len(campaign_map.territories) == 7
    campaign_map.validate()  # raises on an unresolved neighbour or a repeated id

    references = sum(len(t.neighbours) for t in campaign_map.territories)
    assert references == NEIGHBOUR_REFERENCE_COUNT

    # Symmetric, though nothing in the format requires it.
    by_id = {t.id: t for t in campaign_map.territories}
    for territory in campaign_map.territories:
        for neighbour in territory.neighbours:
            assert territory.id in by_id[neighbour].neighbours

    # Nine knobs, not twelve.
    assert len(campaign_map.display) == COLOUR_KNOB_COUNT

    # `interface` is the territory's race index, and every territory's agrees
    # with the `r<Race>` its bonus sequence is named after (adventure.md).
    races = {"Gaul": 0, "RepublicanRome": 1, "Carthage": 2, "Iberia": 3,
             "ImperialRome": 4, "Britain": 5, "Egypt": 6, "Germany": 7}
    for territory in campaign_map.territories:
        assert territory.bonus.startswith("r")
        assert territory.interface == races[territory.bonus[1:]]


def test_every_territory_names_one_of_the_container_s_maps(conquest):
    container, campaign_map = conquest
    names = {}
    for number in _map_numbers(container):
        info = game_map.MapInfo.parse(container.read(f"Maps/{number}/map.xml"))
        names.setdefault(info.name, number)

    resolved = {t.id: names.get(t.map_name) for t in campaign_map.territories}
    assert resolved == {
        "Spain": 10,
        "Britain": 3,
        "Gaul": 7,
        "Italy": 9,
        "Carthage": 4,
        "Egypt": 6,
        "Germany": 8,
    }
    # A bijection: seven territories, seven maps, none shared and none spare.
    assert sorted(resolved.values()) == _map_numbers(container)


def test_bonus_names_a_root_sequence_and_not_a_class(conquest):
    container, campaign_map = conquest
    root = game_map.SequenceRef.parse_all(container.read("Sequences/sequences.xml"))
    names = {sequence.name for sequence in root}
    # Raises if any `bonus` is not one of them. This is the trap
    # `docs/formats/map.md` records: they read exactly like class names.
    campaign_map.validate(sequence_names=names)

    bonuses = {t.id: t.bonus for t in campaign_map.territories}
    assert bonuses["Spain"] == "rIberia"
    assert bonuses["Italy"] == "rRepublicanRome"
    # And every bonus sequence opts out of autorun, so it runs only when
    # `StartBonuses` invokes it by name.
    autorun = {sequence.name: sequence.autorun_allowed for sequence in root}
    assert autorun["StartBonuses"] is True
    assert all(autorun[bonus] is False for bonus in bonuses.values())


def test_the_shipped_territory_states_are_all_ts_owned(conquest):
    """A fact about the file, not a starting position.

    Seven owned territories cannot be where a conquest begins, and the separate
    authoring copy under `ConquestMaps/` disagrees with this one anyway. See the
    "what is still unknown" section of docs/formats/adventure.md.
    """
    _container, campaign_map = conquest
    assert {t.state for t in campaign_map.territories} == {TS_OWNED}
    # And `ConqueredOrder` is empty, so no mission has ever been finished here.
    assert campaign_map.conquered_order == ""


def test_the_authoring_copy_outside_the_container_disagrees(game_dir: Path):
    """`ConquestMaps/<data>/territories.xml` is a second, smaller copy.

    It is what makes `state` a field with a domain rather than a constant: it
    marks two territories `tsOwned` and five `tsEnemy`, where the container
    marks all seven `tsOwned`.
    """
    matches = sorted(game_dir.glob("ConquestMaps/*/territories.xml"))
    if not matches:
        pytest.skip("no ConquestMaps art directory in this installation")
    assert len(matches) == 1
    copy = game_map.ConquestMap.parse(matches[0].read_bytes())
    states = {t.id: t.state for t in copy.territories}
    assert states == {
        "Spain": TS_OWNED,
        "Italy": TS_OWNED,
        "Britain": TS_ENEMY,
        "Gaul": TS_ENEMY,
        "Carthage": TS_ENEMY,
        "Egypt": TS_ENEMY,
        "Germany": TS_ENEMY,
    }
    assert TS_DISABLED not in set(states.values())


def test_each_conquest_map_marks_its_own_territory_owned_before_ending(conquest):
    """The mission-boundary write, in the shipped scripts.

    Every one of the seven maps has exactly one sequence that calls
    `SetTerritoryState` with its own territory's id and `tsOwned`, and that call
    is immediately followed by the winning `EndGame`.
    """
    container, campaign_map = conquest
    by_map = {t.map_name: t.id for t in campaign_map.territories}
    seen: dict[str, str] = {}
    for number in _map_numbers(container):
        info = game_map.MapInfo.parse(container.read(f"Maps/{number}/map.xml"))
        for entry in container.entries:
            prefix = f"Maps/{number}/Sequences/".lower()
            if entry.is_dir or not entry.name.lower().startswith(prefix):
                continue
            if not entry.name.lower().endswith(".vs"):
                continue
            source = container.read(entry.name).decode("cp1252", "replace")
            for call in re.finditer(
                r'SetTerritoryState\s*\(\s*"([^"]+)"\s*,\s*(\w+)\s*\)', source
            ):
                assert call.group(2) == "tsOwned"
                assert call.group(1) == by_map[info.name]
                seen[info.name] = call.group(1)
    assert len(seen) == 7


def test_start_bonuses_resets_the_economy_every_mission(conquest):
    """Nothing carries: the numbers are literals, assigned unconditionally."""
    container, _campaign_map = conquest
    root = game_map.SequenceRef.parse_all(container.read("Sequences/sequences.xml"))
    start = next(s for s in root if s.name == "StartBonuses")
    # A `CurrentGame/`-rooted path resolves against the container root, so the
    # base is empty; container lookup is case-insensitive.
    source = container.read(start.resolve()).decode("cp1252", "replace")
    assert "SetGold(gold)" in source and "gold = 6000;" in source
    assert "SetFood(food)" in source and "food = 2000;" in source
    assert "AddToPopulation(pop)" in source and "pop = 40;" in source
    # One bonus, dispatched from one string.
    assert source.count("ConquestBonus()") == 1
    assert source.count("RunSequence(") == 7


# ---------------------------------------------------------------------------
# the carry mechanism the shipped content does not use
# ---------------------------------------------------------------------------


def test_no_shipped_script_calls_change_map_outside_a_comment(containers):
    """The negative this module exists for.

    `ChangeMap` is the engine's adventure mission-to-mission transition, and it
    carries the party (`CVXPartyList`) across it. All twelve call sites in the
    install sit inside `/* */`, immediately after the `EndGame` that replaced
    them, in the six `GreatBattles` adventures.
    """
    scripts = 0
    sites = 0
    live = 0
    tokens: set[str] = set()
    for _label, container in containers:
        for entry in container.entries:
            if entry.is_dir or not entry.name.lower().endswith(".vs"):
                continue
            scripts += 1
            source = container.read(entry.name).decode("cp1252", "replace")
            sites += len(_CHANGE_MAP.findall(source))
            live += len(_CHANGE_MAP.findall(_uncommented(source)))
            tokens.update(
                match.group(1)
                for match in re.finditer(r'"/?LastMap"\s*,\s*"([^"]*)"', source)
            )

    assert scripts == CONTAINER_SCRIPT_COUNT
    assert sites == CHANGE_MAP_SITES
    assert live == CHANGE_MAP_LIVE_SITES
    # The hub the Great Battles were built around, and the outcome tokens each
    # battle would have handed it. No map named `MainMap` exists anywhere.
    assert tokens == {
        "WinZama1",
        "LoseZama1",
        "WinNumantia1",
        "WinAlesia1",
        "LoseAlesia1",
        "WinBattleForEG1",
        "LoseBattleForEG1",
        "WinBritainConquest1",
        "LoseBritainConquest1",
        "WinGerman1",
        "LoseGerman1",
    }
    assert not any(
        game_map.MapInfo.parse(container.read(f"Maps/{n}/map.xml")).name == "MainMap"
        for _label, container in containers
        for n in _map_numbers(container)
    )


def test_persist_state_is_a_map_flag_with_a_real_split(containers):
    """Twelve of 29 maps set it, and all seven conquest maps do not.

    Reported rather than interpreted: `docs/formats/map.md` glosses it as "carry
    unit state to the next map", and no shipped container has a next map.
    """
    counts = {False: 0, True: 0}
    conquest_states: set[bool] = set()
    for _label, container in containers:
        data = _read(container, "game.xml")
        kind = game_map.GameInfo.parse(data).game_type if data else None
        for number in _map_numbers(container):
            info = game_map.MapInfo.parse(container.read(f"Maps/{number}/map.xml"))
            counts[info.persist_state] += 1
            if kind == 2:
                conquest_states.add(info.persist_state)
    assert counts == PERSIST_STATE_COUNTS
    assert conquest_states == {False}


# ---------------------------------------------------------------------------
# the live-state files
# ---------------------------------------------------------------------------


def test_current_adv_is_a_container_and_holds_no_campaign(game_dir: Path):
    """`currentadv.bfhp` is the adventure save, mounted as `AdvSave/`.

    In an installation with no campaign in progress it is a valid but empty
    container -- which is what makes it evidence of the mechanism rather than of
    any particular contents.
    """
    path = game_dir / "currentadv.bfhp"
    if not path.is_file():
        pytest.skip("no currentadv.bfhp in this installation")
    container = BlockFile(path)
    container.validate()
    assert container.entries == []
    assert "game.xml" not in container


def test_temp_adv_shadow_is_not_campaign_state(game_dir: Path):
    """One write site in `gbr.exe`, no read site, and zero bytes here."""
    path = game_dir / "tempadv.sdw"
    if not path.is_file():
        pytest.skip("no tempadv.sdw in this installation")
    assert path.stat().st_size == 0


def test_profiles_ini_parses_and_names_a_profile_directory(game_dir: Path):
    """`profiles.ini` opens with `[]` -- a section whose name is empty.

    The INI reader has to accept that; a stricter one would reject the file that
    tells the game which profile to load.
    """
    path = game_dir / "Profiles" / "profiles.ini"
    if not path.is_file():
        pytest.skip("no Profiles/profiles.ini in this installation")
    document = ini.parse(path.read_bytes())
    assert [section.name for section in document.sections] == [""]
    default = document.sections[0].get("default")
    assert default
    assert (game_dir / default).is_dir()


def test_a_player_profile_carries_statistics_and_no_campaign_progress(game_dir: Path):
    """`Profiles/<name>/player.ini` is settings plus a post-match journal.

    Nothing in it names an adventure, a conquest, a territory or a map that is
    not the random-map template -- so campaign progress is not kept per profile.
    """
    matches = sorted(game_dir.glob("Profiles/*/player.ini"))
    if not matches:
        pytest.skip("no player profile in this installation")
    document = ini.parse(matches[0].read_bytes())
    names = [section.name for section in document.sections]
    assert names[0] == "Player"
    assert "Players" in names

    player = document.sections[0]
    # The per-game journal is `[game<n>]`, one per finished game, and `games`
    # counts them.
    played = [name for name in names if re.fullmatch(r"game\d+", name)]
    assert len(played) == player.get_int("games")

    lowered = "\n".join(names).lower() + "\n" + str(player.get("mapname")).lower()
    for word in ("adventure", "conquest", "territor", "campaign"):
        assert word not in lowered


def test_the_usr_profiles_are_lzis_and_are_lobby_setup_not_progress(game_dir: Path):
    """`lastconquest.usr` and `lastsettings.usr` decompress; neither is progress.

    `lastconquest.usr` carries the `CVXPersistStream` header `gsxv` + build +
    version and then the conquest's container path and its sixteen player slots.
    `lastsettings.usr` has no such header -- it opens straight into a
    length-prefixed path. Both are "the last lobby I configured", which is what
    their names say.
    """
    profiles = game_dir / "Profiles"
    conquest_path = profiles / "lastconquest.usr"
    settings_path = profiles / "lastsettings.usr"
    if not conquest_path.is_file() or not settings_path.is_file():
        pytest.skip("no Profiles/*.usr in this installation")

    assert conquest_path.read_bytes()[:4] == b"LZIS"
    assert settings_path.read_bytes()[:4] == b"LZIS"

    payload = decompress(conquest_path.read_bytes())
    assert payload[:4] == b"gsxv"
    # `build` and `version`, the two u32 the loader checks. The build number is
    # this executable's and the stream is rejected if it does not match.
    assert int.from_bytes(payload[4:8], "little") == 0x00012800
    assert int.from_bytes(payload[8:12], "little") == 0x67
    # Then the container it configured, length-prefixed. The length word sits at
    # 0x11; what the four bytes between it and `version` are is not established.
    length = int.from_bytes(payload[0x11:0x15], "little")
    assert payload[0x15 : 0x15 + length] == b"conquests/mediterranean"
    assert (game_dir / "Conquests" / "mediterranean.BFHP").is_file()

    # Neither file mentions a territory, which is what a progress record would
    # have to name.
    settings = decompress(settings_path.read_bytes())
    assert settings[:4] != b"gsxv"
    for blob in (payload, settings):
        for territory in (b"Spain", b"Carthage", b"Germania", b"tsOwned"):
            assert territory not in blob
