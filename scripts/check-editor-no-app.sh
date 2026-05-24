#!/bin/bash
# Hard ratchet: forbid new app/glr_* coupling in src/editor/.
#
# The editor (src/editor/) is layer 2 in MODULES.md; non-editor concerns
# are supposed to be filtered by the controller before the editor sees
# them. Direct #includes of app/glr_*.h from the editor invert the
# layering: changes to app/* propagate down into the text-document
# model.
#
# Audit finding #8 in plans/in-review/src-editor-code-smell-audit.md
# documents the current four violations in src/editor/input.c; the
# week-pass fix hoists those call sites into src/app/glr_ctrl.c.
# Until that lands, this ratchet prevents the count from growing.
#
# Comment-only references (block-comment continuation lines, line
# comments, block-comment openers) are filtered out — they describe
# history but do not link.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

baseline_file="${1:-scripts/baselines/editor-no-app.txt}"

if [ ! -f "$baseline_file" ]; then
    echo "ERROR: missing baseline file $baseline_file" >&2
    exit 1
fi

baseline=$(awk -F: '/^app_include_count/{gsub(/[ \t]/,"",$2); print $2}' \
                "$baseline_file")

files=()
while IFS= read -r f; do
  files+=( "$f" )
done < <(find src/editor -type f \( -name '*.c' -o -name '*.h' \) | sort)

if [ "${#files[@]}" -eq 0 ]; then
  echo "ERROR: src/editor/ contains no .c / .h files — guard cannot run." >&2
  exit 1
fi

hits="$( { grep -nE '#include[[:space:]]+"app/' "${files[@]}" 2>/dev/null || true; } \
        | grep -vE ':[[:space:]]*\*|:[[:space:]]*//|:[[:space:]]*/\*' \
        || true )"

count=0
if [ -n "$hits" ]; then
  count=$(echo "$hits" | wc -l | tr -d ' ')
fi

if [ "$count" -gt "$baseline" ]; then
  echo "ERROR: src/editor/ app/ include count grew (${baseline} -> ${count}):" >&2
  echo "$hits" >&2
  echo >&2
  echo "       Hoist the call site into src/app/glr_ctrl.c or expose the" >&2
  echo "       symbol through a thin editor-text helper (see audit finding #8)." >&2
  exit 1
fi

echo "editor-no-app OK (app/ includes=${count}/${baseline})"
