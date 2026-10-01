"""Reader for HMMSYS configuration files (``.ini``).

Specification: docs/formats/ini.md

Reference implementation: correctness and legibility over speed.

The dialect is narrower than "INI" in general, and narrow in ways that matter:
``;`` starts a comment unconditionally, a section header may carry one, and a
section may be an *ordered list* of bare lines rather than a map. See the
specification for the measurements behind each of those.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

#: The files are Windows-1252. Decoding is the caller's business in the C++
#: reader, which is byte-oriented; here it is done once, on read, because a
#: Python caller has nothing to gain from bytes.
ENCODING = "cp1252"


class IniError(Exception):
    """Raised when a file does not conform to the INI dialect."""


@dataclass(frozen=True)
class Entry:
    """One line of content.

    ``key`` is ``None`` for a bare line -- ``[SquadStates]`` in ``AI.INI`` is an
    ordered enum declaration, not a map, and 3,278 lines in the retail corpus
    are of that shape. ``key`` being ``""`` would not do: ``Name`` and ``Name =``
    mean different things and only the file can say which it is.
    """

    key: str | None
    value: str
    line: int

    @property
    def is_bare(self) -> bool:
        return self.key is None


@dataclass
class Section:
    """One ``[Section]``, with its entries in file order."""

    name: str
    line: int
    entries: list[Entry] = field(default_factory=list)

    def get(self, key: str, default: str | None = None) -> str | None:
        """The value of ``key``, or ``default``.

        The **first** match wins, which is what Win32's
        ``GetPrivateProfileString`` does. 22 keys in the retail corpus are
        declared twice within their own section and nine carry different values,
        so the rule is load-bearing; see the specification, where it is recorded
        as inferred from the platform rather than proven from the data.

        A section is not reliably a map at all: ``UNITICONS.INI``'s
        ``[FillCombo]`` gives one label to three different bitmaps. Every entry
        is preserved, and a consumer of a list-shaped section should walk
        ``entries`` rather than call this.

        Bare lines never answer a lookup by key, or an enum member would shadow
        a variable of the same name.
        """
        for entry in self.entries:
            if entry.key is not None and entry.key.lower() == key.lower():
                return entry.value
        return default

    def get_int(self, key: str, default: int | None = None) -> int | None:
        """``get`` parsed as a whole signed decimal, or ``default``.

        A value that is not *entirely* a number gives ``default`` rather than
        the prefix that could be parsed. Reading one half-way is how
        ``ProductionInterval`` came to be recorded as 20 when it is 2000.
        """
        text = self.get(key)
        if text is None:
            return default
        try:
            return int(text, 10)
        except ValueError:
            return default

    @property
    def names(self) -> list[str]:
        """The bare lines, in file order. Their positions are their meaning."""
        return [entry.value for entry in self.entries if entry.is_bare]


@dataclass
class IniFile:
    """A parsed file."""

    sections: list[Section] = field(default_factory=list)

    def section(self, name: str) -> Section | None:
        """The first section of this name, matched without regard to case."""
        for section in self.sections:
            if section.name.lower() == name.lower():
                return section
        return None

    def __iter__(self):
        return iter(self.sections)


def split_list(value: str) -> list[str]:
    """Split a comma-separated value; 8,191 lines in the corpus carry one.

    Elements are stripped and empties preserved: a positional list with a hole
    means something different from a shorter list.

    Values are returned with any ``%Name%`` placeholders intact -- 4,329 of them
    occur across 132 files -- because expanding one needs a scope the file alone
    does not define. See the specification.
    """
    if not value:
        return []
    return [element.strip() for element in value.split(",")]


def parse(data: bytes | str) -> IniFile:
    """Parse one file.

    The only rejection is an unterminated section header -- a ``[`` with no
    ``]`` on the same line. Guessing where the name ends would make that
    failure silent. Every one of the 150 shipped files parses.
    """
    text = data.decode(ENCODING) if isinstance(data, bytes) else data
    out = IniFile()

    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith(";"):
            continue

        if line.startswith("["):
            close = line.find("]")
            if close == -1:
                raise IniError(f"line {number}: unterminated section header: {line!r}")
            out.sections.append(Section(name=line[1:close].strip(), line=number))
            continue

        # `;` unconditionally: no value in the corpus is quoted and no `;` is
        # ever data. All 51 lines containing a double quote have it after the
        # `;`, inside prose.
        content = line.split(";", 1)[0].strip()
        if not content:
            continue

        if not out.sections:
            # No shipped file has an entry before the first header. Dropping
            # such lines would be the wrong way to find out that a modded one
            # does.
            out.sections.append(Section(name="", line=number))

        if "=" in content:
            key, _, value = content.partition("=")
            entry = Entry(key=key.strip(), value=value.strip(), line=number)
        else:
            entry = Entry(key=None, value=content, line=number)
        out.sections[-1].entries.append(entry)

    return out


def read(path: str | Path) -> IniFile:
    """Parse the file at ``path``."""
    return parse(Path(path).read_bytes())
