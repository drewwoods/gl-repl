#!/usr/bin/env python3
"""Validate local Markdown file links and GitHub-style line anchors.

Default scope is tracked Markdown outside plans/ and third_party/. Historical
plans can contain absolute links into old worktrees, so they are intentionally
not part of the default reader-facing docs check.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote


LINK_RE = re.compile(r"(!?)\[([^\]\n]+)\]\(([^)\n]+)\)")
LINE_ANCHOR_RE = re.compile(r"^L([0-9]+)(?:-L?[0-9]+)?$")
EXTERNAL_SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")
TYPE_NAME_RE = re.compile(r"^[A-Z][A-Za-z0-9_]*$")
TYPEDEF_START_RE = re.compile(r"^\s*typedef\s+(struct|enum|union)\b")
FUNCTION_LABEL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\(\)$")


def repo_root() -> Path:
    out = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True)
    return Path(out.strip())


def default_markdown_files(root: Path) -> list[Path]:
    out = subprocess.check_output(
        ["git", "ls-files", "*.md", ":(exclude)plans/**", ":(exclude)third_party/**"],
        cwd=root,
        text=True,
    )
    return dedupe_paths([root / line for line in out.splitlines() if line])


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


def strip_link_destination(raw: str) -> str:
    s = raw.strip()
    if not s:
        return s
    if s[0] == "<":
        end = s.find(">")
        return s[1:end] if end >= 0 else s[1:]
    # CommonMark allows an optional title after whitespace. Paths with spaces
    # should use the <...> form above.
    return s.split(None, 1)[0]


def normalize_label(label: str) -> str:
    s = re.sub(r"<[^>]*>", "", label)
    s = s.replace("`", "")
    s = s.replace("\\[", "[").replace("\\]", "]")
    return " ".join(s.strip().split())


def normalize_line(line: str) -> str:
    return " ".join(line.strip().split())


def is_external(target: str) -> bool:
    return bool(EXTERNAL_SCHEME_RE.match(target))


def split_target(target: str) -> tuple[str, str]:
    if "#" not in target:
        return target, ""
    path_part, anchor = target.split("#", 1)
    return path_part, anchor


def is_typedef_anchor_ok(lines: list[str], line_idx: int, label: str) -> bool:
    if not TYPE_NAME_RE.match(label):
        return False
    if line_idx < 0 or line_idx >= len(lines):
        return False
    if not TYPEDEF_START_RE.search(lines[line_idx]):
        return False

    depth = 0
    saw_open = False
    for idx in range(line_idx, min(len(lines), line_idx + 200)):
        line = lines[idx]
        if label in line:
            return True
        if "{" in line:
            saw_open = True
        depth += line.count("{") - line.count("}")
        if saw_open and depth <= 0 and idx > line_idx and ";" in line:
            return False
    return False


def is_function_anchor_ok(line: str, label: str) -> bool:
    match = FUNCTION_LABEL_RE.match(label)
    if not match:
        return False
    return bool(re.search(r"\b" + re.escape(match.group(1)) + r"\s*\(", line))


def validate_line_anchor(
    *,
    source_file: Path,
    source_line_no: int,
    target_file: Path,
    anchor: str,
    label: str,
    errors: list[str],
) -> None:
    m = LINE_ANCHOR_RE.match(anchor)
    if not m:
        return
    if "-" in anchor:
        errors.append(
            f"{source_file}:{source_line_no}: use a single-line anchor, not #{anchor}: "
            f"{target_file}"
        )
        return

    line_no = int(m.group(1))
    try:
        lines = target_file.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        errors.append(f"{source_file}:{source_line_no}: cannot read linked file as UTF-8: {target_file}")
        return

    if line_no < 1 or line_no > len(lines):
        errors.append(
            f"{source_file}:{source_line_no}: #{anchor} is outside {target_file} "
            f"({len(lines)} lines)"
        )
        return

    label_norm = normalize_label(label)
    linked_line = normalize_line(lines[line_no - 1])
    if label_norm and label_norm in linked_line:
        return
    if is_function_anchor_ok(linked_line, label_norm):
        return
    if is_typedef_anchor_ok(lines, line_no - 1, label_norm):
        return

    errors.append(
        f"{source_file}:{source_line_no}: link label {label_norm!r} is not on "
        f"{target_file}#{anchor}: {linked_line!r}"
    )


def iter_markdown_links(path: Path) -> tuple[int, str, str]:
    in_fence = False
    for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.lstrip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for match in LINK_RE.finditer(line):
            yield line_no, match.group(2), strip_link_destination(match.group(3))


def validate_file(path: Path, root: Path, errors: list[str]) -> int:
    link_count = 0
    try:
        links = list(iter_markdown_links(path))
    except UnicodeDecodeError:
        errors.append(f"{path}: cannot read Markdown file as UTF-8")
        return 0

    for line_no, label, target in links:
        if not target:
            continue
        if is_external(target):
            continue

        target_path_raw, anchor = split_target(target)
        # Same-document heading anchors are intentionally ignored; heading
        # validation is a separate problem from file/line-link validation.
        if not target_path_raw and not anchor.startswith("L"):
            continue

        link_count += 1
        target_path = Path(unquote(target_path_raw)) if target_path_raw else path
        if not target_path.is_absolute():
            doc_relative = (path.parent / target_path).resolve()
            root_relative = (root / target_path).resolve()
            target_path = doc_relative if doc_relative.exists() else root_relative

        if not target_path.exists():
            errors.append(f"{path}:{line_no}: linked path does not exist: {target}")
            continue

        if anchor.startswith("L"):
            validate_line_anchor(
                source_file=path,
                source_line_no=line_no,
                target_file=target_path,
                anchor=anchor,
                label=label,
                errors=errors,
            )

    return link_count


def main(argv: list[str]) -> int:
    root = repo_root()
    if argv:
        files = [(Path(arg) if Path(arg).is_absolute() else root / arg) for arg in argv]
    else:
        files = default_markdown_files(root)

    errors: list[str] = []
    link_count = 0
    for path in files:
        if not path.exists():
            errors.append(f"{path}: Markdown file does not exist")
            continue
        if path.is_dir():
            continue
        link_count += validate_file(path.resolve(), root, errors)

    if errors:
        print("ERROR: invalid Markdown links:", file=sys.stderr)
        for err in errors:
            try:
                display = str(Path(err).relative_to(root))
            except (ValueError, OSError):
                display = err
            print(display, file=sys.stderr)
        return 1

    print(f"doc-links OK ({len(files)} files, {link_count} local file/line links)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
