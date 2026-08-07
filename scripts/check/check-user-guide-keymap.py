#!/usr/bin/env python3
"""Validate USER_GUIDE shortcut claims against scripts/keymap.sh list output."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


CLAIMS: list[tuple[str, str, list[str]]] = [
    ("Config menu shortcut", "GLR_CONFIG_MENU",
     ["Open the **Config** dropdown (or press **{key}**)"]),
    ("Variable panel shortcut", "GLR_VARIABLE_PANEL",
     ["Toggle the panel with **Backquote** (`` {key} ``)",
      "| Backquote (`` {key} ``) | Variable panel |"]),
    ("Code focus shortcut", "GLR_CODE_FOCUS",
     ["| {key} | Toggle code focus"]),
    ("Focus origin shortcut", "GLR_FOCUS_ORIGIN",
     ["| {key} | Focus origin"]),
    ("View mode shortcut", "GLR_VIEW_MODE",
     ["*View mode* ({key}, or the CAMERA section",
      "| {key} | 2D / 3D view mode |"]),
    ("Grid theme shortcut", "GLR_GRID",
     ["| {key} | Grid theme |",
      "grid themes (**{key}**)"]),
    ("Grid extent shortcut", "GLR_GRID_EXTENT",
     ["| {key} | Grid extent |",
      "**Grid extent** ({key})"]),
    ("Grid brightness shortcut", "GLR_GRID_BRIGHTNESS",
     ["| {key} | Grid brightness |",
      "**Grid brightness** ({key})"]),
    ("Grid major shortcut", "GLR_GRID_MAJOR",
     ["**Grid major** ({key})",
      "| {key} | Grid major spacing |"]),
    ("Axes shortcut", "GLR_AXES",
     ["| {key} | Axes theme |",
      "axes themes (**{key}**)"]),
    ("Backdrop shortcut", "GLR_BACKDROP",
     ["| {key} | Backdrop |",
      "**{key}** cycles the scene backdrop"]),
    ("Vertex labels shortcut", "GLR_VERTEX_LABELS",
     ["| {key} | Vertex labels |",
      "**Vertex labels** ({key})"]),
    ("Overlay scope shortcut", "GLR_OVERLAY_SCOPE",
     ["| {key} | Overlay scope |",
      "**Overlay scope** ({key})"]),
    ("Light theme shortcut", "GLR_LIGHT_THEME",
     ["| {key} | Light theme |",
      "**Light themes** (**{key}**)"]),
    ("Syntax highlight shortcut", "GLR_SYNTAX_HL",
     ["| {key} | Cycle syntax highlight |"]),
    ("Accum effect shortcut", "GLR_ACCUM_EFFECT",
     ["**Accum effect** ({key})",
      "| {key} | Accum effect |"]),
    ("Transform guides shortcut", "GLR_XFORM_GUIDES",
     ["With **Transform guides** on ({key})",
      "| {key} | Transform guides |"]),
    ("Vertex outlines shortcut", "GLR_VERTEX_OUTLINES",
     ["**Vertex outlines** ({key})",
      "| {key} | Vertex outlines |"]),
    ("Projection shortcut", "GLR_PROJECTION",
     ["| {key} | Toggle Projection: Perspective / Ortho (free camera) |",
      "| {key} | Projection (Perspective / Ortho) |"]),
    ("Post FX Scope shortcut", "GLR_POST_FX_SCOPE_CYCLE",
     ["**Post FX Scope** ({key} forward / Shift+{key} backward)",
      "| {key} | Post FX Scope |"]),
]


FORBIDDEN_PATTERNS: list[tuple[str, str]] = [
    ("stale Accum effect F2 table", "| F2 | Accum effect"),
    ("stale Accum effect F2 prose", "Accum effect** (F2)"),
    ("stale grid F3 prose", "grid themes (**F3**)"),
    ("stale axes F4 prose", "axes themes (**F4**)"),
    ("stale vertex labels F5 prose", "**Vertex labels** (F5)"),
    ("stale backdrop F6 prose", "**F6** cycles the scene backdrop"),
    ("stale grid major Ctrl+O prose", "**Grid major** (Ctrl+O)"),
    ("stale grid extent F7 prose", "**Grid extent** (F7)"),
    ("stale Xform guides F8 prose", "With **Xform guides** on (F8)"),
    ("stale vertex outlines Ctrl+Shift+E", "Ctrl+Shift+E | Toggle vertex outlines"),
    ("stale focus-origin Ctrl+Shift+O row", "| Ctrl+Shift+O | Focus origin"),
    ("stale variable-panel Ctrl+Shift+P prose",
     "Toggle the panel with **Ctrl+Shift+P**"),
    ("stale Config-menu backquote prose",
     "Open the **Config** dropdown (or press **`**"),
    ("nonexistent standalone Snowfall backdrop",
     "Polar Day, Snowfall, Polar Day+Snow"),
    ("stale AA status 2x", "AA 2x"),
    ("stale AA default passes", "2 passes"),
    ("stale variable limit", "23 user variables"),
    ("stale loop nesting limit", "4 levels"),
]


def repo_root() -> Path:
    out = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True)
    return Path(out.strip())


def normalize(text: str) -> str:
    return " ".join(text.split())


def keymap_bindings(root: Path) -> dict[str, str]:
    out = subprocess.check_output(
        ["bash", "scripts/keymap.sh", "list"],
        cwd=root,
        text=True,
    )
    bindings: dict[str, str] = {}
    for line in out.splitlines():
        match = re.match(r"\s+(\S+)\s+(GLR_[A-Za-z0-9_]+)$", line)
        if match:
            bindings[match.group(2)] = match.group(1)
    return bindings


def binding_label(binding: str) -> str:
    try:
        key, mods = binding.split(",", 1)
    except ValueError:
        raise ValueError(f"bad binding format: {binding!r}") from None

    key = key.strip()
    mods = mods.strip()
    parts: list[str] = []
    if key.startswith("KEY_CTRL_"):
        parts.append("Ctrl")
    if "GLUT_ACTIVE_CTRL" in mods and "Ctrl" not in parts:
        parts.append("Ctrl")
    if "GLUT_ACTIVE_SHIFT" in mods:
        parts.append("Shift")

    base = key_base_label(key)
    return "+".join(parts + [base]) if parts else base


def key_base_label(key: str) -> str:
    if key.startswith("KEY_CTRL_"):
        name = key.removeprefix("KEY_CTRL_")
        if name == "BACKSLASH":
            return "\\"
        if name == "DASH":
            return "-"
        return name
    if key == "KEY_ESC":
        return "Esc"
    if key.startswith("GLUT_KEY_"):
        name = key.removeprefix("GLUT_KEY_")
        if name.startswith("F") and name[1:].isdigit():
            return name
        return name.capitalize()
    if len(key) >= 2 and key[0] == "'" and key[-1] == "'":
        return key[1:-1]
    return key


def main() -> int:
    root = repo_root()
    guide_path = root / "docs" / "USER_GUIDE.md"
    guide = guide_path.read_text(encoding="utf-8")
    normalized_guide = normalize(guide)
    bindings = keymap_bindings(root)
    failures: list[str] = []

    for desc, owner, patterns in CLAIMS:
        binding = bindings.get(owner)
        if binding is None:
            failures.append(f"{desc}: {owner} missing from scripts/keymap.sh list")
            continue
        key = binding_label(binding)
        for pattern in patterns:
            expected = normalize(pattern.format(key=key))
            if expected not in normalized_guide:
                failures.append(f"{desc}: expected USER_GUIDE.md to contain {expected!r}")

    for desc, pattern in FORBIDDEN_PATTERNS:
        forbidden = normalize(pattern)
        if forbidden in normalized_guide:
            failures.append(f"{desc}: found stale USER_GUIDE.md text {forbidden!r}")

    if failures:
        print("USER_GUIDE keymap drift check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("user-guide-keymap OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
