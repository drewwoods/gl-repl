#!/usr/bin/env python3
"""Count Unicode in tracked project sources and enforce replacement rules.

By default this reports every non-ASCII character in project-owned tracked
``*.c``, ``*.h``, and ``*.md`` files (vendored ``third_party/`` sources are
excluded). Unicode is allowed unless a character appears in
``REPLACEMENT_RULES``.  ``--check`` fails when a replacement is required;
``--fix`` applies every declared replacement in place.

Current policy:
  U+2014 EM DASH -> ASCII hyphen-minus (``-``)
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class ReplacementRule:
    """A Unicode character that is forbidden in source and its ASCII form."""

    replacement: str
    reason: str


# Add future normalization rules here. Characters not listed remain legal and
# are still included in the report, so the policy is visible rather than an
# implicit ASCII-only restriction.
REPLACEMENT_RULES = {
    "\u2014": ReplacementRule("-", "use ASCII hyphen-minus"),
}


def tracked_sources() -> list[Path]:
    """Return project-owned tracked C and Markdown files, in stable order.

    Vendored sources remain under their upstream encoding and style policy;
    this guard intentionally applies only to the project's own C sources.
    """
    result = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z", "--", "*.c", "*.h", "*.md"],
        check=False,
        capture_output=True,
    )
    if result.returncode:
        stderr = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"could not list tracked source files: {stderr}")
    names = result.stdout.decode("utf-8").split("\0")
    return [ROOT / name for name in names
            if name and not name.startswith("third_party/")]


def unicode_counts(paths: list[Path]) -> tuple[Counter[str], Counter[str]]:
    """Return total character counts and affected-file counts by character."""
    counts: Counter[str] = Counter()
    files: Counter[str] = Counter()
    for path in paths:
        text = path.read_text(encoding="utf-8")
        chars = [char for char in text if ord(char) > 0x7F]
        counts.update(chars)
        files.update(set(chars))
    return counts, files


def show_report(counts: Counter[str], files: Counter[str]) -> None:
    if not counts:
        print("No Unicode characters found in tracked project sources.")
        return

    print("Unicode in tracked project sources:")
    for char in sorted(counts, key=ord):
        rule = REPLACEMENT_RULES.get(char)
        policy = (
            f"replace with {rule.replacement!r} ({rule.reason})"
            if rule
            else "allowed"
        )
        print(f"  U+{ord(char):04X} {char!r}: {counts[char]} occurrence(s) "
              f"in {files[char]} file(s); {policy}")


def apply_replacements(paths: list[Path]) -> tuple[Counter[str], Counter[str]]:
    """Apply each explicit normalization rule and return (counts, files)."""
    replaced: Counter[str] = Counter()
    changed_files: Counter[str] = Counter()
    for path in paths:
        text = path.read_text(encoding="utf-8")
        updated = text
        for char, rule in REPLACEMENT_RULES.items():
            count = updated.count(char)
            if count:
                replaced[char] += count
                changed_files[char] += 1
                updated = updated.replace(char, rule.replacement)
        if updated != text:
            path.write_text(updated, encoding="utf-8")
    return replaced, changed_files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true",
                       help="fail if any replacement rule matches")
    group.add_argument("--fix", action="store_true",
                       help="apply all replacement rules before reporting")
    args = parser.parse_args()

    try:
        paths = tracked_sources()
        if args.fix:
            replaced, changed_files = apply_replacements(paths)
            for char in sorted(replaced, key=ord):
                rule = REPLACEMENT_RULES[char]
                print(f"Replaced {replaced[char]} U+{ord(char):04X} {char!r} "
                      f"with {rule.replacement!r} in {changed_files[char]} file(s).")
        counts, files = unicode_counts(paths)
    except (OSError, RuntimeError, UnicodeDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    show_report(counts, files)
    forbidden = [char for char in counts if char in REPLACEMENT_RULES]
    if args.check and forbidden:
        print("ERROR: prohibited Unicode found in tracked project sources:",
              file=sys.stderr)
        for char in sorted(forbidden, key=ord):
            rule = REPLACEMENT_RULES[char]
            print(f"  U+{ord(char):04X} {char!r}: {counts[char]} occurrence(s); "
                  f"replace with {rule.replacement!r} ({rule.reason})",
                  file=sys.stderr)
        print("Fix it with: make fix-unicode", file=sys.stderr)
        print("            (or: python3 scripts/count-unicode.py --fix)",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
