#!/bin/bash
# Hard guard: tests must never hit the default-output save path.
#
# repl_save_default_output() and the UI routes that call it write to the
# hardcoded relative path "output.c" in the process working directory —
# which is the repo root when tests run unless a test explicitly sandboxes
# the current directory first. An unsandboxed call silently overwrites the
# user's output.c every time `make test` runs.
#
# Tests that need to verify save behavior must call
# repl_export_save_output() directly with an explicit /tmp/ path.
#
# The only safe exception is glr_ctrl_router_handle_save_key in the
# Ctrl+S integration test (tests/test_repl_editor.c), which uses a mkdtemp +
# chdir redirect before invoking the save-key router.

set -euo pipefail

direct_hits=$(grep -RIn \
    --include='*.c' \
    'repl_save_default_output' \
    tests/ 2>/dev/null | grep -vE '^\./\.claude/worktrees/' || true)

menu_hits=$(grep -RInE \
    --include='*.c' \
    'repl_action_menu_item_activate\(.*REPL_MENU_FILE.*REPL_FILE_ITEM_EXPORT|repl_action_menu_item_activate\(.*REPL_MENU_SCENE.*REPL_SCENE_OFF_SAVE' \
    tests/ 2>/dev/null | grep -vE '^\./\.claude/worktrees/' || true)

save_key_hits=$(grep -RIn \
    --include='*.c' \
    'glr_ctrl_router_handle_save_key' \
    tests/ 2>/dev/null | grep -vE '^tests/test_repl_editor\.c:' | grep -vE '^\./\.claude/worktrees/' || true)

violations=$(printf '%s\n%s\n%s\n' \
    "$direct_hits" \
    "$menu_hits" \
    "$save_key_hits" | sed '/^$/d')

if [ -n "$violations" ]; then
    echo "ERROR: default-output save path reachable from a test file." >&2
    echo "This writes to ./output.c in the repo root (the process CWD when" >&2
    echo "tests run), overwriting the user's saved work unless the test" >&2
    echo "sandboxes cwd first." >&2
    echo "Use repl_export_save_output() with an explicit /tmp/ path, or" >&2
    echo "sandbox cwd around the save-path integration test." >&2
    echo "Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "no-test-default-output OK"
