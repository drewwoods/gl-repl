#!/bin/bash
# Guard: production code must not call editor_undo_clear() directly.
#
# Wholesale document replacement sites should use
# editor_undo_note_wholesale_replacement() which also bumps the
# generation counter.  editor_undo_clear() is retained for
# undo.c internals and test scaffolding only.
#
# Allowed:
#   - src/editor/undo.c      (implementation)
#   - tests/**                (test scaffolding)
#
# Forbidden:
#   - src/app/**              (must use note_wholesale_replacement)
#   - src/editor/* except undo.c

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

violations=$(grep -rn '\beditor_undo_clear\b' \
    --include='*.c' --include='*.h' \
    src/app/ \
    $(find src/editor/ -name '*.c' -o -name '*.h' | grep -v 'undo\.[ch]$') \
    2>/dev/null \
    || true)

if [ -n "$violations" ]; then
    echo "ERROR: production code calls editor_undo_clear() directly." >&2
    echo "Use editor_undo_note_wholesale_replacement() instead — it" >&2
    echo "clears the rings AND bumps the generation counter so stale" >&2
    echo "snapshots cannot cross scene boundaries." >&2
    echo "Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "no-raw-undo-clear OK"
