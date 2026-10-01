"""The player profiles in the installation, and the hash that proves the reading.

`Profiles/<name>/player.ini` holds a journal of every match that player
finished and a `hash=` line folded over it. The career the *Change player*
screen shows is not stored at all: it is aggregated from the journal on every
open, and the hash is the one number that says whether the aggregation is
reading the journal the way the original does.

So this module's load-bearing test is one line. Recompute the fold from the
records and compare it with the `hash=` the file carries. Getting it right
means the record layout, which fields fold, the order they fold in, the seed,
the nine-slot nation histogram and the string constant are all right; getting
any one of them wrong moves the number. `docs/formats/profile.md` has the
whole specification and nothing else in it is independently evidenced -- this
check is its evidence.

The shipped install has one profile, which is enough for the hash but not for
much else, so the arithmetic around it is pinned in
`engine/tests/test_profile.cpp` over records written by hand.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

import corpus
from conftest import requires_game
from imperivm.formats import ini, profile as profile_reader

pytestmark = requires_game


@pytest.fixture(scope="module")
def installed(game_dir):
    """Every profile in the installation, aggregated."""
    return profile_reader.profiles(game_dir)


@pytest.fixture(scope="module")
def const_ini(packs, game_dir) -> ini.IniFile:
    """`DATA\\CONST.INI`, out of `data.pak`, which is where the ranks live."""
    archive = packs.open(game_dir / "Packs" / "data.pak")
    if "DATA\\CONST.INI" not in archive:
        pytest.skip("no DATA/CONST.INI in data.pak")
    return ini.parse(archive.read("DATA\\CONST.INI"))


@pytest.fixture(scope="module")
def imcheck_profile(game_dir):
    """`imcheck profile`, parsed into a dict, for one profile directory."""
    tool, complaint = corpus.find_tool("imcheck", "IMPERIVM_IMCHECK")
    if tool is None or complaint:
        pytest.skip(complaint)

    def run(directory: str) -> dict[str, object]:
        result = subprocess.run(
            [str(tool), "profile", str(game_dir), directory],
            capture_output=True, text=True, timeout=120,
        )
        assert result.returncode == 0, result.stderr
        fields: dict[str, object] = {}
        for line in result.stdout.splitlines():
            key, _, value = line.partition(" ")
            value = value.strip()
            try:
                fields[key] = int(value)
            except ValueError:
                fields[key] = value
        return fields

    return run


def test_the_installation_has_at_least_one_profile(installed):
    # A retail install ships one, `Profiles/Nome`. A tree with none would make
    # every other test here vacuous, so it is a failure rather than a skip.
    assert installed, "no Profiles/<name>/player.ini in the installation"


def test_every_profile_agrees_with_its_stored_hash(installed):
    """The whole specification, in one comparison per profile."""
    checked = 0
    for profile in installed:
        if profile.stored_hash is None:
            continue
        assert profile.stats.signed_hash == profile.stored_hash, (
            f"{profile.directory}: aggregated {profile.stats.signed_hash}, "
            f"file says {profile.stored_hash}"
        )
        checked += 1
    assert checked, "no installed profile carries a hash to check against"


def test_every_profile_validates(installed):
    for profile in installed:
        profile.validate()


def test_the_journal_is_consecutive_and_complete(installed):
    """`games=` counts the records, and the walk found all of them.

    The reader stops at the first absent `game<n>`, so a journal it read
    short would still validate; `[Player] games` is the file's own count and
    catches that.
    """
    for profile in installed:
        assert len(profile.journal) == profile.games, (
            f"{profile.directory}: read {len(profile.journal)} records, "
            f"[Player] games says {profile.games}"
        )


def test_the_default_profile_names_a_directory_that_exists(game_dir, installed):
    default = profile_reader.default_profile(game_dir)
    if default is None:
        pytest.skip("no Profiles/profiles.ini")
    assert default.lower() in {p.directory.lower() for p in installed}


def test_every_record_carries_every_key(installed):
    """A missing key raises rather than defaulting, so reaching here is the check.

    Recorded as a count so that a future install with a truncated record says
    so instead of quietly reading fewer.
    """
    records = sum(len(p.journal) for p in installed)
    assert records > 0
    for profile in installed:
        for game in profile.journal:
            assert len(game.id) == 32, f"{profile.directory}: id {game.id!r} is not 32 hex digits"
            assert int(game.id, 16) >= 0
            assert 1970 <= game.year <= 2200
            assert 1 <= game.month <= 12
            assert 1 <= game.day <= 31
            assert 0 <= game.hour <= 23
            assert 0 <= game.minute <= 59
            assert game.duration >= 0


def test_no_record_claims_a_nation_the_histogram_drops(installed):
    """The guard admits `race` 0..9 into nine counters; nothing shipped uses 9.

    If an install ever does, the engine and the original still agree -- both
    count it towards no nation -- but this specification's claim that the tenth
    slot is unreachable would stop being true of the corpus, and that is worth
    being told about.
    """
    for profile in installed:
        for game in profile.journal:
            assert game.race != 9, f"{profile.directory}: a record claims nation 9"


def test_the_rank_table_is_not_sorted_and_hero_is_unreachable(const_ini):
    """`CONST.INI`'s `[Ranks]`, and the defect the engine reproduces.

    `RankPoints10 = 2` where the section's own comment demands ascending
    order. The walk therefore passes row 10 for any rating at or above 200 and
    only stops at row 11's 300, so `Hero` never names a rank.
    """
    ranks = profile_reader.parse_ranks(const_ini)
    assert len(ranks) == 13
    assert [r.name for r in ranks][:3] == ["Peasant", "Apprentice", "Fighter"]
    assert ranks[9].name == "Hero"
    assert ranks[10].points == 2, "the shipped defect has gone; the reading needs revisiting"
    reachable = {profile_reader.rank_for(ranks, rating) for rating in range(0, 1000)}
    assert 9 not in reachable
    assert profile_reader.rank_for(ranks, 200) == 10
    assert profile_reader.rank_for(ranks, 299) == 10
    assert profile_reader.rank_for(ranks, 300) == 11


def test_the_shipped_profile_reaches_a_rank(installed, const_ini):
    ranks = profile_reader.parse_ranks(const_ini)
    named = 0
    for profile in installed:
        profile_reader.aggregate(profile, ranks)
        if profile.journal:
            assert profile.stats.rank, f"{profile.directory}: rating names no rank"
            named += 1
    assert named


def test_the_engine_and_the_reference_reader_agree(installed, const_ini, imcheck_profile):
    """The C++ aggregation against this one, profile by profile.

    Two implementations written from the same specification is worth something
    only when they are actually compared; without this, the engine could drift
    and only the unit tests' three hand-written records would notice.
    """
    ranks = profile_reader.parse_ranks(const_ini)
    for profile in installed:
        profile_reader.aggregate(profile, ranks)
        engine = imcheck_profile(profile.directory)
        assert engine["hash"] == profile.stats.signed_hash
        assert engine["records"] == len(profile.journal)
        assert engine["rank"] == profile.stats.rank
        assert engine["rating"] == profile.stats.rating
        assert engine["single"] == profile.stats.single_games
        assert engine["single_won"] == profile.stats.single_won_percent
        assert engine["multi"] == profile.stats.multi_games
        assert engine["multi_won"] == profile.stats.multi_won_percent
        assert engine["hours"] == profile.stats.hours
        assert engine["nation"] == profile.stats.favourite_race
        assert engine["nation_percent"] == profile.stats.favourite_race_percent
        assert engine["gold"] == profile.stats.gold_spent
        assert engine["food"] == profile.stats.food_spent
        assert engine["killed"] == profile.stats.units_killed
        assert engine["lost"] == profile.stats.units_lost
        assert engine["health"] == profile.stats.health_sacrificed
        assert engine["level"] == profile.stats.best_level
        assert engine["unit"] == profile.stats.best_unit
        assert engine["most_units"] == profile.stats.most_units
