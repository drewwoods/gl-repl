#!/usr/bin/env python3
"""Generate the compiled-in guided-tour catalog from tours/catalog.ini.

Mirrors scripts/gen_examples.py: the catalog names each tour and points at
its pointer-script file (grammar: src/app/glr_pointer_script.h); this script
embeds every script as a C string array plus one catalog table, consumed by
src/app/glr_tours.c via build/generated/glr_tours_data.inc. Section order is
the Tours menu row order. Comment/blank lines are embedded verbatim — the
runtime parser skips them, and keeping them preserves the file's line
numbering in any load-time error report.
"""

from __future__ import annotations

import argparse
import configparser
import re
import sys
from pathlib import Path


REQUIRED_KEYS = {"file", "name"}
SECTION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


class TourError(Exception):
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


def symbol_for_section(section: str) -> str:
    body = re.sub(r"[^A-Za-z0-9_]", "_", section)
    body = re.sub(r"_+", "_", body).strip("_")
    if not body:
        raise TourError(f"section [{section}] does not form a C symbol")
    return f"g_tour_{body}"


def read_catalog(catalog_path: Path) -> list[dict[str, object]]:
    base_dir = catalog_path.parent.resolve()

    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    try:
        with catalog_path.open("r", encoding="utf-8") as f:
            parser.read_file(f)
    except configparser.Error as exc:
        raise TourError(f"{catalog_path}: {exc}") from exc
    except OSError as exc:
        raise TourError(f"{catalog_path}: {exc}") from exc

    if parser.defaults():
        raise TourError("catalog must not use [DEFAULT] values")

    seen_names: dict[str, str] = {}
    seen_files: dict[Path, str] = {}
    seen_symbols: dict[str, str] = {}
    entries: list[dict[str, object]] = []

    for section in parser.sections():
        if not SECTION_RE.match(section):
            raise TourError(
                f"[{section}] section id must use letters, numbers, '.', '_' or '-'"
            )

        keys = set(parser[section].keys())
        missing = REQUIRED_KEYS - keys
        extra = keys - REQUIRED_KEYS
        if missing:
            raise TourError(f"[{section}] missing required key(s): {', '.join(sorted(missing))}")
        if extra:
            raise TourError(f"[{section}] unknown key(s): {', '.join(sorted(extra))}")

        name = parser[section]["name"].strip()
        rel_file = parser[section]["file"].strip()
        if not name:
            raise TourError(f"[{section}] name must not be empty")
        if not rel_file:
            raise TourError(f"[{section}] file must not be empty")

        name_key = name.lower()
        if name_key in seen_names:
            raise TourError(
                f"[{section}] duplicate tour name {name!r}; first used by [{seen_names[name_key]}]"
            )
        seen_names[name_key] = section

        file_path = (base_dir / rel_file).resolve()
        try:
            file_path.relative_to(base_dir)
        except ValueError as exc:
            raise TourError(f"[{section}] file must live under tours/") from exc
        if file_path.suffix != ".pointer":
            raise TourError(f"[{section}] file must have a .pointer extension")
        if not file_path.is_file():
            raise TourError(f"[{section}] missing tour script: {rel_file}")
        if file_path in seen_files:
            raise TourError(
                f"[{section}] duplicate tour script {rel_file}; first used by [{seen_files[file_path]}]"
            )
        seen_files[file_path] = section

        symbol = symbol_for_section(section)
        if symbol in seen_symbols:
            raise TourError(
                f"[{section}] generated symbol collision with [{seen_symbols[symbol]}]: {symbol}"
            )
        seen_symbols[symbol] = section

        lines = file_path.read_text(encoding="utf-8").splitlines()
        if not any(line.strip() and not line.lstrip().startswith("#") for line in lines):
            raise TourError(f"[{section}] tour script has no events: {rel_file}")

        entries.append(
            {
                "section": section,
                "name": name,
                "file": rel_file,
                "symbol": symbol,
                "lines": lines,
            }
        )

    if not entries:
        raise TourError("catalog must contain at least one tour")
    return entries


def render(entries: list[dict[str, object]]) -> str:
    out: list[str] = [
        "/* Generated by scripts/gen_tours.py. Do not edit. */",
        "",
    ]
    for entry in entries:
        out.append(f"/* {entry['name']} (tours/{entry['file']}) */")
        out.append(f"static const char *const {entry['symbol']}[] = {{")
        for line in entry["lines"]:  # type: ignore[index]
            out.append(f"    {c_string(str(line))},")
        out.append("};")
        out.append("")

    out.append("static const TourEntry g_tours[] = {")
    for entry in entries:
        out.append(f"    {{ {c_string(str(entry['name']))}, {entry['symbol']},")
        out.append(
            f"      (int)(sizeof({entry['symbol']}) / sizeof({entry['symbol']}[0])) }},"
        )
    out.append("};")
    out.append("")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--catalog", type=Path, default=repo_root() / "tours" / "catalog.ini")
    ap.add_argument("--out", type=Path, help="write the generated include here")
    ap.add_argument("--check", action="store_true", help="validate the catalog only")
    args = ap.parse_args()

    try:
        entries = read_catalog(args.catalog.resolve())
    except TourError as exc:
        print(f"gen_tours: {exc}", file=sys.stderr)
        return 1

    if args.check:
        print(f"tours catalog OK ({len(entries)} tour(s))")
        return 0

    if not args.out:
        print("gen_tours: --out is required unless --check", file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(render(entries), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
