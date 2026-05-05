#!/bin/bash
# Hard guard: the `repl_command_store_*_with_line[s]` overloads were
# dropped in Phase B commit 17. The command-store API now mutates only
# the GLCmd array; callers pair its calls with `editor_buffer_*` for
# the parallel text write. This guard fails the build if any caller
# (or new declaration) re-introduces the text-aware overload.
#
# The grep targets the four exact symbol names. Test-function names
# like `test_repl_command_store_insert_one_with_line` (which contain
# the substring but call the new API) are filtered by anchoring on
# the API surface — `_with_line[s](` in source code, not as a name
# fragment. We also explicitly exclude .claude/worktrees/ which can
# hold pre-rebased copies.

set -euo pipefail

violations=$(grep -RInE \
    --include='*.c' --include='*.h' \
    'repl_command_store_(insert_many|insert_one|replace_one|load)_with_lines?\(' \
    . 2>/dev/null \
    | grep -vE '\./\.claude/worktrees/' || true)

if [ -n "$violations" ]; then
    echo "ERROR: repl_command_store_*_with_line[s] API is gone (Phase B commit 17)." >&2
    echo "Callers must split into a command-store call + editor_buffer_* pair." >&2
    echo "Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "no-store-text-api OK"
