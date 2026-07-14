#!/usr/bin/env python3
"""Validate USER_GUIDE's advertised built-in examples against examples/catalog.ini.

The compiled-in example set is generated from examples/catalog.ini
(scripts/gen_examples.py), so the catalog is ground truth. This guard keeps
docs/USER_GUIDE.md's "Built-in Examples" section from drifting:

  - the advertised count ("the N built-in examples") must equal the catalog
    size;
  - the numbered table in that section must list every catalog entry with
    its exact 1-based index and display name — a mid-catalog insert that
    shifts later indices fails here instead of silently misnumbering;
  - `--example "<name>"` references anywhere in the guide must name a real
    example (the CLI matches names case-insensitively);
  - bare numeric `--example <idx>` references are rejected outright: they
    break silently on any catalog insert — use the quoted-name form.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # keep scripts/__pycache__ out of the tree
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import gen_examples


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def guide_examples_section(guide: str) -> str:
    match = re.search(r"^## Built-in Examples\n(.*?)(?=^## )", guide,
                      re.MULTILINE | re.DOTALL)
    if not match:
        raise SystemExit("USER_GUIDE.md: no '## Built-in Examples' section found")
    return match.group(1)


def parse_guide_table(section: str) -> dict[int, str]:
    """Parse the numbered example table (a fenced code block laid out in
    columns: index, two spaces, name, wide gap, index, two spaces, name)."""
    fences = re.findall(r"^```\n(.*?)^```$", section, re.MULTILINE | re.DOTALL)
    if not fences:
        raise SystemExit(
            "USER_GUIDE.md: no fenced example table in the Built-in Examples section")
    table: dict[int, str] = {}
    for line in fences[0].splitlines():
        line = line.strip()
        if not line:
            continue
        tokens = re.split(r"\s{2,}", line)
        if len(tokens) % 2 != 0:
            raise SystemExit(
                f"USER_GUIDE.md example table: cannot pair index/name columns "
                f"in line {line!r} (need two or more spaces between columns)")
        for idx_tok, name in zip(tokens[0::2], tokens[1::2]):
            if not idx_tok.isdigit():
                raise SystemExit(
                    f"USER_GUIDE.md example table: expected an index, got "
                    f"{idx_tok!r} in line {line!r}")
            idx = int(idx_tok)
            if idx in table:
                raise SystemExit(
                    f"USER_GUIDE.md example table: index {idx} listed twice")
            table[idx] = name
    return table


def main() -> int:
    root = repo_root()
    guide = (root / "docs" / "USER_GUIDE.md").read_text(encoding="utf-8")

    try:
        entries = gen_examples.read_catalog(root / "examples" / "catalog.ini")
    except gen_examples.ExampleError as exc:
        print(f"examples/catalog.ini: {exc}", file=sys.stderr)
        return 1
    names = [str(entry["name"]) for entry in entries]

    failures: list[str] = []
    section = guide_examples_section(guide)

    counts = re.findall(r"the (\d+) built-in examples", section)
    if not counts:
        failures.append(
            "count claim: expected 'the N built-in examples' in the "
            "Built-in Examples section")
    for count in counts:
        if int(count) != len(names):
            failures.append(
                f"count claim: guide advertises {count} built-in examples, "
                f"catalog has {len(names)}")

    table = parse_guide_table(section)
    for idx, name in enumerate(names, start=1):
        listed = table.get(idx)
        if listed is None:
            failures.append(f"table: missing entry {idx:2d}  {name}")
        elif listed != name:
            failures.append(
                f"table: entry {idx} is {listed!r}, catalog says {name!r}")
    for idx in sorted(set(table) - set(range(1, len(names) + 1))):
        failures.append(
            f"table: entry {idx} {table[idx]!r} is beyond the catalog "
            f"({len(names)} examples)")

    known = {name.lower() for name in names}
    for ref in re.findall(r'--example "([^"]+)"', guide):
        if ref.lower() not in known:
            failures.append(
                f'--example reference: {ref!r} names no catalog example')
    for line in guide.splitlines():
        if re.search(r"--example \d", line):
            failures.append(
                f"--example reference: numeric index in {line.strip()!r} — "
                f"indices shift on catalog inserts; use --example \"<name>\"")

    if failures:
        print("USER_GUIDE example drift check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(f"user-guide-examples OK ({len(names)} examples)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
