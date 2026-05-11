#!/bin/bash
# Hard guard: forbid editor coupling in repl_*.{c,h}.
#
# Scans repl_*.c / repl_*.h at the repo root for forbidden editor
# patterns (#includes, type names, symbol prefixes) and fails with a
# non-zero exit if any LOAD-BEARING match survives. Comment-only
# references (block-comment continuation lines starting with `*`,
# line comments starting with `//`, and block-comment-open lines
# starting with `/*`) are filtered out — they describe history but do
# not link.
#
# Phase 7 of feature/source-document-port.md flipped this guard from
# the baseline-driven ratchet to a hard zero. The baseline file is
# kept around to record the comment-counting view; the live check uses
# the comment-stripped view.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

pattern='#include "editor/|#include "src/editor/|editor_buffer_|editor_state_|editor_cursor_|EditorBufferView|ReplEditorBuffer'

shopt -s nullglob
files=( repl_*.c repl_*.h )
if [ "${#files[@]}" -eq 0 ]; then
  echo "repl-no-direct-editor OK (no repl_* sources found)"
  exit 0
fi

# Filter steps:
#   1. grep -nE … gives `file:line:content` for every matching line.
#   2. strip block-comment continuations / line comments / block-comment
#      openers — those describe history, not link-time coupling.
#   3. wc -l counts what's left.
#
# grep exits 1 when nothing matches (which is the *success* case here
# once the editor coupling is gone), so wrap it in `|| true` to keep
# set -o pipefail happy.
hits="$( { grep -nE "$pattern" "${files[@]}" 2>/dev/null || true; } \
        | grep -vE ':[[:space:]]*\*|:[[:space:]]*//|:[[:space:]]*/\*' \
        || true )"

if [ -n "$hits" ]; then
  echo "ERROR: repl_*.{c,h} contains load-bearing editor references:" >&2
  echo "$hits" >&2
  echo >&2
  echo "       Route the call through a host-effect sink (repl_install_*_sink" >&2
  echo "       in repl_core.h) or include the symbol from a host adapter" >&2
  echo "       (glr_*.c / tools/repl_demo/*.c) instead." >&2
  exit 1
fi

echo "repl-no-direct-editor OK (no load-bearing editor references)"
