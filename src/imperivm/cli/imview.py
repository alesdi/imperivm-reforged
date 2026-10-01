"""`imview` — build and serve the browser-based asset viewer.

The viewer reads an *export directory*: a `manifest.json` written by
:mod:`imperivm.manifest` plus the PNGs it names. Nothing else is required, and
nothing about the original installation is needed to look at an export.

Two subcommands:

``imview build <export-dir> [--out FILE]``
    Write one self-contained HTML file. The viewer's HTML, CSS and JavaScript
    and the whole manifest are inlined into it; the PNGs are referenced
    relative to the file by default, or inlined as ``data:`` URIs with
    ``--inline``. Inlining a full export is enormous, so ``--inline`` is meant
    to be used with ``--filter``, and the build warns past 50 MB.

``imview serve <export-dir> [--port N]``
    Serve the same page plus the export over :mod:`http.server`, which is how
    you look at a full export: the browser fetches PNGs on demand and can read
    their bytes, which the viewer needs for team-colour remapping.

Standard library only, no build step, no third-party JavaScript.
"""

from __future__ import annotations

import argparse
import base64
import fnmatch
import json
import os
import sys
import webbrowser
from dataclasses import asdict
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from imperivm.manifest import Manifest

#: Files that make up the front end, in the order the page loads them.
VIEWER_FILES = ("index.html", "viewer.css", "pngdec.js", "tables.js", "viewer.js")

#: Anything past this is too big to be a sensible one-file artefact.
INLINE_WARN_BYTES = 50 * 1024 * 1024

DATA_SCRIPT_OPEN = '<script id="imview-data" type="application/json">'
DATA_SCRIPT_CLOSE = "</script>"


class ViewerError(Exception):
    """Raised when the viewer cannot be assembled."""


# --------------------------------------------------------------- locating


def find_viewer_dir(explicit: str | os.PathLike[str] | None = None) -> Path:
    """Locate the `viewer/` source directory.

    It lives at the repository root rather than inside the package, so a
    source checkout or an editable install finds it relative to this module.
    `--viewer-dir` and `IMVIEW_VIEWER_DIR` override the search.
    """
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    env = os.environ.get("IMVIEW_VIEWER_DIR")
    if env:
        candidates.append(Path(env))
    here = Path(__file__).resolve()
    candidates += [
        here.parents[3] / "viewer",  # repository root of a source checkout
        here.parent / "viewer",
        Path.cwd() / "viewer",
    ]
    for candidate in candidates:
        if all((candidate / name).is_file() for name in VIEWER_FILES):
            return candidate
    searched = ", ".join(str(c) for c in candidates)
    raise ViewerError(
        "could not find the viewer/ directory (looked in: "
        + searched
        + "). Pass --viewer-dir or set IMVIEW_VIEWER_DIR."
    )


# ---------------------------------------------------------------- filtering


def manifest_paths(manifest: Manifest) -> list[str]:
    """Every asset path the manifest references, in section order."""
    return (
        [s.path for s in manifest.sprites]
        + [t.path for t in manifest.terrain]
        + [f.path for f in manifest.fonts]
        + [m.path for m in manifest.masks]
    )


def filter_manifest(manifest: Manifest, patterns: list[str]) -> Manifest:
    """Keep only the entries whose path matches one of `patterns`.

    A sprite's `_shadow` partner is pulled in with it, because a body without
    its shadow is a different picture.
    """
    if not patterns:
        return manifest

    def keep(path: str) -> bool:
        return any(fnmatch.fnmatch(path, pat) for pat in patterns)

    sprites = [s for s in manifest.sprites if keep(s.path)]
    wanted = {s.path for s in sprites}
    for sprite in list(sprites):
        partner = sprite.path[:-4] + "_shadow.png" if sprite.path.endswith(".png") else None
        if partner and partner not in wanted:
            for candidate in manifest.sprites:
                if candidate.path == partner:
                    sprites.append(candidate)
                    wanted.add(partner)
                    break

    return Manifest(
        version=manifest.version,
        game=manifest.game,
        tool=manifest.tool,
        sprites=sprites,
        terrain=[t for t in manifest.terrain if keep(t.path)],
        fonts=[f for f in manifest.fonts if keep(f.path)],
        masks=[m for m in manifest.masks if keep(m.path)],
    )


# ------------------------------------------------------------------ build


def _escape_for_html(text: str) -> str:
    """Keep embedded text from terminating the script element that holds it."""
    return text.replace("</script", "<\\/script").replace("<!--", "<\\!--")


def _json_for_html(value: object) -> str:
    return (
        json.dumps(value, separators=(",", ":"))
        .replace("<", "\\u003c")
        .replace(">", "\\u003e")
        .replace("&", "\\u0026")
    )


def _data_uri(path: Path) -> str:
    return "data:image/png;base64," + base64.b64encode(path.read_bytes()).decode("ascii")


def collect_inline_assets(
    export_dir: Path, manifest: Manifest, *, warn_bytes: int = INLINE_WARN_BYTES
) -> tuple[dict[str, str], int, list[str]]:
    """Read every asset in `manifest` as a data URI.

    Returns the mapping, the encoded byte total, and the paths that were
    missing on disk.
    """
    assets: dict[str, str] = {}
    missing: list[str] = []
    total = 0
    for path in manifest_paths(manifest):
        source = export_dir / path
        if not source.is_file():
            missing.append(path)
            continue
        uri = _data_uri(source)
        assets[path] = uri
        total += len(uri)
    return assets, total, missing


#: Serialising a retail-sized manifest costs about half a second, which is paid
#: on every reload while someone is editing `viewer/`. Key the result on the
#: file's identity so an export that changes on disk is still picked up.
_MANIFEST_JSON_CACHE: dict[tuple, str] = {}


def manifest_json_cached(export_dir: Path, manifest: Manifest, patterns: list[str]) -> str:
    """`manifest` as embeddable JSON, reusing the last result when unchanged."""
    source = export_dir / "manifest.json"
    try:
        stat = source.stat()
        key = (str(source), stat.st_mtime_ns, stat.st_size, tuple(patterns))
    except OSError:
        return _json_for_html(asdict(manifest))
    cached = _MANIFEST_JSON_CACHE.get(key)
    if cached is None:
        cached = _json_for_html(asdict(manifest))
        _MANIFEST_JSON_CACHE.clear()  # one export at a time; keep it bounded
        _MANIFEST_JSON_CACHE[key] = cached
    return cached


def build_html(
    export_dir: Path,
    manifest: Manifest,
    *,
    viewer_dir: Path,
    asset_base: str = ".",
    inline_assets: dict[str, str] | None = None,
    generated: str = "",
    manifest_json: str | None = None,
) -> str:
    """Assemble the single-file viewer page."""
    parts = {name: (viewer_dir / name).read_text(encoding="utf-8") for name in VIEWER_FILES}
    html = parts["index.html"]

    css_tag = '<link rel="stylesheet" href="viewer.css">'
    if css_tag not in html:
        raise ViewerError("viewer/index.html no longer contains the stylesheet link")
    html = html.replace(css_tag, "<style>\n" + parts["viewer.css"] + "\n</style>")

    for name in ("pngdec.js", "tables.js", "viewer.js"):
        tag = f'<script src="{name}"></script>'
        if tag not in html:
            raise ViewerError(f"viewer/index.html no longer loads {name}")
        html = html.replace(tag, "<script>\n" + _escape_for_html(parts[name]) + "\n</script>")

    # The manifest dominates the payload, so it is spliced in as pre-serialised
    # text rather than round-tripped through the envelope dict again.
    manifest_part = manifest_json if manifest_json is not None else _json_for_html(asdict(manifest))
    envelope = _json_for_html(
        {
            "assetBase": asset_base,
            "inlineAssets": inline_assets or {},
            "exportPath": str(export_dir),
            "generated": generated,
        }
    )
    boot = '{"manifest":' + manifest_part + "," + envelope[1:]

    start = html.index(DATA_SCRIPT_OPEN) + len(DATA_SCRIPT_OPEN)
    end = html.index(DATA_SCRIPT_CLOSE, start)
    return html[:start] + boot + html[end:]


def relative_asset_base(out_file: Path, export_dir: Path) -> str:
    """The path from the built page to the export, for `<img src>`."""
    try:
        rel = os.path.relpath(export_dir.resolve(), out_file.resolve().parent)
    except ValueError:  # different drives on Windows
        return export_dir.resolve().as_uri()
    rel = rel.replace(os.sep, "/")
    return "." if rel == "." else rel


def check_assets(export_dir: Path, manifest: Manifest) -> list[str]:
    """Paths named by the manifest that are not on disk."""
    return [p for p in manifest_paths(manifest) if not (export_dir / p).is_file()]


def cmd_build(args: argparse.Namespace) -> int:
    export_dir = Path(args.export_dir).resolve()
    viewer_dir = find_viewer_dir(args.viewer_dir)
    manifest = Manifest.read(export_dir)
    manifest = filter_manifest(manifest, args.filter or [])

    counts = (
        f"{len(manifest.sprites)} sprites, {len(manifest.terrain)} terrain, "
        f"{len(manifest.fonts)} fonts, {len(manifest.masks)} masks"
    )
    print(f"{export_dir}: {counts}", file=sys.stderr)

    out = Path(args.out) if args.out else export_dir / "viewer.html"
    out.parent.mkdir(parents=True, exist_ok=True)

    inline_assets: dict[str, str] | None = None
    if args.inline:
        inline_assets, total, missing = collect_inline_assets(export_dir, manifest)
        print(
            f"inlined {len(inline_assets)} assets, {total / 1e6:.1f} MB of base64",
            file=sys.stderr,
        )
        if missing:
            print(f"warning: {len(missing)} manifest assets are missing on disk", file=sys.stderr)
            for path in missing[:10]:
                print(f"  missing: {path}", file=sys.stderr)
        if total > args.max_inline_bytes:
            print(
                f"warning: the inlined payload is {total / 1e6:.1f} MB, over the "
                f"{args.max_inline_bytes / 1e6:.0f} MB guideline. Narrow it with --filter, "
                "or drop --inline and serve the export instead.",
                file=sys.stderr,
            )
    else:
        missing = check_assets(export_dir, manifest)
        if missing:
            print(
                f"warning: {len(missing)} manifest assets are missing on disk "
                "(the viewer will show load errors for them)",
                file=sys.stderr,
            )

    asset_base = "." if args.inline else relative_asset_base(out, export_dir)
    html = build_html(
        export_dir,
        manifest,
        viewer_dir=viewer_dir,
        asset_base=asset_base,
        inline_assets=inline_assets,
        generated=_timestamp(),
    )
    out.write_text(html, encoding="utf-8")
    print(f"wrote {out} ({len(html.encode('utf-8')) / 1e6:.2f} MB)", file=sys.stderr)
    if not args.inline and asset_base != ".":
        print(f"assets are referenced relative to the page as {asset_base}/", file=sys.stderr)
    return 0


def _timestamp() -> str:
    from datetime import datetime, timezone

    return datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")


# ------------------------------------------------------------------ serve


class ViewerRequestHandler(SimpleHTTPRequestHandler):
    """Serves the generated page at `/` and the export everywhere else.

    The page is rebuilt per request so that editing `viewer/` and reloading is
    enough to see the change.
    """

    export_dir: Path
    viewer_dir: Path
    manifest_filter: list[str]

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, directory=str(self.export_dir), **kwargs)

    def do_GET(self) -> None:  # noqa: N802 - http.server's naming
        if self.path.split("?")[0] in ("/", "/index.html", "/viewer.html"):
            self._send_viewer()
            return
        super().do_GET()

    def _send_viewer(self) -> None:
        try:
            manifest = filter_manifest(Manifest.read(self.export_dir), self.manifest_filter)
            html = build_html(
                self.export_dir,
                manifest,
                viewer_dir=self.viewer_dir,
                asset_base=".",
                generated=_timestamp(),
                manifest_json=manifest_json_cached(
                    self.export_dir, manifest, self.manifest_filter
                ),
            ).encode("utf-8")
        except Exception as exc:  # a broken export should show up in the browser
            body = f"<pre>imview could not build the viewer:\n\n{exc}</pre>".encode("utf-8")
            self.send_response(500)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(html)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(html)

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("  %s\n" % (fmt % args))


def cmd_serve(args: argparse.Namespace) -> int:
    export_dir = Path(args.export_dir).resolve()
    viewer_dir = find_viewer_dir(args.viewer_dir)
    manifest = Manifest.read(export_dir)  # fail early on a broken export
    print(
        f"{export_dir}: {len(manifest.sprites)} sprites, {len(manifest.terrain)} terrain, "
        f"{len(manifest.fonts)} fonts, {len(manifest.masks)} masks",
        file=sys.stderr,
    )

    handler = type(
        "BoundViewerRequestHandler",
        (ViewerRequestHandler,),
        {
            "export_dir": export_dir,
            "viewer_dir": viewer_dir,
            "manifest_filter": args.filter or [],
        },
    )

    try:
        server = ThreadingHTTPServer((args.host, args.port), handler)
    except OSError as exc:
        print(f"imview: cannot bind {args.host}:{args.port}: {exc}", file=sys.stderr)
        return 1

    host = args.host if args.host not in ("", "0.0.0.0") else "127.0.0.1"
    url = f"http://{host}:{server.server_address[1]}/"
    print(f"serving the viewer at {url}  (ctrl-c to stop)", file=sys.stderr)
    if args.open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("", file=sys.stderr)
    finally:
        server.server_close()
    return 0


# -------------------------------------------------------------------- cli


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="imview",
        description="Build or serve the Imperivm Reforged asset viewer.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("export_dir", help="directory holding manifest.json and the PNGs")
    common.add_argument(
        "--filter",
        action="append",
        metavar="GLOB",
        help="keep only manifest paths matching this glob; repeatable",
    )
    common.add_argument(
        "--viewer-dir",
        help="where the viewer/ front end lives (default: found next to the repository)",
    )

    p_build = sub.add_parser("build", parents=[common], help="write a self-contained HTML file")
    p_build.add_argument("--out", "-o", metavar="FILE", help="output path (default: <export-dir>/viewer.html)")
    p_build.add_argument(
        "--inline",
        action="store_true",
        help="embed the PNGs as data URIs so the file is portable on its own",
    )
    p_build.add_argument(
        "--max-inline-bytes",
        type=int,
        default=INLINE_WARN_BYTES,
        help=f"warn above this inlined size (default {INLINE_WARN_BYTES // (1024 * 1024)} MB)",
    )
    p_build.set_defaults(func=cmd_build)

    p_serve = sub.add_parser("serve", parents=[common], help="serve the viewer and the export")
    p_serve.add_argument("--port", "-p", type=int, default=8765, help="port (default 8765, 0 picks a free one)")
    p_serve.add_argument("--host", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    p_serve.add_argument("--open", action="store_true", help="open a browser at the served page")
    p_serve.set_defaults(func=cmd_serve)

    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (ViewerError, FileNotFoundError, ValueError) as exc:
        print(f"imview: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
