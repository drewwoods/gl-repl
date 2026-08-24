#!/usr/bin/env python3
"""Format OpenGL REPL scene files (.glr) using typical C formatting with indentation of 2,
plus custom rules for the commands that open a scope without a brace.

The rules mirror the app's own canonical indentation (`source_scope.c`), which
sums four independent depths - braces (for/func/if), glBegin/glEnd,
gluBegin/gluEnd and glPushMatrix/glPopMatrix - so a body written by
`gl-repl --export-glr` is already formatted by this script's definition. The one
asymmetry the app has is reproduced here: glu (tessellator) commands belong to
the tessellator scope, not the GL vertex block, so glBegin depth is excluded
from their indent (`repl_source_scope_view_cmd_tess_indent()`).

Every scene carries an explicit `display() { ... }` wrapper. The wrapper is
format syntax consumed by loaders, but the authored file uses the same C-like
two-space base indentation for camera and body rows that the loader re-derives
in memory.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Scope openers/closers per depth kind. Braces cover for/func/if blocks; the
# rest are commands that open a scope with no brace of their own. A closer
# lands at its opener's level (unindent before the line, like `}`).
BRACE, BEGIN, TESS, MATRIX = 0, 1, 2, 3
NUM_DEPTHS = 4

SCOPE_OPENERS = (("{", BRACE), ("glBegin", BEGIN), ("gluBegin", TESS),
                 ("glPushMatrix", MATRIX))
SCOPE_CLOSERS = (("}", BRACE), ("glEnd", BEGIN), ("gluEnd", TESS),
                 ("glPopMatrix", MATRIX))


def word_at(line: str, i: int, word: str) -> bool:
    """True when `word` occurs at index `i` as a whole identifier."""
    if not line.startswith(word, i):
        return False
    if not word[0].isalpha():          # punctuation ('{', '}') needs no boundary
        return True
    if i > 0 and (line[i - 1].isalnum() or line[i - 1] == "_"):
        return False
    after = i + len(word)
    return after >= len(line) or not (line[after].isalnum() or line[after] == "_")


def line_counts_begin_depth(stripped: str) -> bool:
    """False for the lines the app indents without the glBegin depth: the
    glBegin/glEnd pair itself, and every glu* (tessellator) command."""
    if stripped.startswith("glBegin") or stripped.startswith("glEnd"):
        return False
    return not (stripped.startswith("glu") and not stripped.startswith("glut"))


def scan_line(line: str, in_block_comment: bool) -> tuple[list[int], list[int], bool]:
    """Scans a line of code to compute indentation changes, per depth kind.

    Returns:
        (unindents_before, indents_after, in_block_comment) - the first two
        indexed by BRACE/BEGIN/TESS/MATRIX.
    """
    active_opens = [0] * NUM_DEPTHS
    unindents_before = [0] * NUM_DEPTHS
    in_string = False
    escape = False

    i = 0
    n = len(line)
    while i < n:
        if in_block_comment:
            if i + 1 < n and line[i] == "*" and line[i + 1] == "/":
                in_block_comment = False
                i += 2
            else:
                i += 1
            continue

        if in_string:
            if escape:
                escape = False
            elif line[i] == "\\":
                escape = True
            elif line[i] == '"':
                in_string = False
            i += 1
            continue

        # Check for block comment start
        if line[i] == "/" and i + 1 < n and line[i + 1] == "*":
            in_block_comment = True
            i += 2
            continue

        # Check for line comment start
        if line[i] == "/" and i + 1 < n and line[i + 1] == "/":
            break

        # Check for string literal start
        if line[i] == '"':
            in_string = True
            escape = False
            i += 1
            continue

        # Scope openers and closers, brace and brace-less alike
        matched = 0
        for word, kind in SCOPE_OPENERS:
            if word_at(line, i, word):
                active_opens[kind] += 1
                i += len(word)
                matched = 1
                break
        if matched:
            continue
        for word, kind in SCOPE_CLOSERS:
            if word_at(line, i, word):
                if active_opens[kind] > 0:
                    active_opens[kind] -= 1
                else:
                    unindents_before[kind] += 1
                i += len(word)
                matched = 1
                break
        if matched:
            continue

        i += 1

    return unindents_before, active_opens, in_block_comment


def format_content(content: str) -> str:
    """Formats the contents of a .glr file using C 2-space indentation rules plus
    the brace-less scopes (glBegin, gluBegin, glPushMatrix)."""
    lines = content.splitlines()
    formatted_lines: list[str] = []
    depths = [0] * NUM_DEPTHS
    in_block_comment = False
    in_display = False

    for line in lines:
        stripped = line.strip()

        # Handle blank lines
        if not stripped:
            formatted_lines.append("")
            continue

        # The explicit scene frame is .glr syntax rather than a document row,
        # but it contributes one authored indentation level. Only the exact
        # top-level spelling is recognized, matching doc_order.c.
        if (not in_block_comment and not in_display and
                not any(depths) and stripped == "display() {"):
            formatted_lines.append(stripped)
            in_display = True
            continue
        if (not in_block_comment and in_display and
                not any(depths) and stripped == "}"):
            formatted_lines.append(stripped)
            in_display = False
            continue

        # Identify special comments that must not be indented
        camera_payload = "".join(
            char.lower()
            for char in stripped[2:]
            if char.isascii() and char.isalpha()
        ) if stripped.startswith("//") else ""
        is_special_comment = (
            stripped.startswith("// @cfg")
            or camera_payload == "camera"
            or stripped == "//"
        )

        if is_special_comment:
            formatted_lines.append("  " * int(in_display) + stripped)
            continue

        # Scan line for indent structure
        unindents_before, indents_after, in_block_comment = scan_line(
            line, in_block_comment
        )

        # Apply unindent before formatting this line
        for kind in range(NUM_DEPTHS):
            depths[kind] = max(0, depths[kind] - unindents_before[kind])

        # Format and append - the sum of the depths this line participates in
        level = int(in_display) + depths[BRACE] + depths[TESS] + depths[MATRIX]
        if line_counts_begin_depth(stripped):
            level += depths[BEGIN]
        formatted_lines.append("  " * level + stripped)

        # Apply indent after formatting this line
        for kind in range(NUM_DEPTHS):
            depths[kind] = max(0, depths[kind] + indents_after[kind])

    # Ensure a trailing newline
    return "\n".join(formatted_lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Format OpenGL REPL .glr scene files."
    )
    parser.add_argument(
        "--write", "-w", action="store_true", help="Write formatted changes in-place"
    )
    parser.add_argument(
        "--check",
        "-c",
        action="store_true",
        help="Check formatting and exit with error if any files are unformatted",
    )
    parser.add_argument(
        "files",
        nargs="*",
        type=str,
        help="Paths to files to format (defaults to examples/scenes/*.glr)",
    )

    args = parser.parse_args()

    # Determine files to process
    if args.files:
        files = [Path(p) for p in args.files]
    else:
        root = Path(__file__).resolve().parents[1]
        scenes_dir = root / "examples" / "scenes"
        files = sorted(scenes_dir.glob("*.glr"))

    if not files:
        print("No files found to format.", file=sys.stderr)
        return 0

    has_diffs = False
    for file_path in files:
        if not file_path.is_file():
            print(f"Skipping {file_path}: not a file", file=sys.stderr)
            continue

        try:
            orig_content = file_path.read_text(encoding="utf-8")
        except OSError as exc:
            print(f"Error reading {file_path}: {exc}", file=sys.stderr)
            return 1

        formatted = format_content(orig_content)

        if orig_content != formatted:
            has_diffs = True
            if args.write:
                try:
                    file_path.write_text(formatted, encoding="utf-8")
                    print(f"Formatted {file_path.name}")
                except OSError as exc:
                    print(f"Error writing {file_path}: {exc}", file=sys.stderr)
                    return 1
            elif args.check:
                print(f"File needs formatting: {file_path}")
            else:
                # If neither write nor check, just print names of files that would be formatted
                # unless a single file is explicitly requested, in which case output to stdout.
                if len(files) == 1:
                    sys.stdout.write(formatted)
                else:
                    print(f"Would format {file_path.name}")

    if args.check and has_diffs:
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
