#!/usr/bin/env python3
"""check-palette: hold the shared accent palette's consumers in sync.

accent_palette.h (repo root) is the single source of truth for the
scene/brand accent palette: named anchor sets, the active-palette select,
and the semantic role map C consumers bind to. This check verifies the
two consumers that cannot bind at compile time:

  1. examples/README.md — the "Example Color Language" table must list
     exactly the active anchors with exactly the header's RGB values.
  2. examples/scenes/*.{glr,c} — every pure-literal color triple
     (glColor3f / glColor4f / glClearColor) in a covered scene must be an
     active anchor. Scenes not yet migrated to the palette are listed in
     the file-level ratchet baseline scripts/baselines/palette-coverage.txt:
     they are skipped, but a baseline entry that becomes fully on-palette
     must be removed (ratchet down only), and stale entries fail.
     Exception: the logo scene (examples/scenes/glr-logo.glr — the mark's
     source of truth) validates against the active anchors PLUS the
     brand-mark anchors, so its materials and the mark stay in sync.
  3. docs/images/logo.svg — the mark's hand-traced vector twin: every
     hex color must be a brand-mark / active-anchor hex, pure black or
     white, or a dark ground tone (every channel <= 0x33). Only this SVG
     is covered by decision; the SMIL logo (logo-rotating.svg) is
     generated art owned by scripts/gen_rotating_cube.py, and the other
     packaging/docs SVGs are uncanonized explorations.

Role map sanity is also checked: every PAL_ROLE_* must alias an anchor
that exists in the header (guards a role pointing at a deleted anchor
after a palette swap).

`--list` prints the active anchors (floats + hex) and the role map — the
reference for SVG/doc work (make palette-list).
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PALETTE_H = os.path.join(ROOT, "accent_palette.h")
README = os.path.join(ROOT, "examples", "README.md")
SCENES_DIR = os.path.join(ROOT, "examples", "scenes")
BASELINE = os.path.join(ROOT, "scripts", "baselines", "palette-coverage.txt")

# The logo scene is the brand mark's source of truth; it may (must) use
# the brand-mark materials on top of the active accents.
LOGO_SCENE = "examples/scenes/glr-logo.glr"

# Covered brand vector art (repo-relative). Only the canonical logo SVG
# by decision — see the module docstring.
BRAND_SVGS = ["docs/images/logo.svg"]

TOL = 1e-4  # float-compare tolerance for scene literals vs anchors

RED = "\033[0;31m" if sys.stdout.isatty() else ""
GREEN = "\033[0;32m" if sys.stdout.isatty() else ""
NC = "\033[0m" if sys.stdout.isatty() else ""


def fail(msg):
    print(f"{RED}ERROR:{NC} {msg}")
    return 1


def parse_anchor_list(text, list_name):
    """Parse one X-macro anchor list into an ordered {NAME: (r, g, b)}."""
    m = re.search(r"#define\s+" + list_name + r"\(X\)\s*\\\n(.*?)\n\n",
                  text, re.S)
    if not m:
        sys.exit(fail(f"accent_palette.h: anchor list {list_name} not found"))
    anchors = {}
    for am in re.finditer(
            r"X\(\s*(\w+)\s*,\s*([\d.]+)f?\s*,\s*([\d.]+)f?\s*,\s*([\d.]+)f?\s*\)",
            m.group(1)):
        anchors[am.group(1)] = tuple(float(am.group(i)) for i in (2, 3, 4))
    if not anchors:
        sys.exit(fail(f"accent_palette.h: no X(...) anchors parsed from {list_name}"))
    return anchors


def parse_palette_header():
    """Return (name, anchors, mark, roles): anchors/mark are ordered
    {NAME: (r, g, b)}, roles is an ordered {ROLE: ANCHOR}."""
    text = open(PALETTE_H).read()

    m = re.search(r"#define\s+PALETTE_ACTIVE_NAME\s+\"([^\"]+)\"", text)
    if not m:
        sys.exit(fail("accent_palette.h: PALETTE_ACTIVE_NAME not found"))
    name = m.group(1)

    m = re.search(r"#define\s+PALETTE_ACTIVE_ANCHORS\s+(PALETTE_\w+_ANCHORS)", text)
    if not m:
        sys.exit(fail("accent_palette.h: PALETTE_ACTIVE_ANCHORS not found"))
    anchors = parse_anchor_list(text, m.group(1))
    mark = parse_anchor_list(text, "PALETTE_MARK_ANCHORS")

    roles = {}
    for rm in re.finditer(r"#define\s+(PAL_ROLE_\w+)\s+PAL_(\w+)", text):
        roles[rm.group(1)] = rm.group(2)
    return name, anchors, mark, roles


def check_roles(anchors, roles):
    errs = 0
    for role, anchor in roles.items():
        if anchor not in anchors:
            errs += fail(f"accent_palette.h: {role} aliases PAL_{anchor}, "
                         f"which is not in the active anchor set")
    return errs


def check_readme(name, anchors):
    """The README table must carry exactly the active anchors/values."""
    text = open(README).read()
    rows = {}
    for m in re.finditer(
            r"^\|\s*([A-Za-z]+)\s*\|\s*`([\d.]+)\s+([\d.]+)\s+([\d.]+)`\s*\|",
            text, re.M):
        if m.group(1).lower() == "role":
            continue
        rows[m.group(1).upper()] = tuple(float(m.group(i)) for i in (2, 3, 4))

    errs = 0
    for aname, argb in anchors.items():
        if aname not in rows:
            errs += fail(f"examples/README.md: palette table is missing "
                         f"anchor {aname} (accent_palette.h has it)")
        elif any(abs(a - b) > TOL for a, b in zip(argb, rows[aname])):
            errs += fail(f"examples/README.md: {aname} is {rows[aname]}, "
                         f"but accent_palette.h says {argb}")
    for rname in rows:
        if rname not in anchors:
            errs += fail(f"examples/README.md: table row {rname} is not an "
                         f"anchor of the active \"{name}\" palette")
    return errs


# Pure-literal color-call triples only: arithmetic / variable args are
# expression-driven color (lerp ramps) whose endpoints are the anchors
# appearing elsewhere as literals — they are out of contract here.
COLOR_CALL = re.compile(
    r"\bgl(?:Color[34]f|ClearColor)\s*\(\s*"
    r"(-?[\d.]+)f?\s*,\s*(-?[\d.]+)f?\s*,\s*(-?[\d.]+)f?\s*[,)]")


def scene_off_palette(path, anchors):
    """Return the list of (lineno, triple) literals not matching an anchor."""
    off = []
    for lineno, line in enumerate(open(path).read().splitlines(), 1):
        code = line.split("//", 1)[0]
        for m in COLOR_CALL.finditer(code):
            try:
                trip = tuple(float(x) for x in m.groups())
            except ValueError:
                continue
            if not any(all(abs(a - b) <= TOL for a, b in zip(trip, argb))
                       for argb in anchors.values()):
                off.append((lineno, trip))
    return off


def check_scenes(anchors, mark):
    baseline = set()
    if os.path.exists(BASELINE):
        for line in open(BASELINE):
            line = line.strip()
            if line and not line.startswith("#"):
                baseline.add(line)

    errs = 0
    seen = set()
    for fn in sorted(os.listdir(SCENES_DIR)):
        if not fn.endswith((".glr", ".c")):
            continue
        rel = f"examples/scenes/{fn}"
        seen.add(rel)
        allowed = anchors if rel != LOGO_SCENE else {**anchors, **mark}
        off = scene_off_palette(os.path.join(SCENES_DIR, fn), allowed)
        if rel in baseline:
            if not off:
                errs += fail(f"{rel}: fully on-palette now — remove it from "
                             f"scripts/baselines/palette-coverage.txt (ratchet)")
        elif off:
            for lineno, trip in off:
                errs += fail(f"{rel}:{lineno}: literal color "
                             f"({trip[0]:g}, {trip[1]:g}, {trip[2]:g}) is not an "
                             f"anchor of the active palette (accent_palette.h); use an "
                             f"anchor, or add the file to the coverage baseline "
                             f"if it is intentionally off-palette")
    for rel in sorted(baseline - seen):
        errs += fail(f"palette-coverage baseline entry has no file: {rel}")
    return errs


def rgb_hex(rgb):
    return "#%02x%02x%02x" % tuple(round(v * 255) for v in rgb)


def hex_set(anchor_map):
    return {rgb_hex(rgb) for rgb in anchor_map.values()}


def is_dark_ground(hexv):
    """Dark chrome/ground tones are free in brand art (icon squircle
    gradients etc.): every channel <= 0x33."""
    return all(int(hexv[i:i + 2], 16) <= 0x33 for i in (1, 3, 5))


# Hex colors in SVG text, skipping url(#id) references. 3-digit form is
# normalized by the caller.
SVG_HEX = re.compile(r"(?<!url\()#([0-9a-fA-F]{6}|[0-9a-fA-F]{3})\b")


def check_brand_svgs(anchors, mark):
    allowed = hex_set(anchors) | hex_set(mark) | {"#000000", "#ffffff"}
    errs = 0
    for rel in BRAND_SVGS:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            errs += fail(f"{rel}: covered brand asset is missing")
            continue
        text = open(path).read()
        bad = set()
        for m in SVG_HEX.finditer(text):
            h = m.group(1).lower()
            if len(h) == 3:
                h = "".join(c * 2 for c in h)
            hexv = "#" + h
            if hexv not in allowed and not is_dark_ground(hexv):
                bad.add(hexv)
        for hexv in sorted(bad):
            errs += fail(f"{rel}: color {hexv} is not in the brand-mark / "
                         f"active-anchor hex vocabulary (accent_palette.h)")
    return errs


def list_palette(name, anchors, mark, roles):
    print(f'Active palette: "{name}"  ({len(anchors)} anchors, accent_palette.h)')
    for aname, rgb in anchors.items():
        print(f"  {aname:<8} {rgb[0]:.2f} {rgb[1]:.2f} {rgb[2]:.2f}   {rgb_hex(rgb)}")
    print(f"Brand mark ({len(mark)} anchors; source scene {LOGO_SCENE}):")
    for aname, rgb in mark.items():
        print(f"  {aname:<14} {rgb[0]:.3g} {rgb[1]:.3g} {rgb[2]:.3g}   {rgb_hex(rgb)}")
    print("Roles:")
    for role, anchor in roles.items():
        print(f"  {role:<22} -> {anchor}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true",
                    help="print anchors (floats + hex) and the role map")
    args = ap.parse_args()

    name, anchors, mark, roles = parse_palette_header()
    if args.list:
        list_palette(name, anchors, mark, roles)
        return 0

    errs = 0
    errs += check_roles({**anchors, **mark}, roles)
    errs += check_readme(name, anchors)
    errs += check_scenes(anchors, mark)
    errs += check_brand_svgs(anchors, mark)
    if errs:
        print(f"{RED}check-palette: {errs} problem(s){NC}")
        return 1
    print(f"Palette contract (accent_palette.h vs README + scenes + logo svg) {GREEN}OK{NC}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
