#!/usr/bin/env python3
"""Suggest or apply Markdown links for inline-code file/type identifiers.

This is intentionally conservative. It only considers inline-code spans such as
`ReplRuntimeState` or `src/repl/state.h`, skips existing Markdown links and
fenced code blocks, and only links identifiers with a unique target.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


LINK_RE = re.compile(r"(!?)\[([^\]\n]+)\]\(([^)\n]+)\)")
CODE_SPAN_RE = re.compile(r"`([^`\n]+)`")
TYPE_NAME_RE = re.compile(r"^[A-Z][A-Za-z0-9_]*$")
ALL_CAPS_RE = re.compile(r"^[A-Z0-9_]+$")
FILE_TOKEN_RE = re.compile(r"^(?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_.-]+\.[ch]$")
TYPE_FALLBACK_SUFFIXES = (
    "Bridge",
    "Capture",
    "Change",
    "Config",
    "Context",
    "Desc",
    "Entry",
    "Group",
    "Kind",
    "Layout",
    "Mask",
    "Needs",
    "Output",
    "Plan",
    "Provider",
    "Snapshot",
    "State",
    "Store",
    "Theme",
    "Type",
    "View",
)
TYPE_FALLBACK_PREFIXES = (
    "Cmd",
    "Editor",
    "Export",
    "Flat",
    "Frame",
    "GL",
    "Glr",
    "Mesh",
    "Replay",
    "Repl",
    "Scene",
    "Source",
    "Tutorial",
    "Ui",
)
MULTILINE_TYPEDEF_RE = re.compile(
    r"(?ms)^[ \t]*typedef[ \t]+(?:struct|enum|union)\b.*?\}[ \t]*"
    r"([A-Z][A-Za-z0-9_]*)[ \t]*;"
)
SINGLELINE_TYPEDEF_RE = re.compile(r"\b([A-Z][A-Za-z0-9_]*)\s*;")


@dataclass(frozen=True)
class Target:
    relpath: str
    line: int | None = None

    def markdown(self) -> str:
        if self.line is None:
            return self.relpath
        return f"{self.relpath}#L{self.line}"


@dataclass
class Occurrence:
    source: Path
    line_no: int
    start: int
    end: int
    ident: str
    kind: str
    target: Target | None
    reason: str

    def replacement(self) -> str:
        assert self.target is not None
        return f"[`{self.ident}`]({self.target.markdown()})"


def repo_root() -> Path:
    out = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True)
    return Path(out.strip())


def git_files(root: Path, *patterns: str) -> list[str]:
    out = subprocess.check_output(["git", "ls-files", *patterns], cwd=root, text=True)
    return [line for line in out.splitlines() if line]


def line_no_for_offset(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def build_file_index(root: Path) -> tuple[dict[str, list[Target]], dict[str, Target]]:
    rels = git_files(root, "*.c", "*.h")
    by_basename: dict[str, list[Target]] = {}
    by_path: dict[str, Target] = {}
    for rel in rels:
        target = Target(rel)
        by_path[rel] = target
        by_basename.setdefault(Path(rel).name, []).append(target)
    return by_basename, by_path


def add_type(types: dict[str, list[Target]], name: str, rel: str, line: int) -> None:
    if is_plausible_type_name(name):
        types.setdefault(name, []).append(Target(rel, line))


def build_type_index(root: Path) -> dict[str, list[Target]]:
    types: dict[str, list[Target]] = {}
    for rel in git_files(root, "*.c", "*.h"):
        path = root / rel
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue

        claimed_lines: set[int] = set()
        for match in MULTILINE_TYPEDEF_RE.finditer(text):
            line = line_no_for_offset(text, match.start())
            add_type(types, match.group(1), rel, line)
            claimed_lines.update(range(line, line_no_for_offset(text, match.end()) + 1))

        for line_no, line in enumerate(text.splitlines(), 1):
            stripped = line.strip()
            if line_no in claimed_lines:
                continue
            if not stripped.startswith("typedef "):
                continue
            if not stripped.endswith(";"):
                continue
            # Function-pointer typedefs and complex macro-shaped typedefs are
            # intentionally skipped. The tool favors false negatives.
            if "(" in stripped or ")" in stripped:
                continue
            match = SINGLELINE_TYPEDEF_RE.search(stripped)
            if match:
                add_type(types, match.group(1), rel, line_no)
    return types


def default_markdown_files(root: Path) -> list[Path]:
    rels = git_files(root, "*.md", ":(exclude)plans/**", ":(exclude)third_party/**")
    return [root / rel for rel in rels]


def existing_link_ranges(line: str) -> list[tuple[int, int]]:
    return [(m.start(), m.end()) for m in LINK_RE.finditer(line)]


def overlaps_any(start: int, end: int, ranges: list[tuple[int, int]]) -> bool:
    return any(start < r_end and end > r_start for r_start, r_end in ranges)


def classify_ident(ident: str, types: dict[str, list[Target]]) -> str | None:
    if FILE_TOKEN_RE.match(ident):
        return "file"
    if is_plausible_type_name(ident) and (
        ident in types
        or (ident.startswith(TYPE_FALLBACK_PREFIXES) and ident.endswith(TYPE_FALLBACK_SUFFIXES))
    ):
        return "type"
    return None


def is_plausible_type_name(ident: str) -> bool:
    if not TYPE_NAME_RE.match(ident):
        return False
    if ALL_CAPS_RE.match(ident):
        return False
    if not any(ch.islower() for ch in ident):
        return False
    return True


def resolve_file(
    ident: str,
    source: Path,
    root: Path,
    by_basename: dict[str, list[Target]],
    by_path: dict[str, Target],
) -> tuple[Target | None, str]:
    if "/" in ident:
        if ident.startswith("/"):
            rel = ident
        else:
            try:
                rel = str((source.parent / ident).resolve().relative_to(root))
            except ValueError:
                return None, "file path escapes repository"
        if rel in by_path:
            return by_path[rel], "matched"
        if (root / ident).exists():
            return Target(ident), "matched"
        return None, "no file match"

    matches = by_basename.get(ident, [])
    if len(matches) == 1:
        return matches[0], "matched"
    if len(matches) > 1:
        return None, "ambiguous file basename: " + ", ".join(t.relpath for t in matches[:5])
    return None, "no file match"


def resolve_type(ident: str, types: dict[str, list[Target]]) -> tuple[Target | None, str]:
    matches = types.get(ident, [])
    if len(matches) == 1:
        return matches[0], "matched"
    if len(matches) > 1:
        return None, "ambiguous type: " + ", ".join(t.markdown() for t in matches[:5])
    return None, "no typedef match"


def scan_file(
    path: Path,
    root: Path,
    by_basename: dict[str, list[Target]],
    by_path: dict[str, Target],
    types: dict[str, list[Target]],
) -> tuple[list[Occurrence], list[str]]:
    occurrences: list[Occurrence] = []
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    in_fence = False
    for line_no, line in enumerate(lines, 1):
        stripped = line.lstrip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue

        linked_ranges = existing_link_ranges(line)
        for match in CODE_SPAN_RE.finditer(line):
            if overlaps_any(match.start(), match.end(), linked_ranges):
                continue
            ident = match.group(1).strip()
            if ident != match.group(1) or " " in ident:
                continue
            kind = classify_ident(ident, types)
            if kind is None:
                continue
            if kind == "file":
                target, reason = resolve_file(ident, path, root, by_basename, by_path)
            else:
                target, reason = resolve_type(ident, types)
            occurrences.append(
                Occurrence(path, line_no, match.start(), match.end(), ident, kind, target, reason)
            )
    return occurrences, lines


def apply_replacements(lines: list[str], occurrences: list[Occurrence]) -> list[str]:
    by_line: dict[int, list[Occurrence]] = {}
    for occ in occurrences:
        if occ.target is not None:
            by_line.setdefault(occ.line_no, []).append(occ)

    out = list(lines)
    for line_no, occs in by_line.items():
        line = out[line_no - 1]
        for occ in sorted(occs, key=lambda o: o.start, reverse=True):
            line = line[: occ.start] + occ.replacement() + line[occ.end :]
        out[line_no - 1] = line
    return out


def print_occurrences(root: Path, occurrences: list[Occurrence]) -> None:
    for occ in occurrences:
        rel = occ.source.relative_to(root)
        if occ.target is not None:
            print(f"{rel}:{occ.line_no}:{occ.start + 1}: {occ.kind} `{occ.ident}` -> {occ.target.markdown()}")
        else:
            print(f"{rel}:{occ.line_no}:{occ.start + 1}: {occ.kind} `{occ.ident}` -> ({occ.reason})")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Suggest or apply links for inline-code file/type identifiers in Markdown."
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true", help="Print matches without writing files (default).")
    mode.add_argument("--write", action="store_true", help="Rewrite files, linking unique matches.")
    parser.add_argument("files", nargs="*", help="Markdown files to scan. Defaults to tracked docs outside plans/.")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    root = repo_root()
    files = [(Path(f) if Path(f).is_absolute() else root / f) for f in args.files]
    if not files:
        files = default_markdown_files(root)

    by_basename, by_path = build_file_index(root)
    types = build_type_index(root)

    all_occurrences: list[Occurrence] = []
    writes = 0
    for path in files:
        if not path.exists():
            print(f"ERROR: {path} does not exist", file=sys.stderr)
            return 1
        occurrences, lines = scan_file(path.resolve(), root, by_basename, by_path, types)
        all_occurrences.extend(occurrences)
        if args.write:
            new_lines = apply_replacements(lines, occurrences)
            if new_lines != lines:
                path.write_text("".join(new_lines), encoding="utf-8")
                writes += 1

    print_occurrences(root, all_occurrences)
    matched = sum(1 for occ in all_occurrences if occ.target is not None)
    mode = "write" if args.write else "dry-run"
    print(f"{mode}: {len(all_occurrences)} identifiers, {matched} unique matches, {writes} files written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
