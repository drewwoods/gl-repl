#!/bin/bash
# Hard guard: the standalone repl_demo links no editor implementation.
#
# Two checks:
#   1. REPL_DEMO_DEP_SRCS in the Makefile must not contain src/editor/
#      or glr_source_document.c — the demo provides its own static
#      source-document backend.
#   2. nm repl_demo must not expose editor implementation symbols
#      (editor_buffer_*, editor_state_*, editor_cursor_*, editor_scroll_*,
#      editor_insert_mode_*, EditorState). The host-effect sinks in
#      repl_core.h are editor-NEUTRAL by name (repl_install_input_reset_sink
#      etc.) and the demo leaves them unset, so they do not appear here.
#
# Phase 7 of feature/source-document-port.md.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

binary="${1:-repl_demo}"

# --- Check 1: Makefile REPL_DEMO_DEP_SRCS ------------------------------------
bad_srcs="$(awk '
    /^REPL_DEMO_DEP_SRCS *=/ { in_block = 1 }
    in_block {
        print
        if ($0 !~ /\\$/) in_block = 0
    }
' Makefile | grep -E 'src/editor/|glr_source_document\.c' || true)"

if [ -n "$bad_srcs" ]; then
  echo "ERROR: REPL_DEMO_DEP_SRCS still references editor sources:" >&2
  echo "$bad_srcs" >&2
  echo >&2
  echo "       Replace with tools/repl_demo/source_document.c." >&2
  exit 1
fi

# --- Check 2: nm repl_demo --------------------------------------------------
if [ ! -x "$binary" ]; then
  echo "repl-demo-no-editor SKIP ($binary not built; run \`make $binary USE_GL_STUBS=1\` first)"
  exit 0
fi

# Match editor IMPLEMENTATION symbols, NOT the editor-neutral host-effect
# sink names that live in repl_core.o.
impl_pattern='_editor_buffer_|_editor_state_(input|buffer|virtual|reset|transformers|highlights|line_overrides|search|autocomplete|selection|clipboard|undo|redo)|_editor_cursor_|_editor_scroll_set|_editor_scroll_follow|_editor_insert_mode_set|_editor_insert_mode$| T _editor_insert_mode | T _editor_state_buffer | T _editor_state_input|EditorState'
hits="$(nm "$binary" 2>/dev/null | grep -E "$impl_pattern" || true)"

if [ -n "$hits" ]; then
  echo "ERROR: nm $binary exposes editor implementation symbols:" >&2
  echo "$hits" >&2
  exit 1
fi

echo "repl-demo-no-editor OK (no editor sources in REPL_DEMO_DEP_SRCS; no editor symbols in $binary)"
