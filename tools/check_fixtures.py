#!/usr/bin/env python3
"""Refuse a fixture that is a copy of the rights holder's data.

    python3 tools/check_fixtures.py [--game DIR] [--min N] [--no-cache] [--jobs N]

`docs/legal.md` rule 1: **no game assets in the repository** -- "not as test
fixtures, not as documentation examples, not temporarily". `.gitignore` blocks
the extensions the assets arrive in, which stops a `.bmp` and a `.pak`; it does
nothing about the case that actually happened, which is retail *text* pasted
into a C++ string literal.

This is the check that catches that case. It reads every text file in the
working tree that git would keep -- tracked **and** untracked-but-not-ignored,
which is what lets it refuse a violation rather than only report one after the
commit -- pulls out the string literals and byte arrays a fixture is written
as, and asks whether the bytes occur in the player's own installation. A hit is
a copy, whatever the surrounding comment calls it.

It needs an installation to compare against, so like the corpus tests in
`tests/test_corpus_*.py` it **skips** rather than fails when there is none. That
is not a hole: the check runs wherever the data is, which is on the machine of
whoever is in a position to paste from it.

## What counts as a fixture

Three shapes, because those are the three the tree uses:

  * a raw string literal, `R"XML(...)XML"` -- how a document fixture is written;
  * an ordinary string literal of at least `--min` characters, after undoing
    the C escapes -- how `test_player.cpp` had a document, one line per literal;
  * a run of at least 40 `0x..` bytes -- how a binary fixture is written.

The threshold exists because short strings collide by accident: a class name, a
file path or an `.ini` key is *supposed* to match the installation, and must,
because it names something in it. Sixty characters is well above any of those
and well below any fixture worth having.

## What does not count

**A quotation inside a comment.** Rule 4 asks for behaviour described in prose,
and prose that cites its evidence -- *"the script says it is shared by the
Roman priest and the Gaul druid"* -- is the description doing its job, not a
fixture. Comment bodies are blanked before the scan.

**Prose in a Markdown document**, for the same reason; a `.md` or `.html` file
is scanned only inside its fenced code blocks, which is where rule 1's
"documentation examples" live.

**A line marked `legal-ok:`**, on the line or the one above it, with a reason
after the colon. Two things in this tree genuinely match by coincidence and one
of them is the Latin alphabet. The marker requires a reason so that a reader can
tell a coincidence from a suppression.

## Speed

The cost is not reading the installation (under a second) but searching its
half gigabyte once per candidate: about two thousand candidates, forty seconds.
So each file's verdict is kept in `.cache/check_fixtures.json` (gitignored),
keyed on the file's bytes, and trusted only while the installation (every
file's path, size and modification time), this checker, the readers that
unpack the installation and the thresholds are all unchanged. A file that is
new or edited is judged in full, so a new fixture is never answered from the
cache; the files that are judged are searched across every core. A cold run
takes about eight seconds, a warm one a tenth of one. `--no-cache` judges
everything afresh.

## What to do about a hit

Write the fixture from the format specification instead of from the file --
`tests/synthetic.py` is the Python side of exactly this discipline, and says
why in its own docstring: a fixture built from the spec cross-checks two
transcriptions, while a fixture copied from the data agrees with itself.

If the claim being tested is genuinely about *the retail document* -- that this
container's territory graph closes, that this script compiles -- then it is a
corpus test, not a unit test, and belongs beside the other twenty in
`tests/test_corpus_*.py`, where it reads the installation and skips without one.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import multiprocessing
import os
import re
import subprocess
import sys
import warnings
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))

#: Files whose whole point is to name the installation's paths and formats.
#: They quote nothing that is not a name, and the name is the content.
SKIP = {
    "tools/check_fixtures.py",
}

#: Only text the compiler or a reader would take literally.
SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".py", ".md", ".html", ".txt"}

RAW = re.compile(r'R"([A-Za-z_]*)\((.*?)\)\1"', re.S)
LIT = re.compile(r'(?<![A-Za-z0-9_"])"((?:[^"\\\n]|\\.)*)"')

#: A run of adjacent string literals, which C concatenates into one. A fixture
#: written as twenty of them is one fixture, and looking at them one at a time
#: is how `test_player.cpp` could hold a whole document while every individual
#: literal stayed under the threshold.
LIT_RUN = re.compile(r'(?<![A-Za-z0-9_"])((?:"(?:[^"\\\n]|\\.)*"\s*)+)')
ARR = re.compile(r"(?:0x[0-9A-Fa-f]{2}\s*,\s*){39,}0x[0-9A-Fa-f]{2}")


def installation() -> Path | None:
    """The same discovery `tests/conftest.py` does, without importing pytest."""
    candidates = [os.environ.get("IMPERIVM_GAME_DIR")]
    candidates += [
        "~/Imperivm",
        "~/Games/Imperivm",
        "~/Desktop/Imperivm",
        "~/Downloads/Imperivm",
        "/Applications/Imperivm",
        "/opt/imperivm",
    ]
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate).expanduser()
        if (path / "Packs" / "data.pak").is_file():
            return path
    return None


#: Extensions whose contents are the kind of thing a fixture is written from.
#: The partial-copy search runs over these alone -- they are about 5% of the
#: installation, and the other 95% is sprite and audio payload that no string
#: literal is ever a trimmed copy of.
TEXTUAL = {".xml", ".ini", ".vs", ".txt", ".conv", ".loc"}


def corpus_bytes(game: Path) -> tuple[bytes, bytes]:
    """`(everything, the text of it)`, packs and containers unpacked.

    One flat blob rather than a per-file search: the question is only whether a
    run of bytes occurs *anywhere* in the data, and `bytes.__contains__` over
    half a gigabyte is faster than opening 900 archive members per fixture.

    The second blob is the same thing restricted to the textual members, and it
    exists so that the partial-copy search below has something small enough to
    sweep. It is about a twentieth of the size.
    """
    import corpus as corpus_paths
    from imperivm.formats.bfhp import BlockFile
    from imperivm.formats.pak import PackFile

    blob = bytearray()
    text = bytearray()

    def add(name: str, payload: bytes) -> None:
        blob.extend(payload)
        blob.extend(b"\0\0")
        stem = name.lower().replace("\\", "/")
        if any(stem.endswith(suffix) for suffix in TEXTUAL):
            text.extend(payload)
            text.extend(b"\0\0")

    for path in corpus_paths.pack_paths(game):
        pack = PackFile(path)
        for entry in pack.entries:
            add(entry.name, pack.read(entry.name))
    for path in corpus_paths.container_paths(game):
        block = BlockFile(path)
        for entry in block.entries:
            if not entry.is_dir:
                add(entry.name, block.read(entry.name))
    for path in game.rglob("*"):
        if path.is_file() and path.stat().st_size < 8_000_000:
            try:
                add(path.name, path.read_bytes())
            except OSError:
                continue
    return bytes(blob), bytes(text)


def partial_run(data: bytes, text: bytes, minimum: int) -> int:
    """The length of the longest retail run inside `data`, or 0.

    A *trimmed* copy is still a copy, and `test_ui.cpp` had one: an `.ini`
    fixture with three widgets deleted from the middle, which no whole-string
    comparison can see. This walks non-overlapping windows of `minimum` bytes,
    so any run of at least `2 * minimum - 1` bytes contains a whole aligned
    window and is found; then it grows the hit outwards to report how much of
    the fixture is really a copy.

    **The threshold here is much higher than the whole-string one, and has to
    be.** A fixture written from a format specification shares that format's
    scaffolding with every file in it: sixty bytes of XML indentation and
    attribute names, or of a `for (i = 1; i <= 8; i += 1) {`, occur in the
    retail data because that is what the format looks like, not because anyone
    copied anything. Two hundred bytes of agreement is a different claim.
    """
    if len(data) < 2 * minimum:
        return 0
    for begin in range(0, len(data) - minimum + 1, minimum):
        window = data[begin : begin + minimum]
        at = text.find(window)
        if at < 0:
            continue
        # Grow left and right for the report. The bound is the fixture, so this
        # terminates in at most `len(data)` steps.
        left, right = begin, begin + minimum
        while left > 0 and text[at - (begin - left) - 1 : at + minimum] == data[left - 1 : right]:
            left -= 1
        while right < len(data) and data[left:right + 1] in text:
            right += 1
        return right - left
    return 0


def unescape(text: str) -> bytes:
    """A C string literal's actual bytes, or empty when it will not decode."""
    import codecs
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            decoded = codecs.decode(text.encode("utf-8"), "unicode_escape")
    except (UnicodeDecodeError, ValueError):
        return b""
    return decoded.encode("cp1252", "replace")


#: A line carrying this, or the line above one, is exempt. The text after the
#: colon is the reason and is required.
ALLOW = re.compile(r"legal-ok:\s*\S")

CODE_SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".py"}
PROSE_SUFFIXES = {".md", ".html", ".txt"}


def blank(text: str, begin: int, end: int) -> str:
    """Replace a span with spaces, keeping every later offset where it was.

    Line numbers in the report have to point at the source, so nothing may be
    deleted -- only emptied.
    """
    span = text[begin:end]
    return text[:begin] + "".join(c if c == "\n" else " " for c in span) + text[end:]


def strip_comments(text: str, suffix: str) -> str:
    """Blank the comment bodies. A citation in prose is not a fixture."""
    if suffix == ".py":
        # `tokenize` rather than a regex, so that a `#` inside a string literal
        # stays where it is. Blanking one would hide a fixture, which is the
        # failure this whole check exists to prevent.
        import io
        import tokenize

        try:
            for token in tokenize.generate_tokens(io.StringIO(text).readline):
                if token.type == tokenize.COMMENT:
                    begin = sum(len(l) + 1 for l in text.splitlines()[: token.start[0] - 1])
                    text = blank(text, begin + token.start[1], begin + token.end[1])
        except (tokenize.TokenError, IndentationError, SyntaxError):
            pass
        return text
    # C and C++: line comments and block comments. Doing this with a regex
    # would also blank the `//` inside a string literal such as a URL, which
    # costs nothing here -- blanking a literal can only lose a finding in a
    # file that also has a real one, and no fixture is written that way.
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def code_blocks_only(text: str) -> str:
    """Keep a Markdown document's fenced code and blank everything else."""
    kept = []
    fenced = False
    for line in text.splitlines(keepends=True):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            kept.append("\n" if line.endswith("\n") else "")
            continue
        kept.append(line if fenced else ("\n" if line.endswith("\n") else ""))
    return "".join(kept)


def prepare(text: str, suffix: str) -> tuple[str, str]:
    """`(text for raw strings, text for everything else)`."""
    if suffix in PROSE_SUFFIXES:
        fenced = code_blocks_only(text)
        return fenced, fenced
    if suffix in CODE_SUFFIXES:
        return text, strip_comments(text, suffix)
    return text, text


def exempt(lines: list[str], line: int) -> bool:
    """`legal-ok: reason`, anywhere in the paragraph the line belongs to.

    Not just the line above, because a fixture is often one declaration spread
    over twenty lines and a finding can land on any of them; requiring the
    marker on each would mean twenty copies of the same sentence. The paragraph
    ends at a blank line, so the marker cannot leak into the next fixture.
    """
    number = line
    while 1 <= number <= len(lines):
        text = lines[number - 1]
        if ALLOW.search(text):
            return True
        if number != line and not text.strip():
            break
        number -= 1
    return False


def candidates(text: str, stripped: str, minimum: int):
    """Every fixture-shaped run in one file, as `(line, kind, bytes)`.

    Two texts, and the split matters. Raw strings are read from the **original**
    because a shipped `.vs` script is full of `//` comments and blanking them
    would hide the fixture -- the first version of this check lost a 2,138-byte
    retail script exactly that way. Everything else is read from the text whose
    comments have been blanked, so that a quotation in prose does not count.
    """
    for match in RAW.finditer(text):
        body = match.group(2).strip()
        if len(body) >= minimum:
            line = text[: match.start()].count("\n") + 1
            yield line, "raw string", body.encode("cp1252", "replace")
            # Retail documents are CRLF; a fixture that was reflowed on paste
            # still has to be caught.
            yield line, "raw string", body.replace("\n", "\r\n").encode("cp1252", "replace")
    # Both the run and its pieces. The run catches a document written as twenty
    # adjacent literals, none of which is long enough on its own -- which is how
    # `test_player.cpp` held two of them. The pieces catch a table of short rows
    # where only some of the rows are copies.
    for match in LIT_RUN.finditer(stripped):
        line = stripped[: match.start()].count("\n") + 1
        pieces = [piece.group(1) for piece in LIT.finditer(match.group(1))]
        if len(pieces) > 1:
            body = unescape("".join(pieces))
            if len(body) >= minimum:
                yield line, "string literal run", body
    for match in LIT.finditer(stripped):
        body = unescape(match.group(1))
        if len(body) >= minimum:
            yield stripped[: match.start()].count("\n") + 1, "string literal", body
    for match in ARR.finditer(stripped):
        data = bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(0)))
        yield stripped[: match.start()].count("\n") + 1, "byte array", data


def tracked_files() -> list[Path]:
    """Every file in the working tree git would keep, tracked or not.

    ``--others --exclude-standard`` is what makes this check able to refuse a
    violation rather than only report one: a file that has been written but
    not yet ``git add``ed is not tracked, so a plain ``ls-files`` cannot see
    it, and the fixture it carries stays invisible until the commit that
    introduces it has already been made. That is not hypothetical -- it is
    how two retail fixtures reached this tree, a journal and a rank table,
    each of them passing this check on the way in and failing it on the run
    after. Ignored files stay out, which is what excludes ``build*/``.
    """
    out = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    files = []
    seen = set()
    for name in out.stdout.splitlines():
        if name in SKIP or Path(name).suffix.lower() not in SUFFIXES:
            continue
        if name in seen:
            continue
        seen.add(name)
        path = ROOT / name
        # A tracked file that has been deleted in the working tree is listed
        # and cannot be read; that is a deletion, not a fixture.
        if path.is_file():
            files.append(path)
    return files


# ---------------------------------------------------------------------------
# the cache, and the parallel search
# ---------------------------------------------------------------------------

#: Gitignored. One JSON file: the verdicts of every file last scanned.
CACHE = ROOT / ".cache" / "check_fixtures.json"

#: What the corpus blob is built by. An edit to any of them may change what is
#: in the blob, so it discards every verdict along with the checker's own.
BLOB_SOURCES = (
    "tools/check_fixtures.py",
    "tests/corpus.py",
    "src/imperivm/formats/pak.py",
    "src/imperivm/formats/bfhp.py",
    "src/imperivm/formats/lzis.py",
)


def cache_key(game: Path, args: argparse.Namespace) -> str:
    """What every cached verdict depends on besides the file's own bytes.

    The installation by every file's path, size and modification time -- the
    blob is built from all of them, `Saves/` and `Logs/` included -- plus the
    code that builds the blob and judges against it, and the thresholds. Any
    change to any of these and nothing cached is trusted.
    """
    digest = hashlib.sha256()
    digest.update(f"{game.resolve()}\0{args.min}\0{args.min_partial}\0".encode())
    for path in sorted(game.rglob("*")):
        try:
            stat = path.stat()
        except OSError:
            continue
        if path.is_file():
            relative = path.relative_to(game).as_posix()
            digest.update(f"{relative}\0{stat.st_size}\0{stat.st_mtime_ns}\n".encode())
    for name in BLOB_SOURCES:
        try:
            digest.update(ROOT.joinpath(name).read_bytes())
        except OSError:
            digest.update(b"missing " + name.encode())
    return digest.hexdigest()


def load_cache(key: str) -> dict[str, list]:
    try:
        stored = json.loads(CACHE.read_text())
    except (OSError, ValueError):
        return {}
    if not isinstance(stored, dict) or stored.get("key") != key:
        return {}
    files = stored.get("files")
    return files if isinstance(files, dict) else {}


def save_cache(key: str, files: dict[str, list]) -> None:
    """Atomically, so two runs at once leave one of their caches, never half."""
    try:
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        scratch = CACHE.with_suffix(f".{os.getpid()}.tmp")
        scratch.write_text(json.dumps({"key": key, "files": files}))
        os.replace(scratch, CACHE)
    except OSError:
        pass


#: Set in the parent before the pool forks, so the workers share the half
#: gigabyte copy-on-write rather than each receiving it through a pipe.
_BLOB = b""
_TEXT = b""
_MIN_PARTIAL = 0


def verdict(data: bytes) -> tuple[bool, int]:
    """`(a whole copy, the longest partial run)` for one candidate."""
    if data in _BLOB:
        return True, 0
    return False, partial_run(data, _TEXT, _MIN_PARTIAL)


def verdicts(wanted: list[bytes], jobs: int) -> dict[bytes, tuple[bool, int]]:
    """Every candidate judged, across processes where the platform can fork.

    Each judgement is a scan of the whole blob, and `bytes.__contains__`
    holds the GIL, so threads would not help; forked processes do.
    """
    if jobs > 1 and len(wanted) > 1 and "fork" in multiprocessing.get_all_start_methods():
        with multiprocessing.get_context("fork").Pool(jobs) as pool:
            results = pool.map(verdict, wanted, chunksize=max(1, len(wanted) // (jobs * 8)))
    else:
        results = [verdict(data) for data in wanted]
    return dict(zip(wanted, results))


def main() -> int:
    global _BLOB, _TEXT, _MIN_PARTIAL
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=None)
    parser.add_argument("--min", type=int, default=60, help="shortest whole fixture to consider")
    parser.add_argument(
        "--min-partial",
        type=int,
        default=200,
        help="shortest *trimmed* copy to report; see partial_run for why it is higher",
    )
    parser.add_argument("--list", action="store_true", help="print every file scanned")
    parser.add_argument(
        "--no-cache", action="store_true",
        help="judge every file afresh rather than reusing the verdicts of unchanged ones",
    )
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                        help="processes for the search (default: every core)")
    args = parser.parse_args()

    game = args.game or installation()
    if game is None:
        print("no installation found; nothing to compare against -- skipped")
        print("set IMPERIVM_GAME_DIR to run this check")
        return 0

    # **A verdict is cached per file content, and only while nothing it was
    # judged against has changed.** The expensive part is not reading the
    # installation but searching half a gigabyte once per candidate -- about
    # two thousand of them, forty seconds -- and between two runs almost no
    # file in the tree changes. A file whose bytes are new, or any change to
    # the installation or to this checker, is judged in full, so a new
    # fixture is never answered from the cache.
    key = cache_key(game, args)
    cached = {} if args.no_cache else load_cache(key)

    files = tracked_files()
    # `(name, digest)` for every file read, in order, for the report.
    order: list[tuple[str, str]] = []
    # `(lines, digest, [(line, kind, data)])` for each file not cached.
    pending: list[tuple[list[str], str, list[tuple[int, str, bytes]]]] = []
    fresh: dict[str, list] = {}
    queued: set[str] = set()
    for path in files:
        try:
            raw = path.read_bytes()
        except OSError:
            continue
        suffix = path.suffix.lower()
        digest = hashlib.sha256(suffix.encode() + b"\0" + raw).hexdigest()
        order.append((str(path.relative_to(ROOT)), digest))
        if digest in fresh or digest in queued:
            continue
        if digest in cached:
            fresh[digest] = cached[digest]
            continue
        # What `read_text` would have returned: newlines translated.
        source = raw.decode("utf-8", errors="replace").replace("\r\n", "\n").replace("\r", "\n")
        raw_text, stripped = prepare(source, suffix)
        found = list(candidates(raw_text, stripped, args.min))
        pending.append((source.splitlines(), digest, found))
        queued.add(digest)

    wanted = list(dict.fromkeys(
        data for lines, _, found in pending for line, _, data in found
        if not exempt(lines, line)
    ))
    if wanted:
        _BLOB, _TEXT = corpus_bytes(game)
        _MIN_PARTIAL = args.min_partial
        print(f"installation  {game}  ({len(_BLOB):,} bytes unpacked, {len(_TEXT):,} of it text)")
        judged = verdicts(wanted, args.jobs)
    else:
        # Nothing new is fixture-shaped, so there is nothing to unpack the
        # installation for.
        print(f"installation  {game}  (not unpacked: nothing new to compare)")
        judged = {}

    # The same walk as a search one candidate at a time: the first hit on a
    # line is the finding, and an exempt line has none.
    for lines, digest, found in pending:
        rows: list[list] = []
        hit: set[int] = set()
        for line, kind, data in found:
            if line in hit or exempt(lines, line):
                continue
            whole, run = judged[data]
            if whole:
                hit.add(line)
                rows.append([line, kind, len(data)])
            elif run:
                hit.add(line)
                rows.append([line, f"{kind}, {run} of {len(data)} copied", run])
        fresh[digest] = rows

    findings = [
        (name, line, kind, size) for name, digest in order for line, kind, size in fresh[digest]
    ]
    save_cache(key, fresh)

    print(f"scanned       {len(files)} tracked text files "
          f"({len(pending)} judged, {len(order) - len(pending)} unchanged since the last scan)")
    if not findings:
        print("clean         no fixture in the tree is a copy of installation data")
        return 0

    findings.sort(key=lambda row: (-row[3], row[0], row[1]))
    total = sum(row[3] for row in findings)
    print(f"\n{len(findings)} fixture(s) are installation data, {total:,} bytes:\n")
    for name, line, kind, size in findings:
        print(f"  {size:7,}  {name}:{line}  ({kind})")
    print(
        "\ndocs/legal.md rule 1: no game assets in the repository, not as test\n"
        "fixtures. Build the fixture from the format specification -- see\n"
        "tests/synthetic.py -- or move the claim to a corpus test in tests/."
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
