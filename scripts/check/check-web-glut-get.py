#!/usr/bin/env python3
"""Hard guard: every web-reachable glutGet() asks for an enum the browser can answer.

The web build does not use freeglut's windowing layer - Emscripten's JS GLUT
(`libglut.js`) supplies it, and freeglut's own entry points are renamed away by
include/GL/emscripten_hide_glut.h (see packaging/web/README.md). That JS
implementation handles a fixed set of glutGet types and its `default:` branch is
a hard `abort()`:

    default:
      abort("glutGet(" + type + ") not implemented yet");

So an enum outside the allowlist below does not degrade or return 0 - it kills
the whole app, from wherever it is called. A probe placed between glutInit() and
glutCreateWindow() takes the page down before a window ever opens, with nothing
on screen but "Exception thrown" and a wasm stack trace. That is what
glutGet(GLUT_DISPLAY_MODE_POSSIBLE) - a native Mesa accum-visual workaround -
did to every browser until it was guarded.

Nothing else catches this. `make test-web` links the GL stubs and never calls
main(), so the glutInit -> glutCreateWindow sequence is unexercised there, and
the native builds answer every enum happily.

A call is accepted when it is either (a) an allowlisted enum, or (b) lexically
inside a region the web build does not compile - `#if !defined(__EMSCRIPTEN__)`
/ `#ifndef __EMSCRIPTEN__`, or the `#else` of an `#ifdef __EMSCRIPTEN__`. The
scan tracks that nesting itself rather than trusting a marker comment, so the
guard and the compiler agree on what "native-only" means.

Scope note: this checks the glutGet *enum* only. Its siblings (glutGetWindow,
glutGetColor, ...) are a different failure - the JS GLUT simply does not export
them, so a web build fails at link time, which is loud and self-diagnosing.
"""

import re
import subprocess
import sys
from pathlib import Path

# The exact `case` labels in emscripten/src/lib/libglut.js glutGet().
# Anything not here reaches the abort(). Keep in sync when bumping emsdk.
ALLOWED = {
    "GLUT_WINDOW_X",             # 100
    "GLUT_WINDOW_Y",             # 101
    "GLUT_WINDOW_WIDTH",         # 102
    "GLUT_WINDOW_HEIGHT",        # 103
    "GLUT_WINDOW_STENCIL_SIZE",  # 0x0069
    "GLUT_WINDOW_DEPTH_SIZE",    # 0x006A
    "GLUT_WINDOW_ALPHA_SIZE",    # 0x006E
    "GLUT_WINDOW_NUM_SAMPLES",   # 0x0078
    "GLUT_SCREEN_WIDTH",         # 200
    "GLUT_SCREEN_HEIGHT",        # 201
    "GLUT_INIT_WINDOW_X",        # 500
    "GLUT_INIT_WINDOW_Y",        # 501
    "GLUT_INIT_WINDOW_WIDTH",    # 502
    "GLUT_INIT_WINDOW_HEIGHT",   # 503
    "GLUT_ELAPSED_TIME",         # 700
}

# The TUs the web link actually pulls in: $(SRCS) plus the link-time bootstrap.
# tools/ demos and tests/ are native-only and are deliberately not scanned.
SCAN_GLOBS = (
    "src/**/*.c", "src/**/*.h",
    "include/*.h",
    "gl_repl.c", "gl_repl.h",
    "packaging/web/*.c",
)

CALL_RE = re.compile(r"\bglutGet\s*\(\s*([^,()]*?)\s*\)")
IF_NOT_EMS_RE = re.compile(
    r"^\s*#\s*(?:ifndef\s+__EMSCRIPTEN__\b"
    r"|if\s+!\s*defined\s*\(?\s*__EMSCRIPTEN__\s*\)?\s*$)")
IF_EMS_RE = re.compile(
    r"^\s*#\s*(?:ifdef\s+__EMSCRIPTEN__\b"
    r"|if\s+defined\s*\(?\s*__EMSCRIPTEN__\s*\)?\s*$)")
IF_ANY_RE = re.compile(r"^\s*#\s*if")
ELSE_RE = re.compile(r"^\s*#\s*else\b")
ELIF_RE = re.compile(r"^\s*#\s*elif\b")
ENDIF_RE = re.compile(r"^\s*#\s*endif\b")


def strip_comments(text):
    """Blank out comments and string literals, preserving line structure."""
    out, i, n = [], 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
            i = end
        elif two == "//":
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif text[i] in "\"'":
            quote, j = text[i], i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def scan(text):
    """Yield (lineno, enum) for web-reachable glutGet calls.

    Stack entries are (kind, excluded): `kind` records what the conditional
    opened on, so `#else` can flip an __EMSCRIPTEN__ test and leave an
    unrelated one alone; `excluded` is whether the branch now in effect is
    one the web build drops.
    """
    stack = []
    for lineno, line in enumerate(strip_comments(text).splitlines(), 1):
        if IF_NOT_EMS_RE.match(line):
            stack.append(["not_ems", True])
            continue
        if IF_EMS_RE.match(line):
            stack.append(["ems", False])
            continue
        if IF_ANY_RE.match(line):
            stack.append(["other", False])
            continue
        if ELSE_RE.match(line):
            if stack:
                kind = stack[-1][0]
                # The #else of `#ifdef __EMSCRIPTEN__` is the native arm, and
                # the #else of `#if !defined(__EMSCRIPTEN__)` is the web arm.
                stack[-1][1] = (kind == "ems")
                stack[-1][0] = {"ems": "not_ems",
                                "not_ems": "ems"}.get(kind, "other")
            continue
        if ELIF_RE.match(line):
            if stack:
                # Any other condition may well hold under the web build.
                stack[-1] = ["other", False]
            continue
        if ENDIF_RE.match(line):
            if stack:
                stack.pop()
            continue
        if any(excluded for _, excluded in stack):
            continue
        for m in CALL_RE.finditer(line):
            yield lineno, m.group(1).strip()


def main():
    root = Path(subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True).strip())
    files = sorted({p for g in SCAN_GLOBS for p in root.glob(g) if p.is_file()})
    if not files:
        print("ERROR: no sources matched - guard cannot run.", file=sys.stderr)
        return 1

    bad = []
    seen_any_call = False
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        if "glutGet" not in text:
            continue
        for lineno, enum in scan(text):
            seen_any_call = True
            if enum not in ALLOWED:
                bad.append((path.relative_to(root), lineno, enum))

    if not seen_any_call:
        print("ERROR: scanned sources contain no glutGet() call at all - the "
              "guard is no longer watching anything.", file=sys.stderr)
        return 1

    if bad:
        print("ERROR: glutGet() enums the web build cannot answer:",
              file=sys.stderr)
        for path, lineno, enum in bad:
            print(f"  {path}:{lineno}: glutGet({enum})", file=sys.stderr)
        print("", file=sys.stderr)
        print("       Emscripten's JS GLUT abort()s the whole app on any enum",
              file=sys.stderr)
        print("       outside its switch. Either use an answerable enum, or -",
              file=sys.stderr)
        print("       if the query is a native-only concern - wrap the call in",
              file=sys.stderr)
        print("       #if !defined(__EMSCRIPTEN__), as gl_repl.c does for the",
              file=sys.stderr)
        print("       GLUT_DISPLAY_MODE_POSSIBLE accum-visual probe.",
              file=sys.stderr)
        return 1

    print("web-glut-get OK (every web-reachable glutGet enum is implemented)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
