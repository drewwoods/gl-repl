#!/bin/bash
# Ratchet guard: forbid net-new editor coupling in repl_*.{c,h}.
#
# Counts how many lines in repl_*.c / repl_*.h reference any of the
# forbidden editor patterns (includes, types, symbol prefixes) listed
# below. Compares against a baseline file; passes only if the current
# count is <= baseline. As feature/source-document-port.md phases land
# the baseline is ratcheted DOWN. Phase 7 of the plan flips the guard
# to hard-fail (baseline = 0).
#
# Note: count includes comment matches as well as live coupling. That
# is intentional for Phase 0 — historical comments that still reference
# editor_* APIs will need to be revisited as the migration completes.
set -euo pipefail

baseline_file="${1:-scripts/baselines/repl-no-direct-editor.txt}"

if [ ! -f "$baseline_file" ]; then
  echo "ERROR: missing baseline file $baseline_file" >&2
  exit 1
fi

baseline="$(tr -d '[:space:]' < "$baseline_file")"
if ! [[ "$baseline" =~ ^[0-9]+$ ]]; then
  echo "ERROR: malformed baseline in $baseline_file: $baseline" >&2
  exit 1
fi

pattern='#include "editor/|#include "src/editor/|editor_buffer_|editor_state_|editor_cursor_|EditorBufferView|ReplEditorBuffer'

# repl_*.c and repl_*.h at the repo root. Tests, controller, editor,
# scene, UI, and the demo are out of scope for this guard.
shopt -s nullglob
files=( repl_*.c repl_*.h )
if [ "${#files[@]}" -eq 0 ]; then
  echo "repl-no-direct-editor OK (no repl_* sources found)"
  exit 0
fi

# `grep -h` prints each matching line once without a filename prefix; pipe
# through wc -l for the total count. grep exits 1 when nothing matches
# (which is the *success* case for this guard once the baseline reaches 0),
# so wrap it in `|| true` to keep set -o pipefail happy.
current="$( { grep -hE "$pattern" "${files[@]}" 2>/dev/null || true; } | wc -l | tr -d '[:space:]')"

if [ "$current" -gt "$baseline" ]; then
  echo "ERROR: repl_*.{c,h} editor-coupling count increased (current=$current baseline=$baseline)" >&2
  echo "       New / changed lines matching '$pattern':" >&2
  grep -nE "$pattern" "${files[@]}" >&2 || true
  echo >&2
  echo "       After fixing legitimate regressions, ratchet the baseline DOWN to $current in $baseline_file." >&2
  exit 1
fi

echo "repl-no-direct-editor OK (count=$current <= baseline=$baseline)"
