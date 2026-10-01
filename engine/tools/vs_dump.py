#!/usr/bin/env python3
"""Reference side of the `.vs` cross-validation.

Drives the validated Python parser in `src/imperivm/formats/vs_parse.py` over
every script in a pack and prints the same canonical serialisation that
`imcheck vs` prints from the C++ front end. The two outputs are meant to be
byte identical:

    python3 engine/tools/vs_dump.py <data.pak> > /tmp/py.dump
    ./build-core/engine/tools/imcheck vs <data.pak> > /tmp/cxx.dump
    diff /tmp/py.dump /tmp/cxx.dump

The Python AST is the richer of the two: it keeps `for`, compound assignment,
boolean literals, declarator initialisers, the empty statement and the bracket
flavour of a block. The C++ contract in `imperivm/core/script/ast.hpp` has none
of those, so the front end normalises them away. This script applies exactly
the same five rewrites before dumping, which is what keeps the diff a test of
the parser rather than a test of the rewrite:

  * `for (init; cond; step) body` -> `block { init; while (cond) body }`, with
    the step carried on the loop and emitted after the body,
  * `a += b` -> `a = a + b`, target subtree duplicated,
  * `true` / `false` -> integer 1 / 0,
  * `T a = e;` -> `declare T a` followed by `assign a e`,
  * a stray `;` -> nothing, or an empty block where one statement is required.

Unary `+` is likewise dropped: it has no node in the contract.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "src"))

from imperivm.formats import vs_parse as V  # noqa: E402
from imperivm.formats.pak import PackFile  # noqa: E402

INDENT = "  "


def esc(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def first_line(node) -> int:
    """The lowest line in a subtree, which is where its first token is.

    Used only for the expression statements a `for` header contributes, where
    the C++ side takes the line of the statement's first token and the Python
    AST records per-node lines from the operator tokens instead.
    """
    return min(n.line for n in V.walk(node) if isinstance(n, V.Node))


# --------------------------------------------------------------------------
# Expressions
# --------------------------------------------------------------------------


def emit_expr(node, out: list[str], depth: int) -> None:
    pad = INDENT * depth
    if isinstance(node, V.IntLit):
        out.append(f"{pad}int {node.value}")
    elif isinstance(node, V.BoolLit):
        out.append(f"{pad}int {1 if node.value else 0}")
    elif isinstance(node, V.StrLit):
        out.append(f"{pad}str {esc(node.value)}")
    elif isinstance(node, V.Name):
        out.append(f"{pad}name {node.ident}")
    elif isinstance(node, V.This):
        out.append(f"{pad}this")
    elif isinstance(node, V.Unary):
        if node.op == "+":
            emit_expr(node.operand, out, depth)
            return
        out.append(f"{pad}unary {node.op}")
        emit_expr(node.operand, out, depth + 1)
    elif isinstance(node, V.Binary):
        out.append(f"{pad}binary {node.op}")
        emit_expr(node.left, out, depth + 1)
        emit_expr(node.right, out, depth + 1)
    elif isinstance(node, V.Index):
        out.append(f"{pad}index")
        emit_expr(node.target, out, depth + 1)
        emit_expr(node.subscript, out, depth + 1)
    elif isinstance(node, V.Member):
        # A bare member access is a zero-argument call with the parentheses
        # left off; the contract folds the two into one node.
        out.append(f"{pad}method {node.name} 0 1")
        emit_expr(node.target, out, depth + 1)
    elif isinstance(node, V.Call):
        callee = node.callee
        if isinstance(callee, V.Member):
            out.append(f"{pad}method {callee.name} {len(node.args)} 0")
            emit_expr(callee.target, out, depth + 1)
        elif isinstance(callee, V.Name):
            out.append(f"{pad}call {callee.ident} {len(node.args)}")
        else:
            raise AssertionError(f"call of a {type(callee).__name__}")
        for arg in node.args:
            emit_expr(arg, out, depth + 1)
    else:
        raise AssertionError(f"unexpected expression {type(node).__name__}")


# --------------------------------------------------------------------------
# Statements
# --------------------------------------------------------------------------


def count(node) -> int:
    """How many contract statements one source statement expands to."""
    if isinstance(node, V.Empty):
        return 0
    if isinstance(node, V.VarDecl):
        return 1 + sum(1 for d in node.declarators if d.init is not None)
    return 1


def emit_assign(line: int, op: str, target, value, out: list[str], depth: int) -> None:
    out.append(f"{INDENT * depth}assign {line}")
    emit_expr(target, out, depth + 1)
    if op == "=":
        emit_expr(value, out, depth + 1)
    else:
        out.append(f"{INDENT * (depth + 1)}binary {op[0]}")
        emit_expr(target, out, depth + 2)
        emit_expr(value, out, depth + 2)


def emit_expr_slot(node, out: list[str], depth: int) -> None:
    """A `for` header slot: an assignment or a bare expression, as a statement."""
    if isinstance(node, V.Assign):
        emit_assign(node.line, node.op, node.target, node.value, out, depth)
    else:
        out.append(f"{INDENT * depth}expr {first_line(node)}")
        emit_expr(node, out, depth + 1)


def emit_single(node, out: list[str], depth: int) -> None:
    """Exactly one statement, wrapping in a block when the expansion is not."""
    n = count(node)
    if n == 1:
        emit_one(node, out, depth)
        return
    out.append(f"{INDENT * depth}block {node.line} {n}")
    emit_one(node, out, depth + 1)


def emit_one(node, out: list[str], depth: int) -> None:
    pad = INDENT * depth
    if isinstance(node, V.Empty):
        return

    if isinstance(node, V.VarDecl):
        names = " ".join(d.name for d in node.declarators)
        out.append(f"{pad}declare {node.line} {node.type_name} {names}")
        for d in node.declarators:
            if d.init is not None:
                emit_assign(d.line, "=", V.Name(d.line, d.name), d.init, out, depth)
        return

    if isinstance(node, V.Block):
        total = sum(count(c) for c in node.body)
        out.append(f"{pad}block {node.line} {total}")
        for child in node.body:
            emit_one(child, out, depth + 1)
        return

    if isinstance(node, V.If):
        out.append(f"{pad}if {node.line} {1 if node.orelse is not None else 0}")
        emit_expr(node.cond, out, depth + 1)
        emit_single(node.then, out, depth + 1)
        if node.orelse is not None:
            emit_single(node.orelse, out, depth + 1)
        return

    if isinstance(node, V.While):
        out.append(f"{pad}while {node.line} 0")
        emit_expr(node.cond, out, depth + 1)
        emit_single(node.body, out, depth + 1)
        return

    if isinstance(node, V.For):
        out.append(f"{pad}block {node.line} {1 if node.init is None else 2}")
        if node.init is not None:
            emit_expr_slot(node.init, out, depth + 1)
        out.append(f"{INDENT * (depth + 1)}while {node.line} "
                   f"{0 if node.step is None else 1}")
        if node.cond is None:
            out.append(f"{INDENT * (depth + 2)}int 1")
        else:
            emit_expr(node.cond, out, depth + 2)
        emit_single(node.body, out, depth + 2)
        if node.step is not None:
            emit_expr_slot(node.step, out, depth + 2)
        return

    if isinstance(node, V.Break):
        out.append(f"{pad}break {node.line}")
        return
    if isinstance(node, V.Continue):
        out.append(f"{pad}continue {node.line}")
        return
    if isinstance(node, V.Return):
        out.append(f"{pad}return {node.line} {0 if node.value is None else 1}")
        if node.value is not None:
            emit_expr(node.value, out, depth + 1)
        return

    if isinstance(node, V.ExprStmt):
        if isinstance(node.expr, V.Assign):
            a = node.expr
            emit_assign(a.line, a.op, a.target, a.value, out, depth)
        else:
            out.append(f"{pad}expr {node.line}")
            emit_expr(node.expr, out, depth + 1)
        return

    raise AssertionError(f"unexpected statement {type(node).__name__}")


# --------------------------------------------------------------------------
# Whole scripts
# --------------------------------------------------------------------------


def dump_script(name: str, source: str, out: list[str]) -> None:
    out.append(f"=== {name}")
    signature = V.parse_signature(source)
    if signature is None:
        out.append("sig 0 ")
    else:
        return_type, params = signature
        out.append(f"sig 1 {return_type}")
        for p in params:
            out.append(f"param {p.type_name} {1 if p.is_out else 0} {p.name}")

    try:
        script = V.parse(source, name)
    except V.VSSyntaxError:
        # The second entry mode: one bare expression, no `return`, no `;`.
        parser = V.Parser(V.tokenize(source, name), name)
        expression = parser.expression()
        if parser.tok.kind != "EOF":
            raise
        out.append("exprmode 1")
        out.append(f"{INDENT}expr {expression.line}")
        emit_expr(expression, out, 2)
        return

    out.append("exprmode 0")
    for statement in script.body:
        emit_one(statement, out, 1)


# --------------------------------------------------------------------------
# Inline scripts in the XML
# --------------------------------------------------------------------------

_ENTITIES = {"lt": "<", "gt": ">", "amp": "&", "quot": '"', "apos": "'"}


def decode_entities(text: str) -> str:
    """The five predefined entities plus ASCII numeric references.

    Hand-rolled rather than `html.unescape` so that it matches the C++ side
    exactly: a disagreement about HTML5's several hundred named entities would
    be a disagreement about nothing.
    """
    out: list[str] = []
    i = 0
    while i < len(text):
        if text[i] != "&":
            out.append(text[i])
            i += 1
            continue
        end = text.find(";", i)
        if end < 0 or end - i > 10:
            out.append(text[i])
            i += 1
            continue
        name = text[i + 1 : end]
        if name in _ENTITIES:
            out.append(_ENTITIES[name])
        elif name.startswith("#"):
            body = name[2:] if name[1:2] in ("x", "X") else name[1:]
            base = 16 if name[1:2] in ("x", "X") else 10
            try:
                value = int(body, base)
            except ValueError:
                value = -1
            out.append(chr(value) if 0 <= value < 128 else text[i : end + 1])
        else:
            out.append(text[i : end + 1])
        i = end + 1
    return "".join(out)


def looks_like_a_path(value: str) -> bool:
    if len(value) < 4 or not value[-3:].lower() == ".vs":
        return False
    return all(c.isascii() and (c.isalnum() or c in "_ /\\.-") for c in value)


def inline_scripts(text: str):
    """Every `script="..."` attribute holding source rather than a file name."""
    at = 0
    ordinal = 0
    while True:
        found = text.find('script="', at)
        if found < 0:
            return
        is_attribute = found == 0 or text[found - 1] in " \t\n\r"
        begin = found + 8
        end = text.find('"', begin)
        if end < 0:
            return
        at = end + 1
        if not is_attribute:
            continue
        source = decode_entities(text[begin:end])
        if looks_like_a_path(source):
            continue
        yield ordinal, source
        ordinal += 1


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: vs_dump.py <data.pak>", file=sys.stderr)
        return 2

    pack = PackFile(sys.argv[1])
    out: list[str] = []
    scripts = inline_count = 0

    for entry in pack.entries:
        if entry.name.upper().endswith(".VS"):
            scripts += 1
            dump_script(entry.name, pack.read(entry.name).decode("cp1252"), out)

    for entry in pack.entries:
        if not entry.name.upper().endswith(".XML"):
            continue
        text = pack.read(entry.name).decode("cp1252")
        for ordinal, source in inline_scripts(text):
            inline_count += 1
            dump_script(f"{entry.name}#{ordinal}", source, out)

    print("\n".join(out))
    print(f"{sys.argv[1]}: {scripts} scripts, {inline_count} inline", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
