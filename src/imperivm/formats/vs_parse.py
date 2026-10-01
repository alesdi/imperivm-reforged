"""Tokeniser and parser for the HMMSYS `.vs` script language.

Specification: docs/formats/vs-language.md
Host API inventory: docs/formats/vs-host-api.md

Reference implementation: correctness and legibility over speed. The parser is a
hand-written recursive-descent parser producing the dataclass AST below. It is
purely syntactic: it does not resolve names, types, or host functions.

Usage:
    python3 vs_parse.py FILE.vs              # parse one file, dump the AST
    python3 vs_parse.py --pack data.pak      # parse every .vs in a pack
    python3 vs_parse.py --pack data.pak --api  # dump the host API inventory
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

# --------------------------------------------------------------------------
# Lexer
# --------------------------------------------------------------------------

#: Words that may not begin a declaration and are recognised by the parser.
KEYWORDS = frozenset(
    {"if", "else", "while", "for", "break", "continue", "return", "true", "false"}
)

#: Multi-character operators, longest first so the alternation is greedy.
OPERATORS = (
    "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=",
    "+", "-", "*", "/", "%", "<", ">", "=", "!", ".", ",", ";",
    "(", ")", "[", "]", "{", "}",
)

_TOKEN_RE = re.compile(
    r"""
      (?P<SPACE>[ \t\r\n]+)
    | (?P<LINE_COMMENT>//[^\n]*)
    | (?P<BLOCK_COMMENT_OPEN>/\*)
    | (?P<STRING>"(?:[^"\\\n]|\\.)*"|'(?:[^'\\\n]|\\.)*')
    | (?P<NUMBER>\d+)
    | (?P<IDENT>[A-Za-z_][A-Za-z0-9_]*)
    | (?P<OP>"""
    + "|".join(re.escape(op) for op in OPERATORS)
    + r""")
    """,
    re.VERBOSE,
)


class VSSyntaxError(Exception):
    """Raised when a script does not conform to the grammar."""


@dataclass(frozen=True)
class Token:
    """One lexical token. ``kind`` is IDENT, NUMBER, STRING, OP or EOF."""

    kind: str
    text: str
    line: int


def tokenize(source: str, name: str = "<vs>") -> list[Token]:
    """Return the token stream for one script, discarding comments.

    Both ``//`` line comments and ``/* */`` block comments are dropped. Block
    comments do not nest.
    """
    tokens: list[Token] = []
    pos, line, end = 0, 1, len(source)

    while pos < end:
        match = _TOKEN_RE.match(source, pos)
        if match is None:
            raise VSSyntaxError(f"{name}:{line}: unexpected character {source[pos]!r}")
        kind, text = match.lastgroup, match.group()

        if kind == "BLOCK_COMMENT_OPEN":
            close = source.find("*/", pos + 2)
            if close < 0:
                raise VSSyntaxError(f"{name}:{line}: unterminated block comment")
            line += source.count("\n", pos, close)
            pos = close + 2
            continue

        line_start = line
        line += text.count("\n")
        pos = match.end()
        if kind in ("SPACE", "LINE_COMMENT"):
            continue
        tokens.append(Token(kind, text, line_start))

    tokens.append(Token("EOF", "", line))
    return tokens


# --------------------------------------------------------------------------
# AST
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class Node:
    """Base class for every AST node. ``line`` is the 1-based source line."""

    line: int


# -- expressions -----------------------------------------------------------


@dataclass(frozen=True)
class IntLit(Node):
    value: int


@dataclass(frozen=True)
class StrLit(Node):
    value: str


@dataclass(frozen=True)
class BoolLit(Node):
    value: bool


@dataclass(frozen=True)
class Name(Node):
    """A bare identifier: a local, a parameter, a global constant, or a
    zero-argument host function invoked without parentheses."""

    ident: str


@dataclass(frozen=True)
class This(Node):
    """The implicit receiver written as a leading ``.`` (as in ``.health``)."""


@dataclass(frozen=True)
class Member(Node):
    """``obj.name`` — a property read or a zero-argument method call."""

    target: Node
    name: str


@dataclass(frozen=True)
class Call(Node):
    """``callee(args...)``. ``callee`` is a :class:`Name` for a free function
    and a :class:`Member` for a method."""

    callee: Node
    args: tuple[Node, ...]


@dataclass(frozen=True)
class Index(Node):
    """``target[subscript]``."""

    target: Node
    subscript: Node


@dataclass(frozen=True)
class Unary(Node):
    op: str
    operand: Node


@dataclass(frozen=True)
class Binary(Node):
    op: str
    left: Node
    right: Node


@dataclass(frozen=True)
class Assign(Node):
    """``target op value`` where op is ``=``, ``+=``, ``-=``, ``*=``, ``/=`` or ``%=``."""

    op: str
    target: Node
    value: Node


# -- statements ------------------------------------------------------------


@dataclass(frozen=True)
class Declarator(Node):
    name: str
    size: Node | None
    init: Node | None


@dataclass(frozen=True)
class VarDecl(Node):
    type_name: str
    declarators: tuple[Declarator, ...]


@dataclass(frozen=True)
class Block(Node):
    """A statement list. ``bracket`` is ``"{"`` or ``"["`` — the corpus uses the
    two interchangeably; see the specification."""

    bracket: str
    body: tuple[Node, ...]


@dataclass(frozen=True)
class If(Node):
    cond: Node
    then: Node
    orelse: Node | None


@dataclass(frozen=True)
class While(Node):
    cond: Node
    body: Node


@dataclass(frozen=True)
class For(Node):
    init: Node | None
    cond: Node | None
    step: Node | None
    body: Node


@dataclass(frozen=True)
class Break(Node):
    pass


@dataclass(frozen=True)
class Continue(Node):
    pass


@dataclass(frozen=True)
class Return(Node):
    value: Node | None


@dataclass(frozen=True)
class ExprStmt(Node):
    expr: Node


@dataclass(frozen=True)
class Empty(Node):
    """A stray ``;``."""


@dataclass(frozen=True)
class Param:
    """One entry of the signature comment."""

    type_name: str
    name: str
    is_out: bool


@dataclass(frozen=True)
class Script(Node):
    """A whole ``.vs`` file: an optional signature comment plus a statement list."""

    name: str
    return_type: str | None
    params: tuple[Param, ...]
    body: tuple[Node, ...]


# --------------------------------------------------------------------------
# Signature comment
# --------------------------------------------------------------------------

_SIG_PARAM_RE = re.compile(
    r"""^\s*(?P<type>[A-Za-z_]\w*)\s*
         (?:(?P<star>\*)\s*|\s+(?P<out>OUT)\s+)?
         (?P<name>[A-Za-z_]\w*)\s*$""",
    re.VERBOSE,
)


def parse_signature(source: str) -> tuple[str, tuple[Param, ...]] | None:
    """Parse the leading ``//`` signature comment, if present.

    The convention is ``// <return-type>[, <type> [*|OUT] <name>]...`` on the
    first non-blank line, for example ``//int, int idPlayer, GAIKA g, int *pOverneed``.
    Returns ``None`` when the first non-blank line is not a line comment.
    """
    for raw in source.splitlines():
        line = raw.strip()
        if not line:
            continue
        if not line.startswith("//"):
            return None
        fields = [f.strip() for f in line[2:].split(",")]
        params: list[Param] = []
        for f in fields[1:]:
            m = _SIG_PARAM_RE.match(f)
            if m is None:
                return None
            params.append(
                Param(m["type"], m["name"], bool(m["star"] or m["out"]))
            )
        return fields[0], tuple(params)
    return None


# --------------------------------------------------------------------------
# Parser
# --------------------------------------------------------------------------

#: Binary operator precedence, lowest binding first.
_PRECEDENCE: tuple[tuple[str, ...], ...] = (
    ("||",),
    ("&&",),
    ("==", "!="),
    ("<", ">", "<=", ">="),
    ("+", "-"),
    ("*", "/", "%"),
)

_ASSIGN_OPS = frozenset({"=", "+=", "-=", "*=", "/=", "%="})


class Parser:
    """Recursive-descent parser over a token list."""

    def __init__(self, tokens: list[Token], name: str = "<vs>") -> None:
        self.tokens = tokens
        self.name = name
        self.pos = 0

    # -- token helpers ---------------------------------------------------

    @property
    def tok(self) -> Token:
        return self.tokens[self.pos]

    def peek(self, offset: int = 1) -> Token:
        index = min(self.pos + offset, len(self.tokens) - 1)
        return self.tokens[index]

    def at(self, text: str) -> bool:
        return self.tok.text == text and self.tok.kind in ("OP", "IDENT")

    def accept(self, text: str) -> bool:
        if self.at(text):
            self.pos += 1
            return True
        return False

    def expect(self, text: str) -> Token:
        if not self.at(text):
            raise VSSyntaxError(
                f"{self.name}:{self.tok.line}: expected {text!r}, "
                f"found {self.tok.text!r}"
            )
        token = self.tok
        self.pos += 1
        return token

    # -- statements ------------------------------------------------------

    def parse_script(self, name: str, signature) -> Script:
        body = []
        while self.tok.kind != "EOF":
            body.append(self.statement())
        return_type, params = signature if signature else (None, ())
        return Script(1, name, return_type, params, tuple(body))

    def statement(self) -> Node:
        tok = self.tok
        line = tok.line

        if tok.kind == "OP":
            if tok.text in ("{", "["):
                return self.block()
            if tok.text == ";":
                self.pos += 1
                return Empty(line)

        if tok.kind == "IDENT":
            if tok.text == "if":
                return self.if_stmt()
            if tok.text == "while":
                return self.while_stmt()
            if tok.text == "for":
                return self.for_stmt()
            if tok.text == "break":
                self.pos += 1
                self.expect(";")
                return Break(line)
            if tok.text == "continue":
                self.pos += 1
                self.expect(";")
                return Continue(line)
            if tok.text == "return":
                self.pos += 1
                value = None if self.at(";") else self.expression()
                self.expect(";")
                return Return(line, value)
            if self.looks_like_declaration():
                return self.var_decl()

        expr = self.expression()
        self.expect(";")
        return ExprStmt(line, expr)

    def looks_like_declaration(self) -> bool:
        """``IDENT IDENT`` at statement position is always a declaration.

        No expression form in the corpus places two identifiers side by side, so
        this rule needs no table of known type names.
        """
        if self.tok.kind != "IDENT" or self.tok.text in KEYWORDS:
            return False
        return self.peek().kind == "IDENT" and self.peek().text not in KEYWORDS

    def var_decl(self) -> Node:
        line = self.tok.line
        type_name = self.tok.text
        self.pos += 1
        declarators = [self.declarator()]
        while self.accept(","):
            declarators.append(self.declarator())
        self.expect(";")
        return VarDecl(line, type_name, tuple(declarators))

    def declarator(self) -> Declarator:
        tok = self.tok
        if tok.kind != "IDENT":
            raise VSSyntaxError(
                f"{self.name}:{tok.line}: expected a declarator name, found {tok.text!r}"
            )
        self.pos += 1
        size = None
        if self.accept("["):
            size = self.expression()
            self.expect("]")
        init = self.expression() if self.accept("=") else None
        return Declarator(tok.line, tok.text, size, init)

    def block(self) -> Block:
        line = self.tok.line
        opener = self.tok.text
        closer = "}" if opener == "{" else "]"
        self.pos += 1
        body = []
        while not self.at(closer):
            if self.tok.kind == "EOF":
                raise VSSyntaxError(f"{self.name}:{line}: unterminated {opener} block")
            body.append(self.statement())
        self.pos += 1
        return Block(line, opener, tuple(body))

    def if_stmt(self) -> Node:
        line = self.tok.line
        self.pos += 1
        self.expect("(")
        cond = self.expression()
        self.expect(")")
        then = self.statement()
        orelse = self.statement() if self.accept("else") else None
        return If(line, cond, then, orelse)

    def while_stmt(self) -> Node:
        line = self.tok.line
        self.pos += 1
        self.expect("(")
        cond = self.expression()
        self.expect(")")
        return While(line, cond, self.statement())

    def for_stmt(self) -> Node:
        line = self.tok.line
        self.pos += 1
        self.expect("(")
        init = None
        if not self.at(";"):
            init = self.expression()
        self.expect(";")
        cond = None if self.at(";") else self.expression()
        self.expect(";")
        step = None if self.at(")") else self.expression()
        self.expect(")")
        return For(line, init, cond, step, self.statement())

    # -- expressions -----------------------------------------------------

    def expression(self) -> Node:
        left = self.binary(0)
        if self.tok.kind == "OP" and self.tok.text in _ASSIGN_OPS:
            op, line = self.tok.text, self.tok.line
            self.pos += 1
            return Assign(line, op, left, self.expression())
        return left

    def binary(self, level: int) -> Node:
        if level >= len(_PRECEDENCE):
            return self.unary()
        node = self.binary(level + 1)
        while self.tok.kind == "OP" and self.tok.text in _PRECEDENCE[level]:
            op, line = self.tok.text, self.tok.line
            self.pos += 1
            node = Binary(line, op, node, self.binary(level + 1))
        return node

    def unary(self) -> Node:
        tok = self.tok
        if tok.kind == "OP" and tok.text in ("!", "-", "+"):
            self.pos += 1
            return Unary(tok.line, tok.text, self.unary())
        return self.postfix(self.primary())

    def postfix(self, node: Node) -> Node:
        while True:
            tok = self.tok
            if self.accept("."):
                name = self.tok
                if name.kind != "IDENT":
                    raise VSSyntaxError(
                        f"{self.name}:{name.line}: expected a member name after '.'"
                    )
                self.pos += 1
                node = Member(name.line, node, name.text)
            elif self.accept("("):
                args = []
                if not self.at(")"):
                    args.append(self.expression())
                    while self.accept(","):
                        args.append(self.expression())
                self.expect(")")
                node = Call(tok.line, node, tuple(args))
            elif self.accept("["):
                subscript = self.expression()
                self.expect("]")
                node = Index(tok.line, node, subscript)
            else:
                return node

    def primary(self) -> Node:
        tok = self.tok
        if tok.kind == "NUMBER":
            self.pos += 1
            return IntLit(tok.line, int(tok.text))
        if tok.kind == "STRING":
            self.pos += 1
            return StrLit(tok.line, tok.text[1:-1])
        if tok.kind == "IDENT":
            if tok.text in ("true", "false"):
                self.pos += 1
                return BoolLit(tok.line, tok.text == "true")
            if tok.text in KEYWORDS:
                raise VSSyntaxError(
                    f"{self.name}:{tok.line}: keyword {tok.text!r} in expression"
                )
            self.pos += 1
            return Name(tok.line, tok.text)
        if tok.text == "(":
            self.pos += 1
            node = self.expression()
            self.expect(")")
            return node
        if tok.text == ".":
            # Leading dot: a member of the implicit receiver.
            return This(tok.line)
        raise VSSyntaxError(
            f"{self.name}:{tok.line}: unexpected {tok.text!r} in expression"
        )


def parse(source: str, name: str = "<vs>") -> Script:
    """Parse one script source into a :class:`Script`."""
    signature = parse_signature(source)
    return Parser(tokenize(source, name), name).parse_script(name, signature)


# --------------------------------------------------------------------------
# AST walking and the host API inventory
# --------------------------------------------------------------------------


def walk(node) -> Iterator[Node]:
    """Yield every AST node reachable from ``node``, parents before children."""
    if isinstance(node, Node):
        yield node
        for value in vars(node).values():
            yield from walk(value)
    elif isinstance(node, (tuple, list)):
        for item in node:
            yield from walk(item)


#: Arity recorded for a member used without parentheses, e.g. ``u.IsValid``.
NO_PARENS = -1


@dataclass
class Inventory:
    """Statically observed host surface across a corpus of scripts.

    Counts are call sites, not distinct names. ``-1`` in an arity slot means the
    member was written without parentheses; the language treats that as a
    zero-argument call, so ``-1`` and ``0`` describe the same host entry point.
    """

    #: (member name, arity) -> occurrence count
    methods: dict[tuple[str, int], int] = field(default_factory=dict)
    #: (function name, arity) -> occurrence count
    functions: dict[tuple[str, int], int] = field(default_factory=dict)
    #: bare identifier -> occurrence count (global constants, paren-less calls)
    bare_names: dict[str, int] = field(default_factory=dict)
    #: declared type name -> declaration count
    types: dict[str, int] = field(default_factory=dict)
    #: receiver type name -> member name -> occurrence count
    by_receiver: dict[str, dict[str, int]] = field(default_factory=dict)
    #: member or function name -> variable type it was assigned into -> count
    #:
    #: **Keyed by bare name, so a name that is both a free function and a member
    #: pools the two.** Eight shipped names are both (``AIRun``, ``Count``,
    #: ``Dist``, ``Eval``, ``GetGAIKA``, ``GetSquad``, ``PlaySound``,
    #: ``SetNoAIFlag``), and for ``AIRun`` and ``Eval`` the executable's two
    #: registrations disagree -- the free form returns ``int``, the members
    #: return ``void`` -- so the pooled answer is wrong for one of the two. Use
    #: ``kind_return_types`` where the distinction matters; this field is kept
    #: as it was so that existing readers do not change shape.
    return_types: dict[str, dict[str, int]] = field(default_factory=dict)
    #: ("fn" | "member", name) -> variable type it was assigned into -> count.
    #: The same inference, split by call kind.
    kind_return_types: dict[tuple[str, str], dict[str, int]] = field(default_factory=dict)

    @staticmethod
    def _bump(table: dict, key) -> None:
        table[key] = table.get(key, 0) + 1

    def add_script(self, script: Script) -> None:
        """Fold one parsed script into the inventory."""
        # Local environment. Names are case sensitive: many scripts hold both a
        # parameter ``This`` and a differently typed local ``this``.
        env: dict[str, str] = {p.name: p.type_name for p in script.params}
        for node in walk(script):
            if isinstance(node, VarDecl):
                self._bump(self.types, node.type_name)
                for d in node.declarators:
                    env[d.name] = node.type_name

        this_type = env.get("this")
        callee_ids = {id(n.callee) for n in walk(script) if isinstance(n, Call)}

        def receiver_type(target: Node) -> str:
            if isinstance(target, This):
                return this_type or "?"
            if isinstance(target, Name):
                return env.get(target.ident, "?")
            return "(chained)"

        for node in walk(script):
            if isinstance(node, Call):
                callee, arity = node.callee, len(node.args)
                if isinstance(callee, Member):
                    self._bump(self.methods, (callee.name, arity))
                    slot = self.by_receiver.setdefault(receiver_type(callee.target), {})
                    self._bump(slot, callee.name)
                elif isinstance(callee, Name):
                    self._bump(self.functions, (callee.ident, arity))
            elif isinstance(node, Member) and id(node) not in callee_ids:
                self._bump(self.methods, (node.name, NO_PARENS))
                slot = self.by_receiver.setdefault(receiver_type(node.target), {})
                self._bump(slot, node.name)
            elif isinstance(node, Name) and id(node) not in callee_ids:
                if node.ident not in env:
                    self._bump(self.bare_names, node.ident)

        # Return types, inferred from ``<typed local> = <call>``.
        def record(target: Node, value: Node) -> None:
            if not isinstance(target, Name):
                return
            declared = env.get(target.ident)
            if declared is None:
                return
            callee = value.callee if isinstance(value, Call) else value
            if isinstance(callee, Member):
                name, kind = callee.name, "member"
            elif isinstance(callee, Name):
                name, kind = callee.ident, "fn"
            else:
                name = kind = None
            if name is not None:
                self._bump(self.return_types.setdefault(name, {}), declared)
                self._bump(self.kind_return_types.setdefault((kind, name), {}), declared)

        for node in walk(script):
            if isinstance(node, Assign) and node.op == "=":
                if isinstance(node.value, (Call, Member, Name)):
                    record(node.target, node.value)
            elif isinstance(node, VarDecl):
                for d in node.declarators:
                    if isinstance(d.init, (Call, Member)):
                        record(Name(d.line, d.name), d.init)


def dump(node, indent: int = 0) -> str:
    """Render an AST as an indented tree."""
    pad = "  " * indent
    if isinstance(node, Node):
        fields = {k: v for k, v in vars(node).items() if k != "line"}
        scalars = {
            k: v
            for k, v in fields.items()
            if not isinstance(v, (Node, tuple, list)) or v == ()
        }
        children = {k: v for k, v in fields.items() if k not in scalars}
        head = f"{pad}{type(node).__name__}"
        if scalars:
            head += "(" + ", ".join(f"{k}={v!r}" for k, v in scalars.items()) + ")"
        lines = [head]
        for key, value in children.items():
            if value is None:
                continue
            lines.append(f"{pad}  .{key}:")
            lines.append(dump(value, indent + 2))
        return "\n".join(lines)
    if isinstance(node, (tuple, list)):
        return "\n".join(dump(item, indent) for item in node)
    return f"{pad}{node!r}"


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def _iter_pack_scripts(pack_path: Path):
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from pak import PackFile

    pack = PackFile(pack_path)
    for entry in pack.entries:
        if entry.name.upper().endswith(".VS"):
            yield entry.name, pack.read(entry.name).decode("cp1252")


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(description="Parse HMMSYS .vs scripts.")
    parser.add_argument("file", type=Path, nargs="?", help="a single .vs file")
    parser.add_argument("--pack", type=Path, help="parse every .vs in a pack")
    parser.add_argument("--api", action="store_true", help="dump the host API inventory")
    args = parser.parse_args()

    if args.pack:
        inventory = Inventory()
        total = failed = 0
        for name, source in _iter_pack_scripts(args.pack):
            total += 1
            try:
                script = parse(source, name)
            except VSSyntaxError as exc:
                failed += 1
                print(f"FAIL {exc}")
                continue
            inventory.add_script(script)
        ok = total - failed
        rate = 100.0 * ok / total if total else 0.0
        print(f"{ok}/{total} scripts parsed ({rate:.1f}%)")
        if args.api:
            _print_inventory(inventory)
        return

    if not args.file:
        parser.error("give a file or --pack")

    source = args.file.read_bytes().decode("cp1252")
    script = parse(source, args.file.name)
    print(f"// return {script.return_type}")
    for p in script.params:
        print(f"// param {p.type_name}{'*' if p.is_out else ''} {p.name}")
    print(dump(script.body))


def _print_inventory(inv: Inventory) -> None:
    def table(title: str, rows) -> None:
        print(f"\n== {title} ({len(rows)}) ==")
        for key, count in sorted(rows, key=lambda kv: (-kv[1], str(kv[0]))):
            label = key if isinstance(key, str) else f"{key[0]}/{key[1]}"
            print(f"{count:6d}  {label}")

    table("declared types", list(inv.types.items()))
    table("members (name/arity, -1 = written without parentheses)", list(inv.methods.items()))
    table("free functions (name/arity)", list(inv.functions.items()))
    table("bare identifiers", list(inv.bare_names.items()))

    print(f"\n== members by receiver type ({len(inv.by_receiver)}) ==")
    for recv, members in sorted(
        inv.by_receiver.items(), key=lambda kv: -sum(kv[1].values())
    ):
        names = sorted(members.items(), key=lambda kv: (-kv[1], kv[0]))
        print(f"  {recv} ({sum(members.values())} uses, {len(members)} distinct)")
        print("    " + ", ".join(f"{n}({c})" for n, c in names))

    print(f"\n== inferred return types ({len(inv.kind_return_types)}) ==")
    for (kind, name), kinds in sorted(inv.kind_return_types.items(), key=lambda kv: kv[0][::-1]):
        best = sorted(kinds.items(), key=lambda kv: (-kv[1], kv[0]))
        prefix = "." if kind == "member" else ""
        print(f"  {prefix}{name}: " + ", ".join(f"{t}({c})" for t, c in best))


if __name__ == "__main__":
    main()
