#!/usr/bin/env python3
"""Generate a compiled-in guided-tour catalog from tours/catalog*.ini.

Mirrors scripts/gen_examples.py: the catalog names each tour and points at
its pointer-script file (grammar: src/app/glr_pointer_script.h); this script
embeds every script as a C string array plus one catalog table, consumed by
src/app/glr_tours.c via build/generated/glr_tours_data.inc. Section order is
the Tours menu row order. Comment/blank lines and conditional lines are
embedded verbatim — the runtime parser skips inactive branches, and keeping
them preserves the file's line numbering in any load-time error report.
"""

from __future__ import annotations

import argparse
import configparser
import re
import sys
from pathlib import Path


REQUIRED_KEYS = {"file", "name"}
SECTION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
CONDITIONAL_RE = re.compile(r"^\s*#\s*(ifdef|ifndef|else|endif)\b(.*)$")
UNSUPPORTED_CONDITIONAL_RE = re.compile(r"^\s*#\s*(if|elif|define|undef)\b")
SUPPORTED_MACRO = "__EMSCRIPTEN__"


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


def active_script_lines(
    lines: list[str], *, platform: str, source: str
) -> list[tuple[int, str]]:
    """Validate tour conditionals and return the selected physical lines.

    Conditional lines remain in the generated C array so runtime error/HUD
    source lines still refer to the author's file. This helper only selects
    the branch used by the requested catalog for generator-side validation.
    """

    # parent-active, condition-selected, else-seen, opening line
    stack: list[tuple[bool, bool, bool, int]] = []
    active = True
    selected: list[tuple[int, str]] = []

    for lineno, line in enumerate(lines, start=1):
        match = CONDITIONAL_RE.match(line)
        if match:
            directive, rest = match.groups()
            rest = rest.strip()
            if directive in {"ifdef", "ifndef"}:
                tokens = rest.split()
                if not tokens or tokens[0] != SUPPORTED_MACRO:
                    raise TourError(
                        f"{source}:{lineno}: conditional must name "
                        f"{SUPPORTED_MACRO}"
                    )
                if (
                    len(tokens) > 1
                    and not rest[len(tokens[0]):].lstrip().startswith("#")
                ):
                    raise TourError(
                        f"{source}:{lineno}: unexpected text after #{directive}"
                    )
                if len(stack) >= 16:
                    raise TourError(f"{source}:{lineno}: conditional nesting is too deep")
                condition = (platform == "emscripten")
                if directive == "ifndef":
                    condition = not condition
                stack.append((active, condition, False, lineno))
                active = active and condition
                continue

            if rest and not rest.startswith("#"):
                raise TourError(
                    f"{source}:{lineno}: unexpected text after #{directive}"
                )
            if directive == "else":
                if not stack:
                    raise TourError(f"{source}:{lineno}: #else without a conditional")
                parent_active, condition, else_seen, opened_at = stack[-1]
                if else_seen:
                    raise TourError(f"{source}:{lineno}: duplicate #else")
                stack[-1] = (parent_active, condition, True, opened_at)
                active = parent_active and not condition
                continue

            if directive == "endif":
                if not stack:
                    raise TourError(f"{source}:{lineno}: #endif without a conditional")
                parent_active, _condition, _else_seen, _opened_at = stack.pop()
                active = parent_active
                continue

        elif UNSUPPORTED_CONDITIONAL_RE.match(line):
            raise TourError(
                f"{source}:{lineno}: use #ifdef/#ifndef/#else/#endif; "
                "other preprocessor directives are not supported"
            )

        if active:
            selected.append((lineno, line))

    if stack:
        raise TourError(f"{source}:{stack[-1][3]}: unterminated conditional")
    return selected


def read_catalog(catalog_path: Path, *, platform: str) -> list[dict[str, object]]:
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
        validation_platforms = [platform]
        validation_platforms.extend(
            other for other in ("native", "emscripten") if other != platform
        )
        for validation_platform in validation_platforms:
            selected_lines = active_script_lines(
                lines, platform=validation_platform, source=str(file_path)
            )
            if not any(
                line.strip() and not line.lstrip().startswith("#")
                for _lineno, line in selected_lines
            ):
                raise TourError(
                    f"[{section}] tour script has no events for "
                    f"{validation_platform}: {rel_file}"
                )

            # Controlled tours (glr_pointer_script_start_tour) are untimed,
            # completion-driven scripts. A leading timestamp selects the
            # legacy absolute-time grammar the transport controls cannot
            # step; the runtime rejects it too, but failing here keeps a bad
            # catalog out of the build. A comment/blank line never counts as
            # an executable line.
            for lineno, line in selected_lines:
                stripped = line.lstrip()
                if not stripped or stripped.startswith("#"):
                    continue
                first = stripped.split(None, 1)[0]
                try:
                    float(first)
                except ValueError:
                    continue
                raise TourError(
                    f"[{section}] {rel_file}:{lineno}: tour scripts must use "
                    f"untimed, completion-driven events (found timestamp {first!r})"
                )

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
        out.append(
            f"    {{ {c_string(str(entry['name']))}, "
            f"{c_string(str(entry['file']))}, {entry['symbol']},"
        )
        out.append(
            f"      (int)(sizeof({entry['symbol']}) / sizeof({entry['symbol']}[0])) }},"
        )
    out.append("};")
    out.append("")
    return "\n".join(out)


def write_if_changed(path: Path, text: str) -> None:
    # The Makefile rule is FORCE-driven, so this runs on every build; only touch
    # the mtime when the content actually moved, or glr_tours.o recompiles and
    # every test binary relinks each run.
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--catalog", type=Path, default=repo_root() / "tours" / "catalog.ini")
    ap.add_argument(
        "--platform",
        choices=("native", "emscripten"),
        default="native",
        help="platform used to validate #ifdef branches (default: native)",
    )
    ap.add_argument("--out", type=Path, help="write the generated include here")
    ap.add_argument("--check", action="store_true", help="validate the catalog only")
    args = ap.parse_args()

    try:
        entries = read_catalog(args.catalog.resolve(), platform=args.platform)
    except TourError as exc:
        print(f"gen_tours: {exc}", file=sys.stderr)
        return 1

    if args.check:
        print(f"tours catalog OK ({len(entries)} tour(s))")
        return 0

    if not args.out:
        print("gen_tours: --out is required unless --check", file=sys.stderr)
        return 1

    write_if_changed(args.out, render(entries))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
