#!/bin/bash
# Hard guard: a standalone REPL demo links no editor implementation.
#
# Parameterized by binary name ($1) and Makefile dep-srcs var name ($2) so the
# same guard covers repl_demo (default) and repl_live_demo, which both link the
# REPL pipeline + a static source-document backend but no editor TUs.
#   $ check-repl-demo-no-editor.sh                                  # repl_demo
#   $ check-repl-demo-no-editor.sh repl_live_demo REPL_LIVE_DEMO_DEP_SRCS
#
# Two checks:
#   1. <dep_var> in the Makefile must not contain src/editor/ or
#      glr_source_document.c — the demo provides its own static
#      source-document backend. (REPL_LIVE_DEMO_DEP_SRCS references
#      $(REPL_DEMO_DEP_SRCS) by expansion, so checking its own block is
#      enough: the base set is guarded independently by the repl_demo run.)
#   2. nm <binary> must not expose editor implementation symbols
#      (editor_buffer_*, editor_state_*, editor_cursor_*, editor_scroll_*,
#      editor_insert_mode_*, EditorState). The host-effect sinks in
#      src/repl/host_effects.h are editor-NEUTRAL by name and the demo leaves
#      them unset, so they do not appear here.
#
# Phase 7 of feature/source-document-port.md.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

binary="${1:-repl_demo}"
dep_var="${2:-REPL_DEMO_DEP_SRCS}"

# --- Check 1: Makefile <dep_var> --------------------------------------------
bad_srcs="$(awk -v var="$dep_var" '
    $0 ~ ("^" var " *=") { in_block = 1 }
    in_block {
        print
        if ($0 !~ /\\$/) in_block = 0
    }
' Makefile | grep -E 'src/editor/|glr_source_document\.c' || true)"

if [ -n "$bad_srcs" ]; then
  echo "ERROR: $dep_var still references editor sources:" >&2
  echo "$bad_srcs" >&2
  echo >&2
  echo "       Replace with tools/repl_demo/source_document.c." >&2
  exit 1
fi

# --- Check 2: nm <binary> ---------------------------------------------------
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

echo "repl-demo-no-editor OK (no editor sources in $dep_var; no editor symbols in $binary)"
