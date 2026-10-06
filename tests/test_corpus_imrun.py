"""End-to-end tests for `imrun` against the shipped campaign containers.

Every other test in this repository checks a part. This one checks that the
parts are *connected*, which is the failure this project keeps hitting: the
settlement timers, the command table and — until now — the whole campaign layer
were each complete, registered, ordered and unit-tested while nothing ran the
data's own path.

Concretely, three gaps that no unit test could have shown, because each
subsystem passed its own:

* `Installation` resolved scripts against `data.pak` only, so a container's own
  `Maps/<n>/Sequences/seq*.vs` was unreachable source. `imrun` on an adventure
  started 1,119 scripts and not one of them was the mission.
* Nothing in the engine read a `sequences.xml`, so those 308 scripts — every
  `EndGame`, every root-scope `Env*`, `ConquestBonus`, `SetTerritoryState` —
  never ran.
* `CampaignSystem::configure` had no caller, so the one conquest in the
  installation played with an empty territory table.

So these tests drive the real binary over the real containers and assert on the
things that can only be true if the chain is whole. They are slow by the
standards of this suite and they are worth it.

Point `IMPERIVM_IMRUN` at a built binary, or let discovery find one.
"""

from __future__ import annotations

import collections
import os
import re
import subprocess
from pathlib import Path

import pytest

from conftest import requires_game

import corpus

# Each test is its own subprocess run and the module's fixtures only find a
# binary, so under `--dist loadgroup` its tests may go to different workers.
pytestmark = [requires_game, pytest.mark.spread]


#: The conquest's seven `Maps/<n>` directory numbers. Not contiguous, and not
#: indices: a reader that counted rather than enumerated would take 0..6.
CONQUEST_MAPS = (3, 4, 6, 7, 8, 9, 10)

#: `territories.xml` declares seven territories, one per map.
TERRITORY_COUNT = 7


@pytest.fixture(scope="module")
def imrun() -> Path:
    """The newest built `imrun`, and only if it is newer than the engine.

    Discovery is `corpus.find_tool`, which picks the *newest* candidate rather
    than the first one on a hand-written list -- see the note there for what
    the first-wins version was quietly doing. `IMPERIVM_IMRUN` overrides.
    """
    path, complaint = corpus.find_tool("imrun", "IMPERIVM_IMRUN")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


def run(imrun: Path, game_dir: Path, container: Path, *extra: str, turns: int = 60) -> str:
    result = subprocess.run(
        [str(imrun), str(game_dir), str(container), str(turns), "800", *extra],
        capture_output=True,
        text=True,
        timeout=600,
    )
    assert result.returncode == 0, (
        f"imrun refused {container.name} {' '.join(extra)}:\n"
        f"{result.stdout}\n{result.stderr}"
    )
    return result.stdout


def field(output: str, key: str) -> str:
    match = re.search(rf"^{key}\s+(.*)$", output, re.MULTILINE)
    assert match is not None, f"no `{key}` line in:\n{output}"
    return match.group(1).strip()


def conquest_path(game_dir: Path) -> Path:
    path = game_dir / "Conquests" / "mediterranean.BFHP"
    if not path.is_file():
        pytest.skip("the installation has no Conquests/mediterranean.BFHP")
    return path


@pytest.mark.parametrize("number", CONQUEST_MAPS)
def test_every_conquest_map_loads_and_configures_the_campaign(imrun, game_dir, number):
    """All seven, by directory number.

    Selecting a map used to end the container walk at the map document, which
    left the sixteen `player<i>.xml` unread and made `load_player_table` refuse
    every one of these seven with a bare `malformed`. Parameterising over all
    seven rather than spot-checking one is the point: the bug was invisible on
    the map that happens to sort first.
    """
    output = run(imrun, game_dir, conquest_path(game_dir), str(number))
    assert field(output, "map") == f"Maps/{number}"
    assert f"{TERRITORY_COUNT} territories" in field(output, "campaign")


def test_the_conquest_runs_its_root_sequence_and_writes_the_bonus(imrun, game_dir):
    """The end-to-end proof that the campaign path is reachable.

    `Sequences/seq0.vs` is the conquest's `StartBonuses`. For `/Bonus` to appear
    in the environment's root scope, six separate things all have to work: the
    container manifest is read, `CurrentGame/…` resolves to a container entry,
    the script compiles against the assembled registry, an autorun sequence is
    spawned, `ConquestBonus/0` resolves against a configured `CampaignSystem`,
    and `EnvWriteString/2` — the root-scope arity — reaches the store.
    """
    output = run(imrun, game_dir, conquest_path(game_dir), "10")

    declared = field(output, "sequences")
    assert "17 declared (8 game, 9 map)" in declared
    assert "10 autorun started" in declared

    assert "environment, root scope:" in output
    assert re.search(r'^\s+/Bonus\s+= ""', output, re.MULTILINE), output

    # Nothing failed to resolve or compile. A container script the resolver
    # cannot find is reported as `no source:`, which would be a silently
    # missing mission rather than a failing one.
    assert "no source: " not in output
    assert "compile failed: " not in output


def test_an_adventure_runs_its_map_sequences(imrun, game_dir):
    """The adventures have no `territories.xml` and every one of them has a
    `Maps/<n>/Sequences` manifest, which is the other half of the same wiring."""
    zama = game_dir / "Adventures" / "GreatBattles" / "1_Great_Battles_Zama.bfhp"
    if not zama.is_file():
        pytest.skip("the installation has no GreatBattles/1_Great_Battles_Zama.bfhp")
    # Long enough for every sequence to reach its first host call: three of the
    # seventeen open with a `Sleep` and are still suspended at turn 60.
    output = run(imrun, game_dir, zama, turns=200)

    assert field(output, "map") == "Maps/6"
    assert field(output, "campaign").startswith("no territories.xml")
    assert "17 declared (0 game, 17 map), 17 autorun started" in field(output, "sequences")

    # Every one of the seventeen reaches the interpreter, and **none** of them
    # dies at its own first unimplemented host call any more, so none appears
    # among the traps.
    #
    # **This number has moved three times, and what is pinned is the set rather
    # than the count** so that the next move arrives as a diff naming the script
    # that changed. All seventeen trapped until the `c<class id>` and `<group>`
    # global resolution steps landed -- three of them on an `unknown global`
    # (`cMilitary` in seq9, `NO_Village1` in seq14, `NO_Utica` in seq16), which
    # took it to fourteen. Then the `Wait*` family landed and took it to six:
    # seq4, seq6, seq7, seq8, seq11, seq13 and seq15 were all sitting on a
    # `WaitEnvIntBetween`, a `WaitQueryCountBetween` or a `WaitEmptyQuery`, and
    # seq10 on a `WaitEmptyQuery`.
    #
    # **seq10 came back when spawn templates did, and it is a symptom rather
    # than a regression.** It is the lose condition -- `Sleep(60000);
    # WaitEmptyQuery(Q_Scipio, -1); ClearNotes(); EndGame(1, true, "Scipio
    # Africanus has died.")` -- and `Q_Scipio` is one object, authored as an
    # unspawned template. Until templates were honoured, that template counted
    # as a live member, so the query was never empty and seq10 blocked at its
    # `WaitEmptyQuery` forever. Now the group is correctly empty until something
    # spawns it, and the thing that would is `SpawnGroup("Q_Scipio")` on line 26
    # of **seq2** -- whose *first* statement is `BlockUserInput()`, which is
    # unimplemented and is already in this set. So seq2 dies before it can put
    # Scipio on the field, seq10's sleep expires, and Zama declares the player
    # lost sixty seconds in.
    #
    # The chain is the point: seq10 is downstream of seq2, and it will leave
    # this set again the moment `BlockUserInput` gets a body, without anything
    # here changing. What used to hide it was a phantom object.
    #
    # **seq0 and seq3 left when the AI-helper runner landed**, and they are the
    # move this pinning exists to name. Both are `RunAIHelper`/
    # `IsAIHelperRunning` sites -- seq3 polls `IsAIHelperRunning("OutpostGuards1")`
    # inside its `while(1)`, seq0 reaches a `RunAIHelper` after its diplomacy
    # block -- and neither could get past the call while the family was declared
    # and unimplemented. seq10 stayed: it is still downstream of seq2's
    # `BlockUserInput`.
    #
    # **And seq10 left when `ClearNotes` got a body**, which is the second half
    # of that same prediction arriving by a route nobody wrote down. seq10 is
    # `Sleep(60000); WaitEmptyQuery(Q_Scipio, -1); ClearNotes(); EndGame(...)`,
    # and it was trapping on the `ClearNotes` — not on anything upstream. It
    # runs to `EndGame` now.
    #
    # **seq1 left when `IntArray` did.** Its first four statements are
    # `IntArray nA_Conditions;` and a loop that writes `nA_Conditions[n] = 0`
    # four times — writing past the end of an array it has just declared empty,
    # which is the idiom that opens four shipped sequences. `IntArray` and
    # `StrArray` had no owner: `WorldHost::default_value` listed them as *other
    # domains'* and no domain took them, so every one of those writes was a
    # `host refused this subscript assignment` and seq1 died on line 6. It is
    # the fourth script to leave this set, and the fourth to leave for a reason
    # nobody predicted when it entered.
    #
    # **And then the set emptied, over two commits, and neither of them said
    # so.** That is the interesting part of this entry: a pin only earns its
    # keep if the commit that moves it updates it, and twice in a row one did
    # not, so this assertion spent two commits failing and saying nothing that
    # was not already true.
    #
    #   * **seq12 and seq2 left together**, in the camera-and-chrome commit
    #     that gave `PlayMovie` and `BlockUserInput` bodies. seq12 is one
    #     statement — `PlayMovie(Translate("movies\\1_Intro_Great_B_Zama.avi"))`
    #     — and `BlockUserInput` is seq2's first. And with seq2 went the chain
    #     the comment above predicted out loud: seq2 reaches its
    #     `SpawnGroup("Q_Scipio")` now, so seq10's `WaitEmptyQuery` has
    #     something to wait for and Zama stops declaring the player lost sixty
    #     seconds in. That was written down before it happened, which is the
    #     whole reason the *set* is pinned and not the count.
    #   * **seq5 left with `WaitConvRequest`**, the last of the `Wait*` family,
    #     one commit later.
    #
    # So the assertion is now that **no sequence traps at all**. That is a
    # weaker pin in one direction — it cannot shrink again — and a stronger
    # claim in the other: every one of Zama's seventeen sequences runs to its
    # own logic rather than dying at the host surface, and any regression that
    # puts one back arrives as a diff naming it.
    sources = set(re.findall(r"\((Maps/6/[Ss]equences/seq\d+\.vs)\)", output))
    trapping = {re.search(r"seq\d+", source).group(0) for source in sources}
    assert trapping == set(), sorted(trapping)

    # And no trap anywhere in the run is an unresolved global. That is the
    # property the two resolution steps exist for, and it holds over the whole
    # session rather than over the sequences alone.
    assert "unknown global" not in output, output


# ---------------------------------------------------------------------------
# the campaign between missions
# ---------------------------------------------------------------------------


def test_a_won_conquest_mission_carries_its_reward_to_the_next(imrun, game_dir, tmp_path):
    """The mission boundary, on the real conquest, headless.

    Map 10 is Spain: its victory sequence writes `SetTerritoryState("Spain",
    tsOwned)` and ends the game won, and no headless run wins it in a minute,
    so `--declare-won` ends the human's match the way that sequence would and
    says so in the output. What follows is the real path: the session's
    campaign answers the carry, `territory_of_map` names Spain from the map
    number, the file is written -- and a fresh session on map 4 restores it
    before its sequences start, so that the container-root `StartBonuses`
    reads `ConquestBonus()` as `rIberia` and runs that sequence. Three of the
    seven bonuses never run in the retail game (`docs/formats/adventure.md`);
    `rIberia` is one of the four that do.
    """
    carry = tmp_path / "mediterranean.campaign.ini"
    container = conquest_path(game_dir)

    # A lost -- here, undecided -- mission writes nothing.
    output = run(imrun, game_dir, container, "10", "--campaign", str(carry), "--human", "0",
                 turns=5)
    assert field(output, "carry") == "no campaign in progress"
    assert not carry.exists()

    output = run(imrun, game_dir, container, "10", "--campaign", str(carry), "--human", "0",
                 "--declare-won", turns=5)
    assert "declared  the human's match won by --declare-won" in output, output
    assert re.search(r'^carry\s+wrote .*: territory 0, 1 conquered, bonus "rIberia"$', output,
                     re.MULTILINE), output
    text = carry.read_text()
    assert "[Campaign]" in text
    assert "container=Conquests/mediterranean.BFHP" in text
    assert "territories=Spain,Britain,Gaul,Italy,Carthage,Egypt,Germany" in text
    assert "conquered=0" in text
    assert "active_bonus=rIberia" in text

    # The next mission, with the carry and the trace of the root sequence.
    result = subprocess.run(
        [str(imrun), str(game_dir), str(container), "5", "800", "4", "--campaign", str(carry)],
        capture_output=True,
        text=True,
        timeout=600,
        env={**os.environ, "IMRUN_TRACE": "seq0"},
    )
    assert result.returncode == 0, result.stdout + result.stderr
    output = result.stdout
    assert re.search(r'^carry\s+.* restored: 1 conquered, bonus "rIberia"$', output, re.MULTILINE), (
        output
    )
    assert 'ConquestBonus() -> "rIberia"' in output, output
    assert 'RunSequence("rIberia") -> void' in output, output

    # A carry of another conquest is reported and leaves the authored start.
    other = tmp_path / "other.campaign.ini"
    other.write_text(text.replace("container=Conquests/mediterranean.BFHP", "container=Conquests/other.bfhp"))
    output = run(imrun, game_dir, container, "4", "--campaign", str(other), turns=2)
    assert "is a campaign of Conquests/other.bfhp, not of this container" in field(output, "carry")


# ---------------------------------------------------------------------------
# a skirmish at war, and a town that defends itself
# ---------------------------------------------------------------------------

#: The turn the Crossroads skirmish below is held at. Nothing is ordered and
#: the human seat is idle. It used to end at turn 2,369, when one p2 hero
#: taunted p0's town hall to nothing: the town's sentries stood stacked where
#: their walls had placed them, unable to route along blocked walkways, and
#: never reached the guard order that shoots. With the sentries walking their
#: walls -- straight-line routes for `ignore_passability` (0x0040b580),
#: 32-bit script points, class freedom keeping them out of the AI's armies --
#: the town holds: at 12,000 turns the match is undecided. Whether p0's walls
#: get to fight in that time is the AI's business, and since the town halls
#: keep their people (the economy's population ticks, read off gbr.exe) the
#: computer players' armies go after the outposts and nothing reaches p0's
#: walls; that they shoot what does is held by placement instead, in the test
#: after this one. Why the AI does not march on a walled town is an open
#: thread in `docs/plan.html`. The turn is kept cheap: it was once held past
#: p0's first kill, and nothing here asks for one any more.
#: `test_corpus_app_match.py` holds the app to the same world on the same turn.
CROSSROADS_TURNS = 2_200


def test_a_skirmish_on_crossroads_goes_to_war_and_its_walled_town_holds(imrun, game_dir):
    """The vertical slice's claim, headless: nothing is ordered, the computer
    players raise armies, march and fight, and the human seat's walled town
    defends itself with the sentries its walls post -- on the walls, where
    `WALL_PATROL.VS` and `GATE_PATROL.VS` send them, and nowhere else.
    """
    crossroads = game_dir / "Scenarios" / "Crossroads.BFHP"
    if not crossroads.is_file():
        pytest.skip("Crossroads.BFHP is not in this installation")
    result = subprocess.run(
        [str(imrun), str(game_dir), str(crossroads), str(CROSSROADS_TURNS), "800"],
        capture_output=True,
        text=True,
        timeout=1800,
        env={**os.environ, "IMRUN_SETTLEMENTS": "1", "IMRUN_OBJECTS": "Sentry,Walls,Gate"},
    )
    assert result.returncode == 0, result.stdout + result.stderr
    output = result.stdout

    # Not decided: every stronghold still holds its town hall, and p0's -- the
    # first settlement in the table -- is still p0's, its sentries at full.
    assert field(output, "match").startswith("over=no"), output
    lines = re.findall(r"^\s+#\d+ p\d+ kind 1 .*$", output, re.MULTILINE)
    assert len(lines) == 4, lines
    assert all("Townhall" in line for line in lines), lines
    assert re.match(r"^\s+#0 p0 kind 1 .* sentries (\d+)/\1 ", lines[0]), lines[0]

    # A war between the players: blows landed, men died, and a computer
    # player's report counts kills of its own.
    strikes = int(re.search(r"^\s+strikes\s+(\d+) blow", output, re.MULTILINE).group(1))
    deaths = int(re.search(r"^\s+kills\s+(\d+) death", output, re.MULTILINE).group(1))
    assert strikes > 1000, output
    assert deaths > 0, output
    killed = {int(p): int(k) for p, k in
              re.findall(r"report p(\d+): units \d+ produced (\d+) killed", output)}
    assert max(v for p, v in killed.items() if p != 0) > 0, killed

    # Every sentry on the map stands by a wall or gate of its own side, and
    # they are not stacked: none walked off across the map to a point a
    # sixteen-bit multiply had wrapped (issue #7), and none is stuck where its
    # wall placed it (#8, #11) -- before the fixes three stood on one point and
    # a dozen points held two. Two may meet for a moment at a walkway's end.
    # A wall's centre is up to half its length plus its radius from the ends
    # of its walkway; 400 bounds both.
    objects = re.findall(r"^\s+\d+ (\S+) p(\d+) at \((-?\d+),(-?\d+)\) .* order (\S+)$",
                         output, re.MULTILINE)
    sentries = [(p, int(x), int(y)) for cls, p, x, y, _ in objects if "Sentry" in cls]
    posts = [(p, int(x), int(y)) for cls, p, x, y, _ in objects if "Sentry" not in cls]
    assert len(sentries) > 100, len(sentries)
    for p, x, y in sentries:
        near = min(((x - wx) ** 2 + (y - wy) ** 2 for wp, wx, wy in posts if wp == p), default=None)
        assert near is not None and near <= 400 ** 2, (p, x, y)
    spots = collections.Counter((x, y) for _, x, y in sentries)
    shared = {spot: n for spot, n in spots.items() if n > 1}
    assert max(spots.values()) <= 2 and len(shared) <= 2, shared

    # Nothing the AI or a wall reached on the way traps: no unimplemented entry
    # point, and no script that runs its budget out.
    assert "no traps." in output, output[output.find("distinct traps"):]


#: Outside p0's east wall on Crossroads, between its east and south-east
#: walkways, and p0's town hall inside: where an enemy is stood and where it
#: is sent.
P0_OUTSIDE = (3950, 2700)
P0_TOWNHALL = (2758, 2466)


def test_a_walled_towns_sentries_kill_an_enemy_at_its_walls(imrun, game_dir):
    """The walls fight, by placement rather than by waiting for the AI to
    bring a war to them: one of p1's swordsmen, stood outside p0's east wall
    and sent to p0's town hall, walks along the wall to a gate it cannot pass
    and is killed there by one of p0's sentries.
    """
    crossroads = game_dir / "Scenarios" / "Crossroads.BFHP"
    if not crossroads.is_file():
        pytest.skip("Crossroads.BFHP is not in this installation")
    first = subprocess.run(
        [str(imrun), str(game_dir), str(crossroads), str(ORDER_TURN), "800"],
        capture_output=True, text=True, timeout=600,
        env={**os.environ, "IMRUN_OBJECTS": "ESwordsman"},
    )
    assert first.returncode == 0, first.stdout + first.stderr
    enemies = [int(i) for i in
               re.findall(r"^\s+(\d+) ESwordsman p1 at \(-?\d+,-?\d+\) holder 0 ",
                          first.stdout, re.MULTILINE)]
    assert enemies, first.stdout[-3000:]
    unit = enemies[0]

    # Pinned: out of p1's AI, so the order is the test's. Unpinned, the
    # swordsman stays in its squad's `SS_Approach`, and whether p1's recruiter
    # re-routes that squad to some village before the walls reach it is AI
    # timing -- which it did once garrisoned squads were filed under the node
    # their settlement stands in, and the swordsman died to an independent
    # slinger far from p0.
    second = subprocess.run(
        [str(imrun), str(game_dir), str(crossroads), str(ORDER_TURN + 300), "800"],
        capture_output=True, text=True, timeout=600,
        env={**os.environ, "IMRUN_DEATHS": "1", "IMRUN_PIN": "1",
             "IMRUN_PLACE": f"{P0_OUTSIDE[0]},{P0_OUTSIDE[1]}",
             "IMRUN_GOTO": f"{ORDER_TURN}:{unit}:{P0_TOWNHALL[0]},{P0_TOWNHALL[1]}"},
    )
    assert second.returncode == 0, second.stdout + second.stderr
    out = second.stdout
    assert re.search(rf"^\s+place turn {ORDER_TURN}: {unit} at \(-?\d+,-?\d+\)$", out, re.MULTILINE), \
        out[-3000:]
    assert re.search(rf"^\s+goto turn {ORDER_TURN}: {unit} to .*, 1 issued$", out, re.MULTILINE), \
        out[-3000:]
    death = re.search(rf"^\s+death turn (\d+): {unit} ESwordsman p1 at .* by \d+ (\S+) p(\d+)$",
                      out, re.MULTILINE)
    assert death, out[-3000:]
    assert death.group(3) == "0" and "Sentry" in death.group(2), death.group(0)
    assert "no traps." in out, out[out.find("distinct traps"):]


# ---------------------------------------------------------------------------
# bodies that do not stand on one another
# ---------------------------------------------------------------------------

#: Alesia's battle, which the map's own sequences start with nothing ordered.
ALESIA = "Adventures/GreatBattles/3_Great_Battles_Alesia.bfhp"


def test_alesias_armies_do_not_stand_on_one_another(imrun, game_dir):
    """Playtest report #13: a squad sent at one spot ended as one clump, bodies
    drawn through each other. Every route that `Goto` or `GotoAttack` lays now
    ends on its band's goal ring, cut back to a free spot, and reserves that
    end with an owned destination lock, as `gbr.exe` does (`sim/avoidance.hpp`).

    Counted by `imrun`'s overlap census: standing pairs nearer than their two
    radii. Before the locks, 400 turns of Alesia ended with 1,016 such pairs,
    906 of them one body inside the other, and peaked at 1,538 on turn 80 --
    reinforcements standing on their spawn point, attackers on their target's
    centre. With them: 52, 18, and a peak of 139 on turn 50. What is left is
    mostly pairs the map places on one point, which nothing moves.

    And they go round. The smart pathfinder strikes off every goal point a
    standing unit or another's lock covers before it searches (`0x0040a310`),
    so an attacker whose side of a target is taken routes to a free one round
    it; a band with nothing left sends a unit queued far behind to a free spot
    the free-spot search finds (`0x004180b0`). Sampled every ten turns, melee
    units in a fight stood out of reach 819 times against 288 in reach before;
    with both, 357 against 417, and 6,523 blows landed rather than 5,117.
    """
    alesia = game_dir / ALESIA
    if not alesia.is_file():
        pytest.skip(f"{ALESIA} is not in this installation")
    result = subprocess.run(
        [str(imrun), str(game_dir), str(alesia), "400", "800"],
        capture_output=True, text=True, timeout=900,
        env={**os.environ, "IMRUN_OVERLAPS": "10"},
    )
    assert result.returncode == 0, result.stdout + result.stderr
    output = result.stdout
    end = re.search(r"^\s+overlaps\s+(\d+) standing bod\(ies\): (\d+) pair\(s\) nearer than their "
                    r"radii, (\d+) stacked$", output, re.MULTILINE)
    worst = re.search(r"^\s+overlaps\s+worst at turn (\d+): (\d+) pair\(s\) of (\d+) bod\(ies\), "
                      r"(\d+) stacked$", output, re.MULTILINE)
    assert end and worst, output[-3000:]
    bodies, touching, stacked = map(int, end.groups())
    assert bodies > 500, bodies
    assert touching < 150 and stacked < 60, end.group(0)
    assert int(worst.group(2)) < 400, worst.group(0)
    # And the battle is still fought.
    strikes = int(re.search(r"^\s+strikes\s+(\d+) blow", output, re.MULTILINE).group(1))
    assert strikes > 2000, output
    assert "no traps." in output, output[output.find("distinct traps"):]
    melee = re.search(r"^\s+melee\s+sampled: (\d+) engaged, (\d+) closing, (\d+) waiting out of reach$",
                      output, re.MULTILINE)
    assert melee, output[-3000:]
    engaged, _, waiting = map(int, melee.groups())
    assert engaged > 350 and 2 * waiting < 3 * engaged, melee.group(0)
    aims = re.search(r"^\s+free spot\s+(\d+) full band\(s\) searched out from, (\d+) route\(s\) re-aimed$",
                     output, re.MULTILINE)
    assert aims and int(aims.group(2)) > 0, output[-3000:]


# ---------------------------------------------------------------------------
# birds that fly, and come back
# ---------------------------------------------------------------------------


def eagles(imrun: Path, game_dir: Path, map_path: Path, turns: int) -> dict[int, tuple[int, int]]:
    result = subprocess.run(
        [str(imrun), str(game_dir), str(map_path), str(turns), "800"],
        capture_output=True, text=True, timeout=600,
        env={**os.environ, "IMRUN_OBJECTS": "Eagle"},
    )
    assert result.returncode == 0, result.stdout + result.stderr
    found = re.findall(r"^\s+(\d+) Eagle p\d+ at \((-?\d+),(-?\d+)\) ", result.stdout, re.MULTILINE)
    return {int(i): (int(x), int(y)) for i, x, y in found}


def test_balcans_eagles_fly_and_stay_over_the_map(imrun, game_dir):
    """Playtest report #18: every eagle stayed on its start point. Two causes,
    one after the other.

    `Unit::speed` answered 0 for a unit with no movement record, and an eagle
    never has one -- it flies by `Flying::PlayAnim` alone -- so `EAGLE_MOVE.VS`
    stepped `GetVecByDir(dir, 0)`. `0x005d8ad0` reads the class's `speed`.

    With that fixed, the eagles flew off the map in straight lines:
    `GetAngleByDir` had been read as `acos` where `0x0051b7a3` calls `asin`,
    so the script's thirty-degree turn had fixed points and a bird could not
    turn round. With that fault back in, an eagle is at x = 39,380 after 200
    turns, off a map that ends at 32,767.

    Measured with both: after 200 turns the fifteen are 1,202 to 4,719 units
    from where they were after one (median 2,569), all inside the map, and in
    600 turns `EAGLE_IDLE.VS` issues each between 11 and 54 moves. The few at
    the low end are circling: a bird turning 30 degrees per 90-unit step has a
    turning circle of about 174, wider than the script's 100-unit arrival, so
    a target that falls inside it is orbited -- which is the script's own
    geometry, not a host's (inferred; not seen on the original).
    """
    balcans = game_dir / "Scenarios" / "Balcans.BFHP"
    if not balcans.is_file():
        pytest.skip("Balcans.BFHP is not in this installation")
    start = eagles(imrun, game_dir, balcans, 1)
    later = eagles(imrun, game_dir, balcans, 200)
    assert len(start) == 15 and later.keys() == start.keys(), (start, later)
    # What the report was about, and nothing that hangs on where the war
    # happens to send a bird by turn 200: every eagle has left its start by
    # more than one 90-unit step (speed 0 leaves them all where they were),
    # every one is over the map (the acos reading flies them off it), and the
    # flock as a whole has travelled. A bird orbiting a target inside its
    # turning circle may stay near home, so no single eagle is held to a
    # distance.
    moved = []
    for i, (x, y) in later.items():
        sx, sy = start[i]
        d2 = (x - sx) ** 2 + (y - sy) ** 2
        assert d2 > 90 ** 2, (i, start[i], later[i])
        assert 0 <= x <= 32767 and 0 <= y <= 32767, (i, later[i])
        moved.append(d2)
    assert sorted(moved)[len(moved) // 2] > 1000 ** 2, sorted(moved)


# ---------------------------------------------------------------------------
# a gate that stands closed, and opens for a friend
# ---------------------------------------------------------------------------

#: p1's walled town on Crossroads: its four gates, north, west, east and south
#: -- the corners of the quadrilateral its walls enclose -- and its market,
#: inside them.
P1_GATES = ((14058, 1982), (12848, 2308), (14388, 2960), (13178, 3502))
P1_INSIDE = (13600, 2700)
#: When the swordsman is sent in. By then p1's AI has trained a few and marched
#: them out through its gates.
ORDER_TURN = 300


def _inside(x: int, y: int) -> bool:
    """Whether a point is inside the town: on the inner side of each of the
    quadrilateral's four edges, taken north, east, south, west."""
    n, w, e, s = P1_GATES
    corners = (n, e, s, w)
    signs = []
    for (ax, ay), (bx, by) in zip(corners, corners[1:] + corners[:1]):
        signs.append((bx - ax) * (y - ay) - (by - ay) * (x - ax) > 0)
    return all(signs) or not any(signs)


def test_a_gate_stands_closed_and_opens_for_a_friend_walking_through_it(imrun, game_dir):
    """Playtest report #16: a gate never closed, and its passage was open to
    everybody whatever it stood at -- the map's layer leaves every gate's gap
    open.

    `GATE_IDLE.VS` scans 350 units every half second: an enemy closes the gate,
    a friend opens it, nobody closes it. The scan walks units only (the
    original's walk takes the unit mask), so the gate's own walls and towers,
    and the sentries on them (`ignore_passability`), are not friends there.
    One of p1's swordsmen, standing outside the walls, is sent to the market
    inside: the gate it reaches is closed when it is sent, opens as it comes
    within 350, and it walks through to the market.
    """
    crossroads = game_dir / "Scenarios" / "Crossroads.BFHP"
    if not crossroads.is_file():
        pytest.skip("Crossroads.BFHP is not in this installation")
    # Which swordsman: the first of p1's standing outside its walls, well away
    # from every gate.
    first = subprocess.run(
        [str(imrun), str(game_dir), str(crossroads), str(ORDER_TURN), "100"],
        capture_output=True, text=True, timeout=600,
        env={**os.environ, "IMRUN_OBJECTS": "ESwordsman", "IMRUN_GATES": "1"},
    )
    assert first.returncode == 0, first.stdout + first.stderr
    candidates = [
        int(i)
        for i, x, y in re.findall(r"^\s+(\d+) ESwordsman p1 at \((-?\d+),(-?\d+)\) holder 0 ",
                                  first.stdout, re.MULTILINE)
        if not _inside(int(x), int(y))
        and min((int(x) - gx) ** 2 + (int(y) - gy) ** 2 for gx, gy in P1_GATES) > 450 ** 2
    ]
    assert candidates, first.stdout[-3000:]
    unit = candidates[0]
    # A gate of the town stands open only for a friend near it: walls, towers
    # and sentries do not hold it open, so with nobody near it is closed.
    gates = re.findall(r"^\s+gate (\d+) p1 at \((-?\d+),(-?\d+)\) target (\w+)(.*)$", first.stdout,
                       re.MULTILINE)
    assert len(gates) == 4, gates
    assert all(target == "closed" or "friends near" in rest for *_, target, rest in gates), gates
    assert any(target == "closed" and "near" not in rest for *_, target, rest in gates), gates

    second = subprocess.run(
        [str(imrun), str(game_dir), str(crossroads), str(ORDER_TURN + 300), "100"],
        capture_output=True, text=True, timeout=600,
        env={**os.environ, "IMRUN_GATES": "1", "IMRUN_WATCH": str(unit),
             "IMRUN_GOTO": f"{ORDER_TURN}:{unit}:{P1_INSIDE[0]},{P1_INSIDE[1]}"},
    )
    assert second.returncode == 0, second.stdout + second.stderr
    out = second.stdout
    assert re.search(rf"^\s+goto turn {ORDER_TURN}: {unit} to .*, 1 issued$", out, re.MULTILINE), \
        out[-3000:]
    track = [(int(t), int(x), int(y)) for t, x, y in
             re.findall(rf"^\s+watch turn (\d+): {unit} at \((-?\d+),(-?\d+)\)$", out, re.MULTILINE)
             if int(t) >= ORDER_TURN]
    # It arrives inside, at the market.
    arrived = [t for t, x, y in track
               if (x - P1_INSIDE[0]) ** 2 + (y - P1_INSIDE[1]) ** 2 <= 60 ** 2]
    assert arrived, track
    assert _inside(*P1_INSIDE)
    # Through a gate: the one its track came nearest on the way in.
    ids = {(int(x), int(y)): int(i) for i, x, y, _, _ in gates}
    passed = min(P1_GATES, key=lambda g: min((x - g[0]) ** 2 + (y - g[1]) ** 2
                                             for t, x, y in track if t <= arrived[0]))
    gate = ids[passed]
    events = [(int(t), what) for t, what in
              re.findall(rf"^\s+gate turn (\d+): {gate} (opens|closes)$", out, re.MULTILINE)]
    # Closed when the order was given, opened on the way, before it got there.
    before = [what for t, what in events if t <= ORDER_TURN]
    assert not before or before[-1] == "closes", events
    assert any(what == "opens" and ORDER_TURN < t < arrived[0] for t, what in events), (events, track)
    assert "no traps." in out, out[out.find("distinct traps"):]


#: Outside p1's walls, south-west of its south gate, on ground its own
#: swordsmen walk: where an enemy is stood before it is sent in.
P1_OUTSIDE = (12500, 4300)


def test_an_enemy_sent_into_a_walled_town_is_not_routed_through_its_closed_gates(imrun, game_dir):
    """A gate bars an enemy's route per search (`sim/gate.hpp`): the search
    runs on the open grid, finds a straight way in through a gate, and runs
    again with every gate that counts the mover an enemy laid across its
    passage, unless it stands fully open (gbr.exe's 0x00419110). So one of
    p2's swordsmen, stood outside p1's town and sent to its market, gets a
    route that crosses none of p1's shut gates and never gets in.
    """
    crossroads = game_dir / "Scenarios" / "Crossroads.BFHP"
    if not crossroads.is_file():
        pytest.skip("Crossroads.BFHP is not in this installation")
    first = subprocess.run(
        [str(imrun), str(game_dir), str(crossroads), str(ORDER_TURN), "100"],
        capture_output=True, text=True, timeout=600,
        env={**os.environ, "IMRUN_OBJECTS": "Swordsman", "IMRUN_GATES": "1"},
    )
    assert first.returncode == 0, first.stdout + first.stderr
    # p1's enemies: every other player of the four is at war with it.
    enemies = [int(i) for i in
               re.findall(r"^\s+(\d+) \w*Swordsman p[023] at \(-?\d+,-?\d+\) holder 0 ",
                          first.stdout, re.MULTILINE)]
    assert enemies, first.stdout[-3000:]
    unit = enemies[0]
    gates = re.findall(r"^\s+gate (\d+) p1 at \((-?\d+),(-?\d+)\) target (\w+), ([\w ]+?),",
                       first.stdout, re.MULTILINE)
    assert len(gates) == 4, gates
    # Fully open is the one state an enemy's search does not lay a gate in.
    shut = {int(i) for i, _, _, target, through in gates
            if not (target == "open" and through == "lets units through")}
    assert shut, gates

    second = subprocess.run(
        [str(imrun), str(game_dir), str(crossroads), str(ORDER_TURN + 300), "100"],
        capture_output=True, text=True, timeout=600,
        env={**os.environ, "IMRUN_GATES": "1", "IMRUN_WATCH": str(unit),
             "IMRUN_PLACE": f"{P1_OUTSIDE[0]},{P1_OUTSIDE[1]}",
             "IMRUN_GOTO": f"{ORDER_TURN}:{unit}:{P1_INSIDE[0]},{P1_INSIDE[1]}"},
    )
    assert second.returncode == 0, second.stdout + second.stderr
    out = second.stdout
    assert re.search(rf"^\s+place turn {ORDER_TURN}: {unit} at \(-?\d+,-?\d+\)$", out, re.MULTILINE), \
        out[-3000:]
    assert re.search(rf"^\s+goto turn {ORDER_TURN}: {unit} to .*, 1 issued$", out, re.MULTILINE), \
        out[-3000:]
    # The route it was given crosses none of the gates that stood shut.
    routes = [(int(t), what) for t, what in
              re.findall(rf"^\s+route turn (\d+): {unit} crosses (.*)$", out, re.MULTILINE)
              if int(t) > ORDER_TURN]
    assert routes, out[-3000:]
    _, crossed = routes[0]
    crossed_ids = set() if crossed == "no gate" else {int(g) for g in crossed.split(",")}
    assert not crossed_ids & shut, (routes, gates)
    # It was searched again round a gate: the straight way in crossed one.
    searched = re.search(r"^\s+gates\s+(\d+) route\(s\) searched again round a gate", out,
                         re.MULTILINE)
    assert searched and int(searched.group(1)) >= 1, out[-2000:]
    # And it never gets in.
    track = [(int(t), int(x), int(y)) for t, x, y in
             re.findall(rf"^\s+watch turn (\d+): {unit} at \((-?\d+),(-?\d+)\)$", out, re.MULTILINE)
             if int(t) > ORDER_TURN]
    assert track, out[-3000:]
    assert not any(_inside(x, y) for _, x, y in track), track
    assert "no traps." in out, out[out.find("distinct traps"):]
