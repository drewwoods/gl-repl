#!/usr/bin/env python3
"""Suggest or apply Markdown links for inline-code file/type/function identifiers.

This is intentionally conservative. It only considers inline-code spans such as
`ReplRuntimeState`, `src/repl/state.h`, `source_document_view()`, or
`source_document_view ()`, skips existing Markdown links and fenced code blocks,
and only links identifiers with a unique target.
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
FUNCTION_TOKEN_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(\)$")
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
NAMED_BLOCK_RE = re.compile(
    r"^[ \t]*(?:typedef[ \t]+)?(?:struct|enum|union)[ \t]+"
    r"([A-Z][A-Za-z0-9_]*)[ \t]*\{"
)
TYPEDEF_BLOCK_START_RE = re.compile(r"^[ \t]*typedef[ \t]+(?:struct|enum|union)\b")
TYPEDEF_BLOCK_CLOSE_RE = re.compile(r"\}[ \t]*([A-Z][A-Za-z0-9_]*)[ \t]*;")
TYPEDEF_FORWARD_RE = re.compile(
    r"^[ \t]*typedef[ \t]+(?:struct|enum|union)[ \t]+"
    r"([A-Z][A-Za-z0-9_]*)[ \t]+([A-Z][A-Za-z0-9_]*)[ \t]*;"
)
SINGLELINE_TYPEDEF_RE = re.compile(r"\b([A-Z][A-Za-z0-9_]*)\s*;")
FUNCTION_DECL_NAME_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


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
    named_blocks: dict[tuple[str, str], int] = {}
    for rel in git_files(root, "*.c", "*.h"):
        path = root / rel
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue

        lines = text.splitlines()
        idx = 0
        while idx < len(lines):
            line_no = idx + 1
            line = lines[idx]
            stripped = line.strip()
            if not stripped.startswith("typedef "):
                block_match = NAMED_BLOCK_RE.match(line)
                if block_match:
                    named_blocks[(rel, block_match.group(1))] = line_no
                idx += 1
                continue

            fwd_match = TYPEDEF_FORWARD_RE.match(line)
            if fwd_match:
                tag_name = fwd_match.group(1)
                alias_name = fwd_match.group(2)
                concrete_line = named_blocks.get((rel, tag_name))
                if concrete_line is not None:
                    add_type(types, alias_name, rel, concrete_line)
                idx += 1
                continue

            if TYPEDEF_BLOCK_START_RE.match(line) and "{" in line:
                block_match = NAMED_BLOCK_RE.match(line)
                if block_match:
                    named_blocks[(rel, block_match.group(1))] = line_no
                end_idx = idx
                while end_idx < len(lines):
                    close_match = TYPEDEF_BLOCK_CLOSE_RE.search(lines[end_idx])
                    if close_match:
                        add_type(types, close_match.group(1), rel, line_no)
                        idx = end_idx + 1
                        break
                    end_idx += 1
                else:
                    idx += 1
                continue

            if not stripped.endswith(";"):
                idx += 1
                continue
            # Function-pointer typedefs and complex macro-shaped typedefs are
            # intentionally skipped. The tool favors false negatives.
            if "(" in stripped or ")" in stripped:
                idx += 1
                continue
            match = SINGLELINE_TYPEDEF_RE.search(stripped)
            if match:
                add_type(types, match.group(1), rel, line_no)
            idx += 1
    return types


def build_function_index(root: Path) -> dict[str, list[Target]]:
    funcs: dict[str, list[Target]] = {}
    for rel in git_files(root, "*.h"):
        path = root / rel
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue

        brace_depth = 0
        pending = ""
        pending_line = 0
        for line_no, line in enumerate(lines, 1):
            stripped = line.strip()

            if pending:
                pending += " " + stripped
                if ";" in pending or "{" in pending:
                    add_function_decl(funcs, pending, rel, pending_line)
                    pending = ""
                brace_depth += brace_delta_for_scan(stripped)
                continue

            if brace_depth != 0:
                brace_depth += brace_delta_for_scan(stripped)
                continue

            if line_can_start_function_decl(stripped):
                if ";" in stripped or "{" in stripped:
                    add_function_decl(funcs, stripped, rel, line_no)
                else:
                    pending = stripped
                    pending_line = line_no

            brace_depth += brace_delta_for_scan(stripped)
    return funcs


def brace_delta_for_scan(line: str) -> int:
    return line.count("{") - line.count("}")


def line_can_start_function_decl(stripped: str) -> bool:
    if "(" not in stripped:
        return False
    if not stripped:
        return False
    if stripped.startswith(("#", "/*", "*", "//")):
        return False
    if stripped.startswith("typedef "):
        return False
    return True


def add_function_decl(funcs: dict[str, list[Target]], chunk: str, rel: str, line: int) -> None:
    semi = chunk.find(";")
    brace = chunk.find("{")
    if brace >= 0 and brace < semi:
        decl = chunk[:brace]
        if not decl.lstrip().startswith("static inline "):
            return
    elif brace >= 0 and semi < 0:
        decl = chunk[:brace]
        if not decl.lstrip().startswith("static inline "):
            return
    elif semi >= 0:
        decl = chunk[:semi]
    else:
        return
    if "typedef " in decl or "(*" in decl:
        return
    matches = FUNCTION_DECL_NAME_RE.findall(decl)
    if not matches:
        return
    name = matches[-1]
    if is_plausible_function_name(name):
        funcs.setdefault(name, []).append(Target(rel, line))


def default_markdown_files(root: Path) -> list[Path]:
    rels = git_files(root, "*.md", ":(exclude)docs/plans/**", ":(exclude)third_party/**")
    return dedupe_paths([root / rel for rel in rels])


def dedupe_paths(paths: list[Path]) -> list[Path]:
    by_real: dict[Path, Path] = {}
    order: list[Path] = []
    for path in paths:
        real = path.resolve()
        if real not in by_real:
            by_real[real] = path
            order.append(real)
            continue
        if by_real[real].is_symlink() and not path.is_symlink():
            by_real[real] = path
    return [by_real[real] for real in order]


def existing_link_ranges(line: str) -> list[tuple[int, int]]:
    return [(m.start(), m.end()) for m in LINK_RE.finditer(line)]


def overlaps_any(start: int, end: int, ranges: list[tuple[int, int]]) -> bool:
    return any(start < r_end and end > r_start for r_start, r_end in ranges)


def classify_ident(
    ident: str,
    types: dict[str, list[Target]],
    functions: dict[str, list[Target]],
) -> str | None:
    if FILE_TOKEN_RE.match(ident):
        return "file"
    func_match = FUNCTION_TOKEN_RE.match(ident)
    if func_match and is_plausible_function_name(func_match.group(1)):
        return "function"
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


def is_plausible_function_name(ident: str) -> bool:
    if ident.startswith("_"):
        return False
    if "_" not in ident:
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
        suffix_matches = [target for path, target in by_path.items() if path.endswith("/" + ident)]
        if len(suffix_matches) == 1:
            return suffix_matches[0], "matched"
        if len(suffix_matches) > 1:
            return None, "ambiguous file suffix: " + ", ".join(t.relpath for t in suffix_matches[:5])
        return None, "no file match"

    try:
        rel = str((source.parent / ident).resolve().relative_to(root))
    except ValueError:
        rel = ""
    if rel in by_path:
        return by_path[rel], "matched"

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


def resolve_function(ident: str, functions: dict[str, list[Target]]) -> tuple[Target | None, str]:
    name = FUNCTION_TOKEN_RE.match(ident)
    assert name is not None
    matches = functions.get(name.group(1), [])
    if len(matches) == 1:
        return matches[0], "matched"
    if len(matches) > 1:
        return None, "ambiguous header function: " + ", ".join(t.markdown() for t in matches[:5])
    return None, "no header function match"


def scan_file(
    path: Path,
    root: Path,
    by_basename: dict[str, list[Target]],
    by_path: dict[str, Target],
    types: dict[str, list[Target]],
    functions: dict[str, list[Target]],
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
            if ident != match.group(1):
                continue
            if " " in ident and not FUNCTION_TOKEN_RE.match(ident):
                continue
            kind = classify_ident(ident, types, functions)
            if kind is None:
                continue
            if kind == "file":
                target, reason = resolve_file(ident, path, root, by_basename, by_path)
            elif kind == "type":
                target, reason = resolve_type(ident, types)
            else:
                target, reason = resolve_function(ident, functions)
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
        description="Suggest or apply links for inline-code file/type/function identifiers in Markdown."
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true", help="Print matches without writing files (default).")
    mode.add_argument("--write", action="store_true", help="Rewrite files, linking unique matches.")
    parser.add_argument("files", nargs="*", help="Markdown files to scan. Defaults to tracked docs outside docs/plans/.")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    root = repo_root()
    files = [(Path(f) if Path(f).is_absolute() else root / f) for f in args.files]
    files = dedupe_paths(files)
    if not files:
        files = default_markdown_files(root)

    by_basename, by_path = build_file_index(root)
    types = build_type_index(root)
    functions = build_function_index(root)

    all_occurrences: list[Occurrence] = []
    writes = 0
    for path in files:
        if not path.exists():
            print(f"ERROR: {path} does not exist", file=sys.stderr)
            return 1
        occurrences, lines = scan_file(path, root, by_basename, by_path, types, functions)
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
