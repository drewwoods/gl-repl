#!/bin/sh
# build-historical.sh — build the REPL at an old SHA without OpenGL-Vibe present.
#
# Background:
#   This repo was hoisted out of OpenGL-Vibe/src/immediate-mode-repl/claude4.6-
#   opus-thinking/. Historical Makefiles set REPO_INCLUDE=$(abspath ../../..)/
#   include and expect to find OpenGL-Vibe's project-wide gl_includes.h +
#   miniaudio.h there. Those paths no longer resolve to anything useful, so
#   `make` fails at any commit before the bootstrap commit on `main`.
#
# What this script does:
#   1. Materialises the two vendored headers (gl_includes.h, miniaudio.h) into
#      a scratch dir under .compat-scratch/include/, reading them from the
#      ref named by COMPAT_REF (default: main).
#   2. Invokes `make` with REPO_INCLUDE / PROJECT_ROOT overridden to point at
#      that scratch dir, so the historical Makefile finds the headers.
#   3. Forwards any extra args to make (e.g. `sample`, `test`, `USE_GL_STUBS=1`).
#
# Usage (from the gl-repl repo root, on any SHA):
#
#     git checkout <old-sha>
#     git show main:scripts/build-historical.sh | sh -s -- sample
#     # or, if the script is checked out at HEAD as well:
#     ./scripts/build-historical.sh test USE_GL_STUBS=1
#
# Notes:
#   - The scratch dir lives under .compat-scratch/. Add it to your global
#     gitignore if it bothers you on old SHAs (the tracked .gitignore at
#     HEAD doesn't apply there).
#   - `make test` may still surface pre-existing bugs unrelated to the
#     hoist (e.g. exit() declared without <stdlib.h> in the export
#     template at very old SHAs). The compat gl_includes.h includes
#     stdlib transitively to mask the most common case.

set -eu

REPO_ROOT="$(git rev-parse --show-toplevel)"
COMPAT_REF="${COMPAT_REF:-main}"
SCRATCH="$REPO_ROOT/.compat-scratch"
INCLUDE_DIR="$SCRATCH/include"

mkdir -p "$INCLUDE_DIR"

# Pull the compat gl_includes.h from the named ref. This is the fat
# version under compat/legacy-include/, NOT the slim include/gl_includes.h
# at HEAD — historical export templates rely on transitive stdlib.
if ! git show "$COMPAT_REF:compat/legacy-include/gl_includes.h" \
        > "$INCLUDE_DIR/gl_includes.h" 2>/dev/null; then
    echo "error: $COMPAT_REF:compat/legacy-include/gl_includes.h not found" >&2
    echo "       Set COMPAT_REF to the branch/tag that has compat/ tracked." >&2
    exit 1
fi

# Pull miniaudio.h from the same ref. Try compat/ first, fall back to
# include/ (HEAD has it under include/, no separate compat copy).
if ! git show "$COMPAT_REF:compat/legacy-include/miniaudio.h" \
        > "$INCLUDE_DIR/miniaudio.h" 2>/dev/null; then
    if ! git show "$COMPAT_REF:include/miniaudio.h" \
            > "$INCLUDE_DIR/miniaudio.h" 2>/dev/null; then
        echo "error: miniaudio.h not found at $COMPAT_REF" >&2
        rm -f "$INCLUDE_DIR/miniaudio.h"
        exit 1
    fi
fi

echo "build-historical: using headers from $COMPAT_REF -> $INCLUDE_DIR"
echo "build-historical: invoking make with REPO_INCLUDE=$INCLUDE_DIR"

cd "$REPO_ROOT"
exec make \
    REPO_INCLUDE="$INCLUDE_DIR" \
    PROJECT_ROOT="$SCRATCH" \
    "$@"
