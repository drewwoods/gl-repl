#!/usr/bin/env python3
"""Find high-signal magic-number candidates in project C sources.

The default mode is deliberately conservative: it favors fewer false positives
over exhaustive reporting. It scans tracked .c/.h files, strips comments and
strings, skips tests/vendored files, ignores common identity literals, and
reports numeric literals in high-confidence visual/state tuning contexts.
Use --broad when doing a deeper audit.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Sequence, Tuple


SOURCE_EXTS = {".c", ".h"}
DEFAULT_SKIP_PREFIXES = (
    "build/",
    "third_party/",
    "bench/",
    "tools/",
    "docs/",
    "assets/",
    "packaging/",
)
DEFAULT_SKIP_FILES = {
    "include/miniaudio.h",
}
TEST_PREFIXES = (
    "tests/",
)

HIGH_SIGNAL_RENDER_CALLS = (
    "glPointSize",
    "glLineWidth",
    "glPolygonOffset",
    "glTranslatef",
    "glScalef",
    "glRotatef",
    "glOrtho",
    "gluPerspective",
)

ALPHA_RENDER_CALLS = (
    "scene_clr_a",
    "tg_color4f",
)

COLOR_RENDER_CALLS = (
    "glColor3f",
    "glColor4f",
    "glClearColor",
)

NUMBER_RE = re.compile(
    r"(?<![A-Za-z0-9_])"
    r"[-+]?"
    r"(?:"
    r"0[xX][0-9A-Fa-f]+"
    r"|"
    r"(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?"
    r")"
    r"(?:[fFlLuU]+)?"
    r"(?![A-Za-z0-9_])"
)

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


@dataclass(frozen=True)
class Hit:
    path: Path
    line_no: int
    col_no: int
    literal: str
    reason: str
    line: str


def repo_root() -> Path:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        return Path(out.strip())
    except (subprocess.CalledProcessError, FileNotFoundError):
        return Path.cwd()


def git_tracked_files(root: Path) -> List[Path]:
    try:
        out = subprocess.check_output(
            ["git", "ls-files", "*.c", "*.h"],
            cwd=root,
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []
    return [root / p for p in out.splitlines() if p]


def explicit_files(root: Path, args: Sequence[str]) -> List[Path]:
    files: List[Path] = []
    for raw in args:
        p = Path(raw)
        if not p.is_absolute():
            p = root / p
        if p.is_dir():
            for child in p.rglob("*"):
                if child.is_file() and child.suffix in SOURCE_EXTS:
                    files.append(child)
        elif p.is_file() and p.suffix in SOURCE_EXTS:
            files.append(p)
    return sorted(set(files))


def rel_path(root: Path, path: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def should_scan_path(root: Path, path: Path, include_tests: bool, explicit: bool) -> bool:
    if path.suffix not in SOURCE_EXTS:
        return False
    rel = rel_path(root, path)
    if rel in DEFAULT_SKIP_FILES:
        return False
    if any(rel.startswith(prefix) for prefix in DEFAULT_SKIP_PREFIXES):
        return False
    if not explicit and not include_tests and any(rel.startswith(prefix) for prefix in TEST_PREFIXES):
        return False
    return True


def strip_comments_strings(text: str) -> str:
    """Replace C comments and string/char literal contents with spaces."""
    out: List[str] = []
    i = 0
    state = "code"
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""

        if state == "code":
            if c == "/" and n == "*":
                out.extend("  ")
                i += 2
                state = "block_comment"
                continue
            if c == "/" and n == "/":
                out.extend("  ")
                i += 2
                state = "line_comment"
                continue
            if c == '"':
                out.append(" ")
                i += 1
                state = "string"
                continue
            if c == "'":
                out.append(" ")
                i += 1
                state = "char"
                continue
            out.append(c)
            i += 1
            continue

        if state == "block_comment":
            if c == "\n":
                out.append("\n")
                i += 1
            elif c == "*" and n == "/":
                out.extend("  ")
                i += 2
                state = "code"
            else:
                out.append(" ")
                i += 1
            continue

        if state == "line_comment":
            if c == "\n":
                out.append("\n")
                state = "code"
            else:
                out.append(" ")
            i += 1
            continue

        if state in ("string", "char"):
            if c == "\n":
                out.append("\n")
                state = "code"
                i += 1
            elif c == "\\":
                out.append(" ")
                if i + 1 < len(text):
                    out.append(" ")
                    i += 2
                else:
                    i += 1
            elif (state == "string" and c == '"') or (state == "char" and c == "'"):
                out.append(" ")
                state = "code"
                i += 1
            else:
                out.append(" ")
                i += 1
            continue

    return "".join(out)


def strip_numeric_suffix(literal: str) -> str:
    if literal.lower().startswith(("0x", "+0x", "-0x")):
        return re.sub(r"[uUlL]+$", "", literal)
    return re.sub(r"[fFlLuU]+$", "", literal)


def literal_value(literal: str) -> Optional[float]:
    core = strip_numeric_suffix(literal)
    try:
        if core.lower().startswith(("0x", "+0x", "-0x")):
            return float(int(core, 16))
        return float(core)
    except ValueError:
        return None


def is_common_identity(literal: str) -> bool:
    val = literal_value(literal)
    if val is None:
        return False
    return val in (-1.0, 0.0, 1.0)


def is_conventional_math_literal(literal: str) -> bool:
    val = literal_value(literal)
    if val is None:
        return False
    return val in (90.0, 180.0, 360.0, 720.0)


def is_inside_square_brackets(line: str, start: int, end: int) -> bool:
    last_open = line.rfind("[", 0, start)
    last_close = line.rfind("]", 0, start)
    if last_open <= last_close:
        return False
    next_close = line.find("]", end)
    if next_close < 0:
        return False
    next_semicolon = line.find(";", end)
    return next_semicolon < 0 or next_close < next_semicolon


def is_shift_count(line: str, start: int, end: int) -> bool:
    before = line[max(0, start - 3):start]
    after = line[end:end + 3]
    return "<<" in before or ">>" in before or "<<" in after or ">>" in after


def line_is_preprocessor_continuation(masked_line: str, in_continuation: bool) -> Tuple[bool, bool]:
    stripped = masked_line.lstrip()
    is_preproc = in_continuation or stripped.startswith("#")
    continues = is_preproc and masked_line.rstrip().endswith("\\")
    return is_preproc, continues


def line_starts_definition(line: str) -> bool:
    stripped = line.lstrip()
    if stripped.startswith("#"):
        return True
    if stripped.startswith(("case ", "default:")):
        return True
    if stripped.startswith(("{", "}", ".", "[")):
        return True
    if re.match(r"(?:static\s+)?const\s+", stripped):
        return True
    if re.match(r"(?:static\s+)?(?:const\s+)?[A-Za-z_][A-Za-z0-9_\s\*]*\s+[A-Za-z_][A-Za-z0-9_]*\s*\[\s*\]\s*=", stripped):
        return True
    return False


def looks_like_for_loop(line: str) -> bool:
    return re.search(r"\bfor\s*\(", line) is not None


def identifier_before(line: str, idx: int) -> Optional[str]:
    j = idx - 1
    while j >= 0 and line[j].isspace():
        j -= 1
    end = j + 1
    while j >= 0 and (line[j].isalnum() or line[j] == "_"):
        j -= 1
    if end == j + 1:
        return None
    return line[j + 1:end]


def containing_call_names(line: str, start: int) -> List[str]:
    """Return call names whose open parens contain start, nearest first."""
    names: List[str] = []
    depth = 0
    for i in range(start - 1, -1, -1):
        c = line[i]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                name = identifier_before(line, i)
                if name:
                    names.append(name)
            else:
                depth -= 1
    return names


def classify_literal(line: str, start: int, end: int, broad: bool,
                     include_alpha: bool, include_colors: bool) -> Optional[str]:
    literal = line[start:end]
    if is_common_identity(literal):
        return None
    if not broad and is_conventional_math_literal(literal):
        return None
    if is_inside_square_brackets(line, start, end):
        return None
    if is_shift_count(line, start, end):
        return None

    call_names = containing_call_names(line, start)
    render_calls = list(HIGH_SIGNAL_RENDER_CALLS)
    if include_alpha:
        render_calls.extend(ALPHA_RENDER_CALLS)
    if include_colors:
        render_calls.extend(COLOR_RENDER_CALLS)
    if any(name in render_calls for name in call_names):
        return "render-tuning"

    if not broad:
        return None

    window_start = max(0, start - 2)
    window_end = min(len(line), end + 2)
    window = line[window_start:window_end]
    if "*" in window or "/" in window:
        return "scale-factor"

    if "." in literal or "e" in literal.lower():
        return "float-literal"

    if call_names:
        return "call-arg"

    if broad:
        return "literal"
    return None


def find_hits_in_file(root: Path, path: Path, include_definitions: bool, broad: bool,
                      render_only: bool, include_alpha: bool,
                      include_colors: bool) -> List[Hit]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="ignore")

    masked = strip_comments_strings(text)
    raw_lines = text.splitlines()
    masked_lines = masked.splitlines()
    hits: List[Hit] = []
    in_preproc_cont = False
    enum_depth = 0

    for idx, masked_line in enumerate(masked_lines):
        raw_line = raw_lines[idx] if idx < len(raw_lines) else masked_line
        line_no = idx + 1

        is_preproc, in_preproc_cont = line_is_preprocessor_continuation(
            masked_line, in_preproc_cont
        )
        if is_preproc and not include_definitions:
            continue

        stripped = masked_line.strip()
        starts_enum = re.search(r"\benum\b", masked_line) and "{" in masked_line
        if starts_enum:
            enum_depth += masked_line.count("{") - masked_line.count("}")
            if not include_definitions:
                continue
        elif enum_depth > 0:
            enum_depth += masked_line.count("{") - masked_line.count("}")
            if enum_depth < 0:
                enum_depth = 0
            if not include_definitions:
                continue

        if not include_definitions and line_starts_definition(masked_line):
            continue
        if not broad and looks_like_for_loop(masked_line):
            continue
        if not stripped:
            continue

        for m in NUMBER_RE.finditer(masked_line):
            reason = classify_literal(
                masked_line, m.start(), m.end(), broad,
                include_alpha, include_colors,
            )
            if not reason:
                continue
            if render_only and reason != "render-tuning":
                continue
            hits.append(
                Hit(
                    path=path,
                    line_no=line_no,
                    col_no=m.start() + 1,
                    literal=raw_line[m.start():m.end()],
                    reason=reason,
                    line=raw_line.rstrip(),
                )
            )

    return hits


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Find high-signal magic-number candidates in C sources. "
            "Defaults prefer fewer false positives over exhaustive reporting."
        )
    )
    parser.add_argument(
        "paths",
        nargs="*",
        help="Optional files/directories to scan. Defaults to tracked project .c/.h files.",
    )
    parser.add_argument(
        "--include-tests",
        action="store_true",
        help="Include tests/ when scanning default tracked files.",
    )
    parser.add_argument(
        "--include-definitions",
        action="store_true",
        help="Also report numeric literals in #defines, const definitions, enums, and initializers.",
    )
    parser.add_argument(
        "--broad",
        action="store_true",
        help="Loosen heuristics and report more literal uses.",
    )
    parser.add_argument(
        "--include-alpha",
        action="store_true",
        help="Include scene_clr_a/tg_color4f alpha literals in render tuning hits.",
    )
    parser.add_argument(
        "--include-colors",
        action="store_true",
        help="Include glColor*/glClearColor literals. Noisy in art-heavy files.",
    )
    parser.add_argument(
        "--render-only",
        action="store_true",
        help="Only report literals in known GL/scene visual tuning calls.",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Stop printing after N hits. Summary still shows the full count.",
    )
    parser.add_argument(
        "--no-summary",
        action="store_true",
        help="Do not print the trailing summary.",
    )
    return parser.parse_args(argv)


def print_hits(root: Path, hits: Sequence[Hit], limit: int) -> None:
    shown = 0
    for hit in hits:
        if limit and shown >= limit:
            break
        rel = rel_path(root, hit.path)
        print(f"{rel}:{hit.line_no}:{hit.col_no}: {hit.literal} [{hit.reason}]")
        print(f"    {hit.line.strip()}")
        shown += 1
    if limit and len(hits) > limit:
        print(f"... {len(hits) - limit} more hits hidden by --limit {limit}")


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    root = repo_root()
    os.chdir(root)

    explicit = bool(args.paths)
    files = explicit_files(root, args.paths) if explicit else git_tracked_files(root)
    files = [
        p for p in files
        if should_scan_path(root, p, args.include_tests, explicit)
    ]

    hits: List[Hit] = []
    for path in files:
        hits.extend(
            find_hits_in_file(
                root,
                path,
                include_definitions=args.include_definitions,
                broad=args.broad,
                render_only=args.render_only,
                include_alpha=args.include_alpha,
                include_colors=args.include_colors,
            )
        )

    print_hits(root, hits, args.limit)

    if not args.no_summary:
        reason_counts = {}
        for hit in hits:
            reason_counts[hit.reason] = reason_counts.get(hit.reason, 0) + 1
        counts = ", ".join(f"{k}={reason_counts[k]}" for k in sorted(reason_counts))
        if counts:
            print(f"\n{len(hits)} candidate(s) across {len(files)} file(s): {counts}")
        else:
            print(f"\n0 candidate(s) across {len(files)} file(s)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
