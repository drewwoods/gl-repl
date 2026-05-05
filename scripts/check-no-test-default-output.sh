#!/bin/bash
# Hard guard: tests must never call repl_save_default_output().
#
# That function writes to the hardcoded relative path "output.c" in the
# process working directory — which is the repo root when tests run.
# A bare call would silently overwrite the user's output.c every time
# `make test` runs.
#
# Tests that need to verify save behavior must call
# repl_export_save_output() directly with an explicit /tmp/ path.
#
# The only safe exception is imrepl_ctrl_router_handle_save_key in the
# Ctrl+S integration test (test_repl_editor.c), which uses a mkdtemp +
# chdir redirect and does not call repl_save_default_output() directly.

set -euo pipefail

violations=$(grep -RIn \
    --include='*.c' \
    'repl_save_default_output' \
    tests/ 2>/dev/null | grep -vE '^\./\.claude/worktrees/' || true)

if [ -n "$violations" ]; then
    echo "ERROR: repl_save_default_output() called from a test file." >&2
    echo "This writes to ./output.c in the repo root (the process CWD when" >&2
    echo "tests run), overwriting the user's saved work." >&2
    echo "Use repl_export_save_output() with an explicit /tmp/ path instead." >&2
    echo "Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "no-test-default-output OK"
