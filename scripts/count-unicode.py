#!/usr/bin/env python3
"""Count Unicode in tracked project sources and enforce replacement rules.

By default this reports every non-ASCII character in project-owned tracked
``*.c``, ``*.h``, ``*.md``, and ``*.glr`` files (vendored ``third_party/``
sources are excluded). C, header, and Markdown Unicode is allowed unless a
character appears in ``REPLACEMENT_RULES``. Scene ``*.glr`` files are
ASCII-only. ``--check`` fails when a replacement is required or a scene has
Unicode; ``--fix`` applies every declared replacement in place.

Use ``--c-files``, ``--md-files``, and/or ``--glr-files`` to limit the scan
to one or more file groups. Without a file-group option, all supported types
are scanned.

Current policy:
  U+2013 EN DASH -> ASCII hyphen-minus (``-``)
  U+2014 EM DASH -> ASCII hyphen-minus (``-``)
  U+2212 MINUS SIGN -> ASCII hyphen-minus (``-``)
  U+00D7 MULTIPLICATION SIGN -> ``x`` in ``*.c`` and ``*.glr`` files
  U+201C LEFT DOUBLE QUOTATION MARK -> ``"`` in ``*.c`` and ``*.glr`` files
  U+201D RIGHT DOUBLE QUOTATION MARK -> ``"`` in ``*.c`` and ``*.glr`` files
  U+2019 RIGHT SINGLE QUOTATION MARK -> ``'`` in ``*.c`` and ``*.glr`` files
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CYAN = "\033[36m" if sys.stdout.isatty() else ""
RESET = "\033[0m" if sys.stdout.isatty() else ""
FILE_GROUP_SUFFIXES = {
    "c_files": (".c", ".h"),
    "md_files": (".md",),
    "glr_files": (".glr",),
}


@dataclass(frozen=True)
class ReplacementRule:
    """A Unicode character that is forbidden in source and its ASCII form."""

    replacement: str
    reason: str
    suffixes: tuple[str, ...] = ()

    def applies_to(self, path: Path) -> bool:
        """Return whether this rule applies to a file path."""
        return not self.suffixes or path.suffix in self.suffixes

    def scope_description(self) -> str:
        """Return a user-facing description of this rule's file scope."""
        if not self.suffixes:
            return "disallowed globally"
        suffixes = ", ".join(f"*{suffix}" for suffix in self.suffixes)
        return f"disallowed in {suffixes} files"


# Add future normalization rules here. Characters not listed remain legal and
# are still included in the report. They are legal in C, headers, and Markdown
# but not in ASCII-only .glr scene files. A rule can optionally limit itself to
# particular filename suffixes.
REPLACEMENT_RULES = {
    "\u2013": ReplacementRule("-", "use ASCII hyphen-minus"),
    "\u2014": ReplacementRule("-", "use ASCII hyphen-minus"),
    "\u2212": ReplacementRule("-", "use ASCII hyphen-minus"),
    "\u00D7": ReplacementRule("x", "use ASCII x", (".c", ".glr")),
    "\u201C": ReplacementRule("\"", "use ASCII double quote", (".c", ".glr")),
    "\u201D": ReplacementRule("\"", "use ASCII double quote", (".c", ".glr")),
    "\u2019": ReplacementRule("'", "use ASCII apostrophe", (".c", ".glr")),
}


def selected_suffixes(args: argparse.Namespace) -> set[str]:
    """Return the selected suffixes, defaulting to every supported type."""
    suffixes = {
        suffix
        for option, group_suffixes in FILE_GROUP_SUFFIXES.items()
        if getattr(args, option)
        for suffix in group_suffixes
    }
    if suffixes:
        return suffixes
    return {suffix for group_suffixes in FILE_GROUP_SUFFIXES.values()
            for suffix in group_suffixes}


def tracked_sources(suffixes: set[str]) -> list[Path]:
    """Return project-owned tracked C, Markdown, and scene files, in stable order.

    Vendored sources remain under their upstream encoding and style policy;
    this guard intentionally applies only to the project's own C sources.
    """
    patterns = [f"*{suffix}" for suffix in sorted(suffixes)]
    result = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z", "--", *patterns],
        check=False,
        capture_output=True,
    )
    if result.returncode:
        stderr = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"could not list tracked source files: {stderr}")
    names = result.stdout.decode("utf-8").split("\0")
    return [ROOT / name for name in names
            if name and not name.startswith("third_party/")]


def unicode_counts(
        paths: list[Path],
) -> tuple[Counter[str], Counter[str], dict[Path, Counter[str]]]:
    """Return aggregate counts, affected-file counts, and counts per file."""
    counts: Counter[str] = Counter()
    files: Counter[str] = Counter()
    per_file: dict[Path, Counter[str]] = {}
    for path in paths:
        text = path.read_text(encoding="utf-8")
        chars = [char for char in text if ord(char) > 0x7F]
        counts.update(chars)
        files.update(set(chars))
        if chars:
            per_file[path] = Counter(chars)
    return counts, files, per_file


def show_report(counts: Counter[str], files: Counter[str]) -> None:
    if not counts:
        print("No Unicode characters found in tracked project sources.")
        return

    print("Unicode in tracked project sources:")
    for char in sorted(counts, key=ord):
        rule = REPLACEMENT_RULES.get(char)
        policy = (
            f"replace with {rule.replacement!r} ({rule.reason}; "
            f"{rule.scope_description()})"
            if rule
            else "allowed outside ASCII-only .glr files"
        )
        print(f"  U+{ord(char):04X} {char!r}: {counts[char]} occurrence(s) "
              f"in {files[char]} file(s); {policy}")


def show_per_file_report(per_file: dict[Path, Counter[str]]) -> None:
    """Print Unicode counts for every affected source file."""
    if not per_file:
        return

    tree: dict[str, object] = {}
    for path, chars in per_file.items():
        node = tree
        parts = path.relative_to(ROOT).parts
        for part in parts[:-1]:
            child = node.setdefault(part, {})
            assert isinstance(child, dict)
            node = child
        node[parts[-1]] = chars

    def print_tree(node: dict[str, object], depth: int) -> None:
        for name in sorted(node):
            value = node[name]
            indent = "  " * (depth + 1)
            if isinstance(value, Counter):
                summary = ", ".join(
                    f"U+{ord(char):04X} {char!r}={value[char]}"
                    for char in sorted(value, key=ord)
                )
                print(f"{indent}{name}: {summary}")
            else:
                assert isinstance(value, dict)
                print(f"{indent}{CYAN}{name}/{RESET}")
                print_tree(value, depth + 1)

    print("\nUnicode by file:")
    print_tree(tree, 0)


def apply_replacements(paths: list[Path]) -> tuple[Counter[str], Counter[str]]:
    """Apply each explicit normalization rule and return (counts, files)."""
    replaced: Counter[str] = Counter()
    changed_files: Counter[str] = Counter()
    for path in paths:
        text = path.read_text(encoding="utf-8")
        updated = text
        for char, rule in REPLACEMENT_RULES.items():
            count = updated.count(char) if rule.applies_to(path) else 0
            if count:
                replaced[char] += count
                changed_files[char] += 1
                updated = updated.replace(char, rule.replacement)
        if updated != text:
            path.write_text(updated, encoding="utf-8")
    return replaced, changed_files


def replacement_violations(
        per_file: dict[Path, Counter[str]],
) -> Counter[str]:
    """Return configured replacement-rule violations across scanned files."""
    violations: Counter[str] = Counter()
    for path, chars in per_file.items():
        for char, count in chars.items():
            rule = REPLACEMENT_RULES.get(char)
            if rule and rule.applies_to(path):
                violations[char] += count
    return violations


def glr_unicode_violations(
        per_file: dict[Path, Counter[str]],
) -> dict[Path, Counter[str]]:
    """Return all Unicode characters in .glr files not handled by a rule."""
    return {
        path: Counter({char: count for char, count in chars.items()
                       if (rule := REPLACEMENT_RULES.get(char)) is None
                       or not rule.applies_to(path)})
        for path, chars in per_file.items()
        if path.suffix == ".glr" and any(
            (rule := REPLACEMENT_RULES.get(char)) is None or not rule.applies_to(path)
            for char in chars
        )
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true",
                       help="fail if any replacement rule matches")
    group.add_argument("--fix", action="store_true",
                       help="apply all replacement rules before reporting")
    parser.add_argument("--per-file", action="store_true",
                        help="also report Unicode counts for each affected file")
    parser.add_argument("--c-files", action="store_true",
                        help="scan only C implementation and header files")
    parser.add_argument("--md-files", action="store_true",
                        help="scan only Markdown files")
    parser.add_argument("--glr-files", action="store_true",
                        help="scan only .glr scene files")
    args = parser.parse_args()

    try:
        paths = tracked_sources(selected_suffixes(args))
        if args.fix:
            replaced, changed_files = apply_replacements(paths)
            for char in sorted(replaced, key=ord):
                rule = REPLACEMENT_RULES[char]
                print(f"Replaced {replaced[char]} U+{ord(char):04X} {char!r} "
                      f"with {rule.replacement!r} in {changed_files[char]} file(s).")
        counts, files, per_file = unicode_counts(paths)
    except (OSError, RuntimeError, UnicodeDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    show_report(counts, files)
    if args.per_file:
        show_per_file_report(per_file)
    forbidden = replacement_violations(per_file)
    glr_violations = glr_unicode_violations(per_file)
    if args.check and (forbidden or glr_violations):
        print("ERROR: prohibited Unicode found in tracked project sources:",
              file=sys.stderr)
        for char in sorted(forbidden, key=ord):
            rule = REPLACEMENT_RULES[char]
            print(f"  U+{ord(char):04X} {char!r}: {forbidden[char]} occurrence(s); "
                  f"replace with {rule.replacement!r} ({rule.reason}; "
                  f"{rule.scope_description()})",
                  file=sys.stderr)
        for path in sorted(glr_violations):
            chars = glr_violations[path]
            summary = ", ".join(
                f"U+{ord(char):04X} {char!r}={chars[char]}"
                for char in sorted(chars, key=ord)
            )
            print(f"  {path.relative_to(ROOT)}: {summary}; .glr files must be ASCII-only",
                  file=sys.stderr)
        if forbidden:
            print("Fix configured replacements with: make fix-unicode", file=sys.stderr)
        if glr_violations:
            print("Replace the listed .glr characters with ASCII manually; "
                  "make fix-unicode only applies configured replacements.",
                  file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
