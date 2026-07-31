#!/usr/bin/env python3
"""Validate that every user-facing REPL command is documented in USER_GUIDE.md.

For each CmdType in command.h, the corresponding user-facing title in
command_descriptions.txt must appear somewhere in the '## The REPL Language'
section of docs/USER_GUIDE.md — unless the command is listed in EXEMPT_TYPES
below.

Exempt command types are REPL language-level constructs (control flow,
variables, labels, comments) that are documented in the language-structure
sections of USER_GUIDE.md rather than the GL-command bullet lists, or that
are truly internal and have no direct user-facing name at all.

To exempt a new command: add its CmdType name to EXEMPT_TYPES with a short
comment explaining why it doesn't need a bullet in the supported-commands
section. To document a new command: ensure its title from
command_descriptions.txt (the `title = ...` line) appears in USER_GUIDE.md's
'## The REPL Language' section.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Exempt types — these CmdTypes do not need a bullet in USER_GUIDE.md's
# "Supported GL commands" section.  Each entry must carry a short reason.
# ---------------------------------------------------------------------------
EXEMPT_TYPES: dict[str, str] = {
    # Control-flow block heads / tails — documented in the "Control flow"
    # subsections (Variables, Loops, Functions, Conditionals), not as GL cmds.
    "CMD_FOR_BEGIN":  "documented in the Variables / Loops prose section",
    "CMD_FOR_END":    "documented in the Variables / Loops prose section",
    "CMD_FUNC_DEF":   "documented in the Functions prose section",
    "CMD_FUNC_END":   "documented in the Functions prose section",
    "CMD_CALL":       "documented in the Functions prose section",
    "CMD_IF_BEGIN":   "documented in the Conditionals prose section",
    "CMD_IF_END":     "documented in the Conditionals prose section",
    "CMD_ELSE_IF":    "documented in the Conditionals prose section",
    "CMD_ELSE":       "documented in the Conditionals prose section",
    # Variable and scratch assignment — documented in the Variables section.
    "CMD_VAR_ASSIGN":    "documented in the Variables prose section",
    "CMD_SCRATCH_ASSIGN": "documented in the Variables / scratch-array section",
    "CMD_VAR_DECLARE":   "documented in the Variables prose section",
    # Labels and gotos — documented in the Control flow section.
    "CMD_GOTO_LABEL": "documented in the Labels / goto prose section",
    "CMD_GOTO":       "documented in the Labels / goto prose section",
    # Comment and empty line — not a GL command; not listed in bullet form.
    "CMD_COMMENT":    "not a GL command; explained in the language overview",
    "CMD_EMPTY":      "not a user-visible command; represents blank lines",
}

# ---------------------------------------------------------------------------
# Name of the section in USER_GUIDE.md that must contain every non-exempt cmd.
# The check searches the full '## The REPL Language' section (from that
# heading to the next same-level '## ' heading) so all subsections
# (Supported GL commands, GLUT solid shapes, GLU tessellator, Bitmap text,
# Clip planes, Clearing mid-scene, Stencil masks, Fog, ...) are included.
# ---------------------------------------------------------------------------
REPL_LANGUAGE_SECTION_RE = re.compile(
    r"^## The REPL Language\n(.*?)(?=^## |\Z)",
    re.MULTILINE | re.DOTALL,
)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def parse_cmd_types(header: str) -> list[str]:
    """Return all CmdType names from the typedef enum in command.h."""
    # Find the block: typedef enum { ... } CmdType;
    m = re.search(
        r"typedef\s+enum\s*\{([^}]*)\}\s*CmdType\s*;",
        header,
        re.DOTALL,
    )
    if not m:
        raise SystemExit("command.h: cannot locate the CmdType typedef enum")
    body = m.group(1)
    # Extract identifiers (drop numeric suffixes / trailing comments).
    names: list[str] = []
    for token in re.findall(r"\bCMD_[A-Z0-9_]+\b", body):
        if token == "CMD_TYPE_COUNT":
            continue
        if token not in names:
            names.append(token)
    return names


def parse_command_titles(desc_txt: str, spec_c: str) -> dict[str, str]:
    """Return {CmdType -> user-facing name} for all documented commands.

    Primary source: command_descriptions.txt — the `title = ...` line of each
    [command CMD_XXX] section.

    Fallback: command_spec.c — the first positional string in each
    k_enum_command_specs[] and k_std_command_specs[] row (the GL function name,
    e.g. "glEnable").  Used for commands like CMD_ENABLE / CMD_DISABLE that
    intentionally have no [command ...] entry in command_descriptions.txt
    (their popup descriptions come from the capability lookup path instead).
    """
    mapping: dict[str, str] = {}

    # Primary: command_descriptions.txt titles
    for m in re.finditer(
        r"^\[command\s+(CMD_[A-Z0-9_]+)\](.*?)(?=^\[|\Z)",
        desc_txt,
        re.MULTILINE | re.DOTALL,
    ):
        cmd_type = m.group(1)
        body = m.group(2)
        title_m = re.search(r"^title\s*=\s*(.+)", body, re.MULTILINE)
        if title_m:
            mapping[cmd_type] = title_m.group(1).strip()

    # Fallback: k_enum_command_specs[] — rows like:
    #   { "glEnable", CMD_ENABLE, ... }
    for m in re.finditer(
        r'\{\s*"([^"]+)"\s*,\s*(CMD_[A-Z0-9_]+)\s*,',
        spec_c,
    ):
        gl_name, cmd_type = m.group(1), m.group(2)
        if cmd_type not in mapping:
            mapping[cmd_type] = gl_name

    # Fallback: k_std_command_specs[] — rows like:
    #   { "glColor3f", CMD_COLOR3F, ... }
    # (same pattern; already covered by the loop above)

    return mapping


def repl_language_section(guide: str) -> str:
    m = REPL_LANGUAGE_SECTION_RE.search(guide)
    if not m:
        raise SystemExit(
            "USER_GUIDE.md: cannot find the '## The REPL Language' section"
        )
    return m.group(1)


def line_number(text: str, name: str) -> str:
    """Return a human-readable location for the first occurrence of name."""
    idx = text.find(name)
    if idx < 0:
        return ""
    ln = text.count("\n", 0, idx) + 1
    return f":{ln}"


def main() -> int:
    root = repo_root()
    header_path    = root / "src" / "repl" / "command.h"
    desc_path      = root / "src" / "repl" / "command_descriptions.txt"
    spec_path      = root / "src" / "repl" / "command_spec.c"
    guide_path     = root / "docs" / "USER_GUIDE.md"

    header = header_path.read_text(encoding="utf-8")
    desc   = desc_path.read_text(encoding="utf-8")
    spec_c = spec_path.read_text(encoding="utf-8")
    guide  = guide_path.read_text(encoding="utf-8")

    cmd_types  = parse_cmd_types(header)
    titles     = parse_command_titles(desc, spec_c)
    section    = repl_language_section(guide)

    failures: list[str] = []

    for cmd in cmd_types:
        if cmd in EXEMPT_TYPES:
            continue

        title = titles.get(cmd)
        if title is None:
            failures.append(
                f"{cmd}: has no title in command_descriptions.txt "
                f"(add a [command {cmd}] section with title = ..., "
                f"or add {cmd!r} to EXEMPT_TYPES)"
            )
            continue

        # A command is considered documented if its title appears anywhere in
        # the REPL Language section (as a plain word, in a backtick span, as
        # a link label, etc.).  We do a simple substring search on the raw
        # markdown so author style is not constrained.
        if title not in section:
            loc = f"USER_GUIDE.md{line_number(guide, '## The REPL Language')}"
            failures.append(
                f"{cmd} ({title!r}): title not found in the "
                f"'## The REPL Language' section of {loc}"
            )

    if failures:
        print("user-guide-commands drift check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print(
            "\nTo fix: add the command name to the 'Supported GL commands' "
            "(or appropriate subsection) in docs/USER_GUIDE.md,\n"
            "or add the CmdType to EXEMPT_TYPES in this script with a reason.",
            file=sys.stderr,
        )
        return 1

    exempt_count = len(EXEMPT_TYPES)
    cmd_count    = len(cmd_types) - exempt_count
    print(
        f"user-guide-commands OK "
        f"({cmd_count} commands documented; {exempt_count} exempt)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
