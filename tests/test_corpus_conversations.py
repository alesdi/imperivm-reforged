"""Every conversation the installation declares, read by the engine's reader.

110 `.conv.xml` documents across the campaign containers, 260 phrases between
them. They are the narration layer: what a mission says, when it says it, and
which of the things it could say it picks.

This module drives `imcheck conv` over every container and asserts on the whole
census, so that no part of it has to be copied into the repository -- see
`docs/legal.md` rule 1 and `tools/check_fixtures.py`. The unit tests in
`engine/tests/test_conversation.cpp` cover the reader's behaviour against
documents written from `docs/formats/conv-xml.md`; what is here is the claim
that the reader agrees with the data.

## The four claims worth pinning

1. **All 110 parse, and the count is exact.** A reader that quietly skipped one
   would still look healthy: the missing conversation would simply never be
   found by `Init`, and the mission would run past it in silence.

2. **One document, one conversation, and the file name is not the name.**
   `cnv1.conv.xml` declares `C_Conv2`. A catalogue keyed on the file would find
   nothing that any script asks for.

3. **The mode vocabulary the shipped data uses is three of the seven** --
   `first`, `end` and `choice`. The other four (`random`, `cycle`,
   `cycle then first`, `cycle then random`) are in the executable's table and
   in this engine's enumeration and are exercised by no container, which is
   worth knowing when one of them turns up in a mod.

4. **98 of the 110 documents end on a `first`.** This is the evidence for the
   whole default-candidate rule: `followup="first"` with no `followup_phrases`
   means *the next phrase in document order*, and the conversation ends when
   there is none. Under any reading where `first` rescanned the document, those
   98 would loop on their own opening line for ever.
"""

from __future__ import annotations

import collections
import subprocess
from pathlib import Path

import pytest

from conftest import requires_game

import corpus

pytestmark = requires_game


DOCUMENTS = 110
PHRASES = 260


@pytest.fixture(scope="module")
def imcheck() -> Path:
    path, complaint = corpus.find_tool("imcheck", "IMPERIVM_IMCHECK")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


@pytest.fixture(scope="module")
def census(imcheck: Path, game_dir: Path):
    """`imcheck conv` over every container, grouped by leading keyword."""
    rows: dict[str, list[list[str]]] = collections.defaultdict(list)
    totals = collections.Counter()
    for container in corpus.container_paths(game_dir):
        result = subprocess.run(
            [str(imcheck), "conv", str(container)],
            capture_output=True,
            text=True,
            timeout=120,
        )
        assert result.returncode == 0, (
            f"imcheck conv refused {container.name}:\n{result.stdout}\n{result.stderr}"
        )
        for line in result.stdout.splitlines():
            # Tab-separated: a conversation name can contain spaces, and the
            # tutorial's do -- `1 Welcome`, `A Feed`, `B Stronghold`.
            head, _, rest = line.partition("\t")
            if head in ("documents", "failures", "total"):
                totals[head] += int(rest.strip())
            elif head:
                rows[head].append([container.name, *rest.split("\t")])
    return rows, totals


def fields(row: list[str]) -> dict[str, str]:
    """`[container, name, 'k=v', ...]` -> a dict, plus `container` and `name`."""
    out = {"container": row[0], "name": row[1]}
    for token in row[2:]:
        key, _, value = token.partition("=")
        out[key] = value
    return out


def test_every_conversation_document_parses(census):
    rows, totals = census
    assert totals["documents"] == DOCUMENTS
    assert totals["failures"] == 0
    # No document was skipped as a duplicate name, so one document really is
    # one conversation.
    assert "duplicate" not in rows
    assert totals["total"] == DOCUMENTS
    assert len(rows["conv"]) == DOCUMENTS


def test_the_file_name_is_not_the_conversation_name(census):
    rows, _ = census
    documents = {(row[0], row[1]) for row in rows["document"]}
    names = {(row[0], row[1]) for row in rows["conv"]}
    # Not one conversation is named after the file that declares it.
    stems = {(container, Path(path).name.split(".")[0]) for container, path in documents}
    assert not (stems & names), "a conversation named after its own file would hide the bug"
    # And the names repeat across containers, so a catalogue keyed globally
    # rather than per map would collide.
    counts = collections.Counter(name for _, name in names)
    assert max(counts.values()) > 1


def test_the_shipped_data_uses_three_of_the_seven_modes(census):
    rows, _ = census
    startup = collections.Counter(fields(row)["startup"] for row in rows["conv"])
    followup = collections.Counter(fields(row)["followup"] for row in rows["phrase"])
    # `startup` is `first` in every one of them.
    assert set(startup) == {"first"}
    assert startup["first"] == DOCUMENTS
    # `followup` is one of three, and `choice` is the rare one.
    assert set(followup) == {"first", "end", "choice"}
    assert sum(followup.values()) == PHRASES
    assert followup["choice"] == 11


def test_most_documents_end_by_running_off_the_last_phrase(census):
    rows, _ = census
    last: dict[tuple[str, str], str] = {}
    for row in rows["phrase"]:
        entry = fields(row)
        last[(entry["container"], entry["name"])] = entry["followup"]
    assert len(last) == DOCUMENTS
    tally = collections.Counter(last.values())
    # 98 documents whose final phrase says `first`. There is nothing after it,
    # so `first` has to mean "the next one, and stop if there is none".
    assert tally["first"] == 98
    assert tally["end"] == 10
    assert tally["choice"] == 2


def test_the_optional_attributes_are_as_rare_as_the_defaults_assume(census):
    rows, _ = census
    conversations = [fields(row) for row in rows["conv"]]
    phrases = [fields(row) for row in rows["phrase"]]

    # `startup_phrases` is present 9 times out of 110, so the *default*
    # candidate list is what the reader is mostly doing.
    assert sum(1 for c in conversations if int(c["startlist"])) == 9
    # `followup_phrases`, 23 times out of 260.
    assert sum(1 for p in phrases if int(p["list"])) == 23
    # 34 documents restore the view.
    assert sum(1 for c in conversations if c["restore_view"] == "1") == 34

    # Script-bearing attributes, which are the ones whose escapes have to be
    # decoded before anything can compile them.
    assert sum(1 for p in phrases if p["cond"] == "1") == 31
    assert sum(1 for p in phrases if p["act"] == "1") == 31
    assert sum(1 for p in phrases if p["ret"] == "1") == 4

    # Every `choice_text` is on a phrase that some `choice` can reach, and
    # there are 22 of them.
    assert sum(1 for p in phrases if p["choice"] == "1") == 22


def test_almost_every_phrase_names_an_actor_the_conversation_declares(census):
    rows, _ = census
    declared: dict[tuple[str, str], set[str]] = collections.defaultdict(set)
    for row in rows["actor"]:
        declared[(row[0], row[1])].add(row[2])

    named = 0
    unknown = []
    for row in rows["phrase"]:
        entry = fields(row)
        if entry["actor"] == "-":
            continue
        named += 1
        if entry["actor"] not in declared[(entry["container"], entry["name"])]:
            unknown.append((entry["container"], entry["name"], entry["actor"]))
    # Two of the 260 name nobody: a `choice` prompt can be the conversation
    # talking rather than a character.
    assert named == 258
    # And every actor a phrase does name is one its own conversation declares,
    # which is what makes `SetActor`'s key checkable at load rather than at
    # play.
    assert not unknown, unknown
