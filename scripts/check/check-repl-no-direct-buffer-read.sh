#!/bin/bash
# Hard guard: REPL pipeline modules (repl_*.c) must not call the
# global `editor_buffer_line(idx)` accessor. They take an
# `EditorBufferView` parameter (or fetch one explicitly at the
# function entry) and call `editor_buffer_view_line(view, idx)`
# instead. Phase B commits 15-16 threaded the view through every
# REPL pipeline reader.
#
# Editor-side modules whose ownership renames in Phase H are
# allowlisted: they own the buffer and read it directly. Once Phase
# H ships, the allowlist contracts further.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

allowlist="${1:-scripts/allowlists/repl-no-direct-buffer-read.txt}"

if [ ! -f "$allowlist" ]; then
    echo "ERROR: missing allowlist file $allowlist" >&2
    exit 1
fi

# Match `editor_buffer_line(` as an actual function call. The string
# also appears in comments (e.g. "the global editor_buffer_line API
# remains during migration"); those don't end with `(` immediately
# after, but our regex still matches if a comment phrase happens to
# read `editor_buffer_line(...)`. Tolerate that by stripping `//` and
# `/*` lines before grep.
violations=$(grep -RIn 'editor_buffer_line(' \
    src/repl/*.c 2>/dev/null \
    | grep -v '^\./\?\.claude/worktrees/' \
    | grep -vE '^[^:]+:[0-9]+:\s*(//|\*|/\*)' \
    || true)

# Apply allowlist: any line whose file basename matches an allowed
# entry is filtered out.
filtered="$violations"
if [ -n "$violations" ]; then
    while IFS= read -r allowed; do
        case "$allowed" in
            ''|\#*) continue ;;
        esac
        filtered=$(printf '%s\n' "$filtered" \
            | grep -vE "^(\\./)?$(printf '%s' "$allowed" | sed 's|/|\\/|g'):" \
            || true)
    done < "$allowlist"
fi

if [ -n "$filtered" ]; then
    echo "ERROR: repl_*.c files call editor_buffer_line() directly." >&2
    echo "Use editor_buffer_view_line(view, idx) with an EditorBufferView" >&2
    echo "parameter, or add the file to $allowlist with a Phase H rename" >&2
    echo "note." >&2
    echo "Hits:" >&2
    printf '%s\n' "$filtered" >&2
    exit 1
fi

echo "repl-no-direct-buffer-read OK"
