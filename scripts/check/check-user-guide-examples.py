#!/usr/bin/env python3
"""Validate documented built-in examples against examples/catalog.ini.

The compiled-in example set is generated from examples/catalog.ini
(scripts/gen_examples.py), so the catalog is ground truth. This guard keeps
example references and advertised counts in README.md plus the top-level
docs/*.md files from drifting, and also checks docs/USER_GUIDE.md's
"Built-in Examples" section:

  - advertised counts (for example, "N built-in scenes" or "F12 cycles all
    N") must equal the catalog size;
  - docs/SHOWCASE.md must link every catalog scene file, so adding an example
    cannot silently leave the "full showcase" incomplete;
  - each catalog scene's Showcase tile (or feature section) must embed a local
    PNG, JPEG, or GIF, and that media file must exist;
  - `--example "<name>"` references in any top-level docs/*.md file must name
    a real example (the CLI matches names case-insensitively);
  - bare numeric `--example <idx>` references in those docs are rejected: they
    break silently on any catalog insert — use the quoted-name form.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # keep scripts/__pycache__ out of the tree
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import gen_examples


QUOTED_EXAMPLE_RE = re.compile(
    r"--example\s+(?:\"([^\"\n]+)\"|'([^'\n]+)')")
NUMERIC_EXAMPLE_RE = re.compile(r"--example\s+\d+")
SHOWCASE_IMAGE_RE = re.compile(
    r'<img\b[^>]*\bsrc="([^"?#]+\.(?:png|jpe?g|gif))"', re.IGNORECASE)
COUNT_CLAIM_RE = re.compile(
    r"\b(?P<built_in>\d+)\s+built-in\s+(?:examples|scenes)\b"
    r"|\bF12\*{0,2}\s+cycles(?:\s+\w+){0,4}\s+all\s+(?P<f12>\d+)\b",
    re.IGNORECASE)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def guide_examples_section(guide: str) -> str:
    match = re.search(r"^## Built-in Examples\n(.*?)(?=^## )", guide,
                      re.MULTILINE | re.DOTALL)
    if not match:
        raise SystemExit("USER_GUIDE.md: no '## Built-in Examples' section found")
    return match.group(1)


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def doc_count_claim_failures(path: Path, text: str,
                             catalog_count: int) -> list[str]:
    """Return advertised example-count claims that disagree with the catalog."""
    failures: list[str] = []
    for match in COUNT_CLAIM_RE.finditer(text):
        count = int(match.group("built_in") or match.group("f12"))
        if count != catalog_count:
            failures.append(
                f"{path.name}:{line_number(text, match.start())}: count claim "
                f"advertises {count} built-in examples, catalog has "
                f"{catalog_count}")
    return failures


def doc_reference_failures(path: Path, text: str,
                           known: set[str]) -> list[str]:
    """Return stale or insertion-fragile --example references in one doc."""
    failures: list[str] = []
    for match in QUOTED_EXAMPLE_RE.finditer(text):
        ref = match.group(1) or match.group(2)
        # Documentation syntax placeholders are not catalog references.
        if ref.startswith("<") and ref.endswith(">"):
            continue
        if ref.lower() not in known:
            failures.append(
                f"{path.name}:{line_number(text, match.start())}: "
                f"--example reference {ref!r} names no catalog example")

    for match in NUMERIC_EXAMPLE_RE.finditer(text):
        line_start = text.rfind("\n", 0, match.start()) + 1
        line_end = text.find("\n", match.end())
        if line_end < 0:
            line_end = len(text)
        failures.append(
            f"{path.name}:{line_number(text, match.start())}: numeric index in "
            f"{text[line_start:line_end].strip()!r} — indices shift on catalog "
            f'inserts; use --example "<name>"')
    return failures


def showcase_media_scope(text: str, offset: int) -> str:
    """Return the gallery tile or feature section containing an entry link."""
    tile_start = text.rfind("<td", 0, offset)
    tile_end = text.find("</td>", offset)
    if tile_start >= 0 and tile_end >= offset:
        return text[tile_start:tile_end]

    section_start = text.rfind("\n### ", 0, offset)
    section_end = text.find("\n### ", offset)
    if section_end < 0:
        section_end = len(text)
    return text[section_start if section_start >= 0 else 0:section_end]


def showcase_entry_failures(showcase_path: Path, text: str,
                            entries: list[dict[str, object]]) -> list[str]:
    """Return catalog scenes without a Showcase link or local gallery media."""
    linked_scenes = set(re.findall(
        r"\]\((\.\./examples/[^)\s]+)\)", text))
    failures: list[str] = []
    for entry in entries:
        expected = f"../examples/{entry['file']}"
        if expected not in linked_scenes:
            failures.append(
                f"SHOWCASE.md: missing entry for {entry['name']!r} "
                f"(expected link {expected})")
            continue

        link_offset = text.find(f"]({expected})")
        scope = showcase_media_scope(text, link_offset)
        media = SHOWCASE_IMAGE_RE.findall(scope)
        if not media:
            failures.append(
                f"SHOWCASE.md: {entry['name']!r} has no PNG, JPEG, or GIF "
                "in its Showcase tile or feature section")
            continue
        for src in media:
            media_path = (showcase_path.parent / src).resolve()
            if not media_path.is_file():
                failures.append(
                    f"SHOWCASE.md: {entry['name']!r} references missing media "
                    f"{src}")
    return failures


def main() -> int:
    root = repo_root()
    docs = [root / "README.md", *sorted((root / "docs").glob("*.md"))]
    guide_path = root / "docs" / "USER_GUIDE.md"
    showcase_path = root / "docs" / "SHOWCASE.md"
    guide = guide_path.read_text(encoding="utf-8")
    showcase = showcase_path.read_text(encoding="utf-8")

    try:
        entries = gen_examples.read_catalog(root / "examples" / "catalog.ini")
    except gen_examples.ExampleError as exc:
        print(f"examples/catalog.ini: {exc}", file=sys.stderr)
        return 1
    names = [str(entry["name"]) for entry in entries]

    failures: list[str] = []
    section = guide_examples_section(guide)
    failures.extend(showcase_entry_failures(showcase_path, showcase, entries))

    if not COUNT_CLAIM_RE.search(section):
        failures.append(
            "count claim: expected an advertised built-in-example count in the "
            "Built-in Examples section")

    known = {name.lower() for name in names}
    for path in docs:
        text = guide if path == guide_path else path.read_text(encoding="utf-8")
        failures.extend(doc_count_claim_failures(path, text, len(names)))
        failures.extend(doc_reference_failures(path, text, known))

    if failures:
        print("documentation example drift check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(f"doc-examples OK ({len(names)} examples; {len(docs)} docs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
