"""Reader for the player profile and its match journal (``player.ini``).

Specification: docs/formats/profile.md

Reference implementation: correctness and legibility over speed.

A profile is a directory under ``Profiles/``. The file inside it holds the
options that player last chose, a journal of every finished match, and a hash
over that journal. The career the *Change player* screen shows is not stored:
it is aggregated from the journal on every open, and this module does the same
aggregation.

The hash is the proof. Recomputing it from the shipped profile's 57 records
returns the ``hash=`` line that profile carries, bit for bit, which is what
settles the record layout, the fold order, the seed and the nine-slot nation
histogram all at once. ``validate()`` makes that check; ``main()`` prints it.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path

from . import ini

#: The rolling hash's seed, and what a string field folds. Both are literals in
#: the executable. A string contributes the same constant whatever it holds.
HASH_SEED = 0x48564849
HASH_STRING = 0x0EA75617

_MASK = 0xFFFFFFFF

#: The keys every ``[game<n>]`` record must carry, in the order the record
#: reader asks for them. ``id``, ``level_max_unit`` and ``favorite`` are text;
#: the rest are whole signed decimals.
GAME_TEXT_KEYS = ("id", "level_max_unit", "favorite")
GAME_INT_KEYS = (
    "year", "month", "day", "hour", "minute",
    "duration", "mapsize", "multi", "lost",
    "gold", "food",
    "units_prod", "units_killed", "units_lost", "units_max",
    "level_max", "health_sacr", "priests",
    "enemies", "allies", "race",
    "damage_taken", "damage_inflicted",
    "player_id", "poser_score", "kill_healths", "die_healths",
)


class ProfileError(Exception):
    """Raised when a profile's journal is not readable as the engine reads it."""


def fold(hash_: int, value: int) -> int:
    """One step of the rolling hash.

    ``mix(v) = ((((v >> 8) ^ v) >> 5) & 0x07FFFFF8) + (v & 7) + 8v``, xored with
    the previous hash doubled, plus the value, then ``x + (x >> 31)``. Unsigned
    and wrapping throughout.
    """
    value &= _MASK
    mixed = (((((value >> 8) ^ value) & _MASK) >> 5) & 0x07FFFFF8) + (value & 7) + (value * 8)
    folded = (value + ((mixed & _MASK) ^ ((hash_ * 2) & _MASK))) & _MASK
    return (folded + (folded >> 31)) & _MASK


@dataclass(frozen=True)
class Game:
    """One ``[game<n>]`` record. Fields are named as the file names them."""

    id: str
    year: int
    month: int
    day: int
    hour: int
    minute: int
    duration: int
    mapsize: int
    multi: bool
    lost: bool
    gold: int
    food: int
    units_prod: int
    units_killed: int
    units_lost: int
    units_max: int
    level_max: int
    level_max_unit: str
    health_sacr: int
    priests: int
    favorite: str
    enemies: int
    allies: int
    race: int
    damage_taken: int
    damage_inflicted: int
    player_id: int
    poser_score: int
    kill_healths: int
    die_healths: int

    @property
    def score(self) -> int:
        """This match's contribution to the military rating.

        Both sides of the ratio carry a floor, which is what stops a match in
        which nothing happened from dividing by zero; a health counter weighs
        half. Unsigned 32-bit, truncating.
        """
        scored = ((self.kill_healths & _MASK) // 2 + self.damage_inflicted + 1000) * 100
        against = (self.die_healths & _MASK) // 2 + self.damage_taken + 10000
        quotient = (scored & _MASK) // (against & _MASK)
        return quotient - 0x100000000 if quotient >= 0x80000000 else quotient


@dataclass(frozen=True)
class Rank:
    """One row of ``DATA\\CONST.INI``'s ``[Ranks]``."""

    points: int
    name: str


def parse_ranks(document: ini.IniFile) -> list[Rank]:
    """``[Ranks]``, in file order and unsorted.

    The walk stops at the first row with no ``RankPoints``, as the
    executable's does, so a hole shortens the table.
    """
    section = document.section("Ranks")
    if section is None:
        return []
    count = section.get_int("RanksCount", 0) or 0
    ranks: list[Rank] = []
    for index in range(count):
        points = section.get_int(f"RankPoints{index}")
        if points is None:
            break
        ranks.append(Rank(points, section.get("RankName{}".format(index), "") or ""))
    return ranks


def rank_for(ranks: list[Rank], rating: int) -> int:
    """The index of the rank the rating has reached, or ``-1``.

    The last row, **in file order**, whose ``points`` is at or below the
    rating, stopping at the first that is above. The shipped table is not
    sorted: ``RankPoints10 = 2`` makes ``Legend`` answer every rating from 200
    to 299 and leaves ``Hero`` unreachable. See the specification.
    """
    reached = 0
    while reached < len(ranks) and ranks[reached].points <= rating:
        reached += 1
    return reached - 1


@dataclass
class Stats:
    """The career the profile screen shows. None of it is stored in the file."""

    rank: str = ""
    single_games: int = 0
    single_won_percent: int = 100
    multi_games: int = 0
    multi_won_percent: int = 100
    duration: int = 0
    rating_total: int = 0
    favourite_race: int = -1
    favourite_race_percent: int = 100
    gold_spent: int = 0
    food_spent: int = 0
    units_killed: int = 0
    units_lost: int = 0
    health_sacrificed: int = 0
    best_unit: str = ""
    best_level: int = 0
    most_units: int = 0
    hash: int = HASH_SEED

    @property
    def rating(self) -> int:
        """The summed match scores over the matches, the divisor floored at one."""
        matches = self.single_games + self.multi_games
        return self.rating_total // max(1, matches)

    @property
    def hours(self) -> int:
        """``duration`` in whole hours, the only unit the screen shows."""
        return self.duration // 3_600_000

    @property
    def signed_hash(self) -> int:
        """The hash as ``[Player] hash`` spells it."""
        return self.hash - 0x100000000 if self.hash >= 0x80000000 else self.hash


@dataclass
class Profile:
    """A profile directory: its ``[Player]`` block, its journal, its career."""

    directory: str
    name: str = ""
    favourite_unit: str = ""
    colour: int = 0
    race: int = 0
    games: int = 0
    stored_hash: int | None = None
    journal: list[Game] = field(default_factory=list)
    stats: Stats = field(default_factory=Stats)

    def validate(self) -> None:
        """Assert the invariants the specification claims.

        The load-bearing one is the hash: a profile that carries ``hash=``
        must agree with the aggregation. A profile written by this engine
        carries none, and that is not a failure.
        """
        assert self.stats.single_games + self.stats.multi_games == len(self.journal)
        assert 0 <= self.stats.single_won_percent <= 100
        assert 0 <= self.stats.multi_won_percent <= 100
        assert -1 <= self.stats.favourite_race <= 8
        for game in self.journal:
            assert len(game.id) > 0
        if self.stored_hash is not None:
            assert self.stats.signed_hash == self.stored_hash, (
                f"{self.directory}: hash {self.stats.signed_hash} "
                f"disagrees with the stored {self.stored_hash}"
            )


def parse(document: ini.IniFile, directory: str = "") -> Profile:
    """Read a parsed ``player.ini``.

    Raises ``ProfileError`` when a ``[game<n>]`` section is missing a key the
    record needs, which is what the executable does with it.
    """
    profile = Profile(directory=directory)
    player = document.section("Player")
    if player is not None:
        profile.name = player.get("name", "") or ""
        profile.favourite_unit = player.get("fav", "") or ""
        profile.colour = player.get_int("color", 0) or 0
        profile.race = player.get_int("race", 0) or 0
        profile.games = player.get_int("games", 0) or 0
        profile.stored_hash = player.get_int("hash")
    index = 0
    while True:
        section = document.section(f"game{index}")
        if section is None:
            break
        values: dict[str, object] = {}
        for key in GAME_TEXT_KEYS:
            text = section.get(key)
            if text is None:
                raise ProfileError(f"[game{index}] has no {key}")
            values[key] = text
        for key in GAME_INT_KEYS:
            number = section.get_int(key)
            if number is None:
                raise ProfileError(f"[game{index}] has no {key}")
            values[key] = number
        values["multi"] = bool(values["multi"])
        values["lost"] = bool(values["lost"])
        profile.journal.append(Game(**values))  # type: ignore[arg-type]
        index += 1
    aggregate(profile)
    return profile


def read(path: str | Path) -> Profile:
    """Read ``<directory>/player.ini``, named after its directory."""
    path = Path(path)
    if path.is_dir():
        path = path / "player.ini"
    return parse(ini.read(path), directory=path.parent.name)


def profiles(game_dir: str | Path) -> list[Profile]:
    """Every profile in an installation, in directory order.

    The executable enumerates ``Profiles/`` and takes every subdirectory; the
    ``default=`` line in ``profiles.ini`` names one of them and does not list
    them.
    """
    root = Path(game_dir) / "Profiles"
    if not root.is_dir():
        return []
    found = []
    for entry in sorted(root.iterdir()):
        if entry.is_dir() and (entry / "player.ini").is_file():
            found.append(read(entry))
    return found


def default_profile(game_dir: str | Path) -> str | None:
    """``Profiles/profiles.ini``'s ``default=profiles/<name>``, as a name."""
    path = Path(game_dir) / "Profiles" / "profiles.ini"
    if not path.is_file():
        return None
    for section in ini.read(path).sections:
        value = section.get("default")
        if value:
            return value.replace("\\", "/").rsplit("/", 1)[-1]
    return None


def aggregate(profile: Profile, ranks: list[Rank] | None = None) -> Stats:
    """Recompute ``profile.stats`` from ``profile.journal``.

    The order of the folds is the executable's and is not the order the keys
    sit in; see the specification's table. Nine nation counters are folded per
    record, and the tenth slot the guard admits is written and never read.
    """
    stats = Stats()
    single_won = multi_won = 0
    nations = [0] * 10

    def step(value: int) -> None:
        stats.hash = fold(stats.hash, value)

    for game in profile.journal:
        if game.multi:
            if not game.lost:
                multi_won += 1
            stats.multi_games += 1
        else:
            if not game.lost:
                single_won += 1
            stats.single_games += 1
        step(stats.multi_games)
        step(stats.single_games)
        stats.duration += game.duration
        step(game.duration)
        if 0 <= game.race <= 9:
            nations[game.race] += 1
        for nation in range(9):
            step(nations[nation])
        stats.gold_spent += game.gold
        step(game.gold)
        stats.food_spent += game.food
        step(game.food)
        stats.units_killed += game.units_killed
        step(game.units_killed)
        stats.units_lost += game.units_lost
        step(game.units_lost)
        stats.health_sacrificed += game.health_sacr
        step(game.health_sacr)
        step(game.units_max)
        step(1 if game.lost else 0)
        step(game.allies)
        step(game.year & 0xFFFF)
        step(game.month & 0xFFFF)
        step(game.day & 0xFFFF)
        step(game.hour & 0xFFFF)
        step(game.minute & 0xFFFF)
        step(game.enemies)
        step(game.level_max)
        step(HASH_STRING)  # level_max_unit
        step(game.mapsize)
        step(game.priests)
        step(game.units_prod)
        step(HASH_STRING)  # favorite
        step(HASH_STRING)  # id
        stats.rating_total += game.score
        # The level's comparison is unsigned in the executable and the unit
        # count's is signed. Nothing a match can produce tells them apart.
        if (game.level_max & _MASK) > (stats.best_level & _MASK):
            stats.best_level = game.level_max
            stats.best_unit = game.level_max_unit
        if game.units_max > stats.most_units:
            stats.most_units = game.units_max

    if stats.single_games:
        stats.single_won_percent = single_won * 100 // stats.single_games
    if stats.multi_games:
        stats.multi_won_percent = multi_won * 100 // stats.multi_games
    played = sum(nations[:9])
    if played:
        best = 0
        for nation in range(1, 9):
            if nations[best] < nations[nation]:
                best = nation
        stats.favourite_race = best
        stats.favourite_race_percent = nations[best] * 100 // played
    if ranks:
        index = rank_for(ranks, stats.rating)
        if index >= 0:
            stats.rank = ranks[index].name

    profile.stats = stats
    return stats


#: The nine nations, as the ``race`` translation context names them.
RACE_NAMES = (
    "Gaul", "Republican Rome", "Carthage", "Iberia", "Imperial Rome",
    "Britain", "Egypt", "Germany", "random",
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("path", help="a profile directory, its player.ini, or an installation")
    parser.add_argument("--const", help="DATA/CONST.INI, extracted, for the rank names")
    args = parser.parse_args(argv)

    ranks = parse_ranks(ini.read(args.const)) if args.const else []
    path = Path(args.path)
    found = profiles(path) if (path / "Profiles").is_dir() else [read(path)]
    if not found:
        print(f"{args.path}: no profiles", file=sys.stderr)
        return 1

    for profile in found:
        aggregate(profile, ranks)
        profile.validate()
        stats = profile.stats
        nation = RACE_NAMES[stats.favourite_race] if stats.favourite_race >= 0 else "unknown"
        print(f"{profile.directory}: {profile.name or '(unnamed)'}")
        print(f"  Rank: {stats.rank or '(none)'} (military rating {stats.rating})")
        print(f"  Single player games: {stats.single_games} ({stats.single_won_percent}% won)")
        print(f"  Multiplayer games: {stats.multi_games} ({stats.multi_won_percent}% won)")
        print(f"  Game time: {stats.hours} hours")
        print(f"  Favorite nation: {nation} ({stats.favourite_race_percent}%)")
        print(f"  Favorite unit: {profile.favourite_unit or 'unknown'}")
        print(f"  Resources spent: {stats.gold_spent} gold, {stats.food_spent} food")
        print(f"  Units eliminated: {stats.units_killed}")
        print(f"  Units lost: {stats.units_lost}")
        print(f"  Health spent for rituals: {stats.health_sacrificed}")
        print(f"  Most experienced unit: {stats.best_unit or 'Unknown'} (Level {stats.best_level})")
        print(f"  Maximum number of units: {stats.most_units}")
        stored = "none" if profile.stored_hash is None else str(profile.stored_hash)
        print(f"  {len(profile.journal)} records, hash {stats.signed_hash} (stored {stored})")
    return 0


if __name__ == "__main__":  # pragma: no cover - CLI
    raise SystemExit(main())
