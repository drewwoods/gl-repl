#!/usr/bin/env python3
"""Generate the compiled-in command-description catalog from plain text."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SECTION_RE = re.compile(r"^\[(command|capability) ([A-Z][A-Z0-9_]*)\]$")
TITLE_RE = re.compile(r"^title\s*=\s*(.+)$")

# Language/editor records are GLCmd values too, but they are not GL-family
# commands. glEnable/glDisable are deliberately absent: their popup content is
# selected from the capability catalog by args[0].
NON_GL_COMMANDS = {
    "CMD_ENABLE",
    "CMD_DISABLE",
    "CMD_FOR_BEGIN",
    "CMD_FOR_END",
    "CMD_FUNC_DEF",
    "CMD_FUNC_END",
    "CMD_CALL",
    "CMD_IF_BEGIN",
    "CMD_IF_END",
    "CMD_ELSE_IF",
    "CMD_ELSE",
    "CMD_COMMENT",
    "CMD_EMPTY",
    "CMD_VAR_ASSIGN",
    "CMD_SCRATCH_ASSIGN",
    "CMD_VAR_DECLARE",
    "CMD_GOTO_LABEL",
    "CMD_GOTO",
}


class CatalogError(Exception):
    pass


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def c_string(text: str) -> str:
    out = ['"']
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 32 or ord(ch) > 126:
            out.append("\\x%02x" % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def strip_c_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)


def command_types(header_path: Path) -> set[str]:
    try:
        text = header_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CatalogError(f"{header_path}: {exc}") from exc
    match = re.search(r"typedef\s+enum\s*\{(.*?)\}\s*CmdType\s*;", text,
                      flags=re.DOTALL)
    if not match:
        raise CatalogError(f"{header_path}: could not find CmdType enum")
    tokens = set(re.findall(r"\bCMD_[A-Z0-9_]+\b",
                            strip_c_comments(match.group(1))))
    tokens.discard("CMD_TYPE_COUNT")
    if not tokens:
        raise CatalogError(f"{header_path}: CmdType enum is empty")
    return tokens


def enable_capabilities(spec_path: Path) -> set[str]:
    try:
        text = spec_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CatalogError(f"{spec_path}: {exc}") from exc
    match = re.search(
        r"static\s+const\s+ReplEnumEntry\s+k_enable_caps\[\]\s*=\s*\{(.*?)\};",
        text, flags=re.DOTALL)
    if not match:
        raise CatalogError(f"{spec_path}: could not find k_enable_caps")
    rows = re.findall(r'\{\s*"([A-Z0-9_]+)"\s*,\s*([A-Z0-9_]+)\s*\}',
                      strip_c_comments(match.group(1)))
    caps: set[str] = set()
    for label, token in rows:
        if label != token:
            raise CatalogError(
                f"{spec_path}: capability label {label} does not match {token}"
            )
        caps.add(token)
    if not caps:
        raise CatalogError(f"{spec_path}: k_enable_caps is empty")
    return caps


def normalize_body(lines: list[str]) -> str:
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(line.rstrip() for line in lines)


def read_catalog(path: Path) -> dict[tuple[str, str], tuple[str, str]]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise CatalogError(f"{path}: {exc}") from exc

    records: dict[tuple[str, str], tuple[str, str]] = {}
    current: tuple[str, str] | None = None
    title: str | None = None
    body_lines: list[str] = []

    def finish(line_no: int) -> None:
        nonlocal current, title, body_lines
        if current is None:
            return
        body = normalize_body(body_lines)
        if not title:
            raise CatalogError(
                f"{path}:{line_no}: [{current[0]} {current[1]}] has no title"
            )
        if not body:
            raise CatalogError(
                f"{path}:{line_no}: [{current[0]} {current[1]}] has no description"
            )
        records[current] = (title, body)
        current = None
        title = None
        body_lines = []

    for line_no, line in enumerate(lines, start=1):
        section = SECTION_RE.match(line.strip())
        if section:
            finish(line_no)
            current = (section.group(1), section.group(2))
            if current in records:
                raise CatalogError(
                    f"{path}:{line_no}: duplicate [{current[0]} {current[1]}]"
                )
            continue
        if current is None:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            raise CatalogError(f"{path}:{line_no}: content outside a section")
        title_match = TITLE_RE.match(line.strip())
        if title is None and title_match:
            title = title_match.group(1).strip()
            if not title:
                raise CatalogError(f"{path}:{line_no}: title must not be empty")
            continue
        if title is None and line.strip():
            raise CatalogError(
                f"{path}:{line_no}: first section field must be 'title = ...'"
            )
        body_lines.append(line)

    finish(len(lines) + 1)
    if not records:
        raise CatalogError(f"{path}: catalog is empty")
    return records


def validate(records: dict[tuple[str, str], tuple[str, str]],
             commands: set[str], caps: set[str]) -> None:
    expected_commands = commands - NON_GL_COMMANDS
    actual_commands = {key for kind, key in records if kind == "command"}
    actual_caps = {key for kind, key in records if kind == "capability"}

    unknown_non_gl = NON_GL_COMMANDS - commands
    if unknown_non_gl:
        raise CatalogError(
            "NON_GL_COMMANDS contains unknown CmdType(s): "
            + ", ".join(sorted(unknown_non_gl))
        )

    problems: list[str] = []
    for label, missing, extra in (
        ("command", expected_commands - actual_commands,
         actual_commands - expected_commands),
        ("capability", caps - actual_caps, actual_caps - caps),
    ):
        if missing:
            problems.append(f"missing {label}(s): {', '.join(sorted(missing))}")
        if extra:
            problems.append(f"unknown {label}(s): {', '.join(sorted(extra))}")
    if problems:
        raise CatalogError("; ".join(problems))


def render(records: dict[tuple[str, str], tuple[str, str]]) -> str:
    out = [
        "/* Generated by scripts/gen_command_descriptions.py. Do not edit. */",
        "",
        "static const ReplCommandDescriptionRecord",
        "g_repl_command_description_records[CMD_TYPE_COUNT] = {",
    ]
    for kind, key in sorted(records):
        if kind != "command":
            continue
        title, body = records[(kind, key)]
        out.append(f"    [{key}] = {{ {c_string(title)}, {c_string(body)} }},")
    out.extend(["};", "", "static const ReplCapabilityDescriptionRecord",
                "g_repl_capability_description_records[] = {"])
    for kind, key in sorted(records):
        if kind != "capability":
            continue
        title, body = records[(kind, key)]
        out.append(f"    {{ {key}, {c_string(title)}, {c_string(body)} }},")
    out.extend(["};", ""])
    return "\n".join(out)


def write_if_changed(path: Path, text: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def main(argv: list[str]) -> int:
    root = repo_root()
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--catalog", default=str(root / "command_descriptions.txt")
    )
    parser.add_argument(
        "--command-header", default=str(root / "src/repl/command.h")
    )
    parser.add_argument(
        "--command-spec", default=str(root / "src/repl/command_spec.c")
    )
    parser.add_argument(
        "--out",
        default=str(root / "build/generated/repl_command_descriptions_data.inc"),
    )
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)

    try:
        records = read_catalog(Path(args.catalog))
        validate(records, command_types(Path(args.command_header)),
                 enable_capabilities(Path(args.command_spec)))
        if not args.check:
            write_if_changed(Path(args.out), render(records))
    except CatalogError as exc:
        print(f"gen_command_descriptions.py: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
