#!/bin/bash
# Hard guard: forbid editor coupling in src/repl/.
#
# Scans every .c / .h under src/repl/ for forbidden editor patterns
# (#includes, type names, symbol prefixes) and fails with a non-zero
# exit if any LOAD-BEARING match survives. Comment-only references
# (block-comment continuation lines starting with `*`, line comments
# starting with `//`, and block-comment-open lines starting with
# `/*`) are filtered out — they describe history but do not link.
#
# Phase 7 of feature/source-document-port.md flipped this guard from
# the baseline-driven ratchet to a hard zero. The baseline file is
# kept around to record the comment-counting view; the live check uses
# the comment-stripped view.
#
# Phase 0 of plans/in-review/edit-line-ownership.md repointed the
# glob from `repl_*.c repl_*.h` at the repo root (where the REPL code
# no longer lives — it moved to src/repl/ during the
# source-document-port work) to src/repl/**.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

pattern='#include "editor/|#include "src/editor/|editor_buffer_|editor_state_|editor_cursor_|EditorBufferView|EditorBuffer'

# Use find rather than bash globstar so this works the same way under
# CI shells that don't enable shopt -s globstar reliably, and also so
# this script works under bash 3 (default on macOS) where `mapfile`
# isn't built in.
files=()
while IFS= read -r f; do
  files+=( "$f" )
done < <(find src/repl -type f \( -name '*.c' -o -name '*.h' \) | sort)

if [ "${#files[@]}" -eq 0 ]; then
  echo "ERROR: src/repl/ contains no .c / .h files — guard cannot run." >&2
  exit 1
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
  echo "       in src/repl/core.h) or include the symbol from a host adapter" >&2
  echo "       (glr_*.c / tools/repl_demo/*.c) instead." >&2
  exit 1
fi

echo "repl-no-direct-editor OK (no load-bearing editor references)"
