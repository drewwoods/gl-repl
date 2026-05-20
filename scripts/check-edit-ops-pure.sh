#!/bin/bash
# Hard guard: src/editor/edit_ops.{c,h} must stay REPL-free.
#
# edit_ops is the generic text-editing primitive library called by
# both src/editor/input.c (REPL editor dispatcher) and
# tools/editor_demo/input.c (generic editor dispatcher). If REPL
# coupling leaks in, the demo's input.c can no longer reuse the
# primitives without dragging in REPL, which defeats the whole
# Phase 8 forcing function.
#
# Fails on:
#   - #include "repl/..." or #include <repl/...> or any path
#     starting with src/repl/.
#   - References to repl_* identifiers (function calls or types).
#
# Allowed: anything under src/editor/ (state, limits) plus standard
# headers. The generic UI helpers (text_panel etc.) are not
# referenced today but would be fine if a future primitive needs
# them.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

violations=$(grep -nE \
    '^[[:space:]]*#include[[:space:]]+["<](src/)?repl/|\brepl_[A-Za-z0-9_]+' \
    src/editor/edit_ops.c src/editor/edit_ops.h 2>/dev/null \
    || true)

if [ -n "$violations" ]; then
    echo "ERROR: src/editor/edit_ops.* picked up a REPL dependency." >&2
    echo "edit_ops must stay generic — primitives are called by both the" >&2
    echo "REPL editor (src/editor/input.c) and the generic demo dispatcher" >&2
    echo "(tools/editor_demo/input.c). Move REPL-specific logic to a" >&2
    echo "REPL-flavored controller file (input.c, commit.c) instead." >&2
    echo "Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "edit-ops-pure OK"
