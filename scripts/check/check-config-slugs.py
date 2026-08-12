#!/usr/bin/env python3
"""Check scene @cfg headers against the app's authoritative config table.

The list comes from ``gl-repl --list-config`` rather than from a second parser
for ``g_cfg_items[]``.  Its tab-separated output is intentionally small enough
to consume from shell or Python while keeping the C descriptor table as the
single source of truth.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

CFG_RE = re.compile(r"^\s*//\s*@cfg\s+([A-Za-z0-9_]+)\s*=")


def usage() -> None:
    print(
        "usage: check-config-slugs.py GL_REPL [SCENE_DIR ...]",
        file=sys.stderr,
    )


def config_slugs(binary: str) -> set[str]:
    try:
        text = subprocess.check_output([binary, "--list-config"], text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"could not run {binary} --list-config: {exc}") from exc

    lines = text.splitlines()
    if not lines or lines[0] != "slug\tlabel\tstates":
        raise SystemExit(
            f"{binary} --list-config did not produce the expected TSV header")

    known: set[str] = set()
    for row in lines[1:]:
        fields = row.split("\t")
        if len(fields) != 3 or not fields[0]:
            raise SystemExit(f"malformed config-list row: {row!r}")
        known.add(fields[0])
    return known


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        usage()
        return 2

    known = config_slugs(argv[1])
    roots = [Path(arg) for arg in argv[2:]] or [Path("examples/scenes")]
    failures = 0
    for root in roots:
        if not root.is_dir():
            print(f"{root}: not a scene directory", file=sys.stderr)
            failures += 1
            continue
        for path in sorted(root.rglob("*.glr")):
            for line_no, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), 1
            ):
                match = CFG_RE.match(line)
                if match and match.group(1) not in known:
                    print(
                        f"{path}:{line_no}: unknown @cfg slug {match.group(1)!r}",
                        file=sys.stderr,
                    )
                    failures += 1
    if failures:
        print(f"config slug check failed: {failures} problem(s)", file=sys.stderr)
        return 1
    print(f"config slugs OK ({len(known)} known; {', '.join(str(r) for r in roots)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
