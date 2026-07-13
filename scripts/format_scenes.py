#!/usr/bin/env python3
"""Format OpenGL REPL scene files (.glr) using typical C formatting with indentation of 2,
plus custom rules where glBegin indents and glEnd unindents.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def scan_line(line: str, in_block_comment: bool) -> tuple[int, int, bool]:
    """Scans a line of code to compute indentation changes.

    Returns:
        (unindents_before, indents_after, in_block_comment)
    """
    active_opens = 0
    unindents_before = 0
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

        char = line[i]
        if char == "{":
            active_opens += 1
            i += 1
            continue
        elif char == "}":
            if active_opens > 0:
                active_opens -= 1
            else:
                unindents_before += 1
            i += 1
            continue

        # Check for glBegin / glEnd words
        if line[i:].startswith("glBegin"):
            before_ok = i == 0 or not (line[i - 1].isalnum() or line[i - 1] == "_")
            after_idx = i + len("glBegin")
            after_ok = after_idx >= n or not (
                line[after_idx].isalnum() or line[after_idx] == "_"
            )
            if before_ok and after_ok:
                active_opens += 1
                i += len("glBegin")
                continue
        elif line[i:].startswith("glEnd"):
            before_ok = i == 0 or not (line[i - 1].isalnum() or line[i - 1] == "_")
            after_idx = i + len("glEnd")
            after_ok = after_idx >= n or not (
                line[after_idx].isalnum() or line[after_idx] == "_"
            )
            if before_ok and after_ok:
                if active_opens > 0:
                    active_opens -= 1
                else:
                    unindents_before += 1
                i += len("glEnd")
                continue

        i += 1

    return unindents_before, active_opens, in_block_comment


def format_content(content: str) -> str:
    """Formats the contents of a .glr file using C 2-space indentation rules + glBegin/glEnd."""
    lines = content.splitlines()
    formatted_lines: list[str] = []
    indent_level = 0
    in_block_comment = False

    for line in lines:
        stripped = line.strip()

        # Handle blank lines
        if not stripped:
            formatted_lines.append("")
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
            formatted_lines.append(stripped)
            continue

        # Scan line for indent structure
        unindents_before, indents_after, in_block_comment = scan_line(
            line, in_block_comment
        )

        # Apply unindent before formatting this line
        indent_level = max(0, indent_level - unindents_before)

        # Format and append
        indent_spaces = "  " * indent_level
        formatted_lines.append(indent_spaces + stripped)

        # Apply indent after formatting this line
        indent_level = max(0, indent_level + indents_after)

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
