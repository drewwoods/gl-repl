#!/usr/bin/env python3
"""Generate the compiled-in built-in example catalog from examples/catalog.ini."""

from __future__ import annotations

import argparse
import configparser
import re
import sys
from pathlib import Path


TAG_MACROS = {
    "2D": "EXAMPLE_TAG_2D",
    "3D": "EXAMPLE_TAG_3D",
    "Polygons": "EXAMPLE_TAG_POLYGONS",
    "Lines": "EXAMPLE_TAG_LINES",
}

REQUIRED_KEYS = {"file", "name", "tags", "group"}
SECTION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


class ExampleError(Exception):
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
        raise ExampleError(f"section [{section}] does not form a C symbol")
    return f"g_example_{body}"


def parse_tags(section: str, value: str) -> list[str]:
    tags = [part.strip() for part in value.split(",") if part.strip()]
    if not tags:
        raise ExampleError(f"[{section}] tags must not be empty")
    macros: list[str] = []
    for tag in tags:
        if tag == "All":
            raise ExampleError(f"[{section}] must not list synthetic tag All")
        macro = TAG_MACROS.get(tag)
        if not macro:
            known = ", ".join(TAG_MACROS)
            raise ExampleError(f"[{section}] unknown tag {tag!r}; expected one of: {known}")
        macros.append(macro)
    return macros


def read_catalog(catalog_path: Path) -> list[dict[str, object]]:
    base_dir = catalog_path.parent.resolve()
    scenes_dir = (base_dir / "scenes").resolve()

    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    try:
        with catalog_path.open("r", encoding="utf-8") as f:
            parser.read_file(f)
    except configparser.Error as exc:
        raise ExampleError(f"{catalog_path}: {exc}") from exc
    except OSError as exc:
        raise ExampleError(f"{catalog_path}: {exc}") from exc

    if parser.defaults():
        raise ExampleError("catalog must not use [DEFAULT] values")

    seen_names: dict[str, str] = {}
    seen_files: dict[Path, str] = {}
    seen_symbols: dict[str, str] = {}
    entries: list[dict[str, object]] = []

    for section in parser.sections():
        if not SECTION_RE.match(section):
            raise ExampleError(
                f"[{section}] section id must use letters, numbers, '.', '_' or '-'"
            )

        keys = set(parser[section].keys())
        missing = REQUIRED_KEYS - keys
        extra = keys - REQUIRED_KEYS
        if missing:
            raise ExampleError(f"[{section}] missing required key(s): {', '.join(sorted(missing))}")
        if extra:
            raise ExampleError(f"[{section}] unknown key(s): {', '.join(sorted(extra))}")

        name = parser[section]["name"].strip()
        group = parser[section]["group"].strip()
        rel_file = parser[section]["file"].strip()
        if not name:
            raise ExampleError(f"[{section}] name must not be empty")
        if not group:
            raise ExampleError(f"[{section}] group must not be empty")
        if not rel_file:
            raise ExampleError(f"[{section}] file must not be empty")

        name_key = name.lower()
        if name_key in seen_names:
            raise ExampleError(
                f"[{section}] duplicate example name {name!r}; first used by [{seen_names[name_key]}]"
            )
        seen_names[name_key] = section

        file_path = (base_dir / rel_file).resolve()
        try:
            file_path.relative_to(scenes_dir)
        except ValueError as exc:
            raise ExampleError(f"[{section}] file must live under examples/scenes") from exc
        if file_path.suffix != ".c":
            raise ExampleError(f"[{section}] file must have a .c extension")
        if not file_path.is_file():
            raise ExampleError(f"[{section}] missing scene file: {rel_file}")
        if file_path in seen_files:
            raise ExampleError(
                f"[{section}] duplicate scene file {rel_file}; first used by [{seen_files[file_path]}]"
            )
        seen_files[file_path] = section

        symbol = symbol_for_section(section)
        if symbol in seen_symbols:
            raise ExampleError(
                f"[{section}] generated symbol collision with [{seen_symbols[symbol]}]: {symbol}"
            )
        seen_symbols[symbol] = section

        tags = parse_tags(section, parser[section]["tags"])
        lines = file_path.read_text(encoding="utf-8").splitlines()

        entries.append(
            {
                "section": section,
                "name": name,
                "group": group,
                "file": rel_file,
                "symbol": symbol,
                "tags": tags,
                "lines": lines,
            }
        )

    if not entries:
        raise ExampleError("catalog must contain at least one example")
    return entries


def render(entries: list[dict[str, object]]) -> str:
    out: list[str] = [
        "/* Generated by scripts/gen_examples.py. Do not edit. */",
        "",
    ]
    for entry in entries:
        out.append(f"/* {entry['name']} ({entry['file']}) */")
        out.append(f"static const char *const {entry['symbol']}[] = {{")
        for line in entry["lines"]:  # type: ignore[index]
            out.append(f"    {c_string(str(line))},")
        out.append("    NULL")
        out.append("};")
        out.append("")

    out.append("static const ReplExampleEntry g_example_entries[] = {")
    for entry in entries:
        tag_expr = " | ".join(entry["tags"])  # type: ignore[arg-type]
        out.append(f"    {{ {c_string(str(entry['name']))}, {entry['symbol']},")
        out.append(f"      {tag_expr},")
        out.append(f"      {c_string(str(entry['group']))} }},")
    out.append("};")
    out.append("")
    return "\n".join(out)


def write_if_changed(path: Path, text: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def main(argv: list[str]) -> int:
    root = repo_root()
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", default=str(root / "examples/catalog.ini"))
    parser.add_argument("--out", default=str(root / "build/generated/repl_examples_data.inc"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)

    try:
        entries = read_catalog(Path(args.catalog))
        if not args.check:
            write_if_changed(Path(args.out), render(entries))
    except ExampleError as exc:
        print(f"gen_examples.py: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
