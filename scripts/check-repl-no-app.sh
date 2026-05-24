#!/bin/bash
# Hard ratchet: forbid new app/glr_* coupling in src/repl/.
#
# The REPL (src/repl/) is layer 1; the app shell (src/app/) is layer 6.
# Direct #includes of app/glr_*.h from REPL code invert the layering:
# the program-model layer should never know about the controller.
#
# Current baseline: one violation (src/repl/help_text.c includes
# app/glr_config.h to read GLR_CONFIG_COUNT). The cleaner fix is to
# move the config-key vocabulary into src/repl/ or pass the count
# through an init parameter; until then, this ratchet prevents the
# count from growing.
#
# Comment-only references (block-comment continuation lines, line
# comments, block-comment openers) are filtered out — they describe
# history but do not link.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

baseline_file="${1:-scripts/baselines/repl-no-app.txt}"

if [ ! -f "$baseline_file" ]; then
    echo "ERROR: missing baseline file $baseline_file" >&2
    exit 1
fi

baseline=$(awk -F: '/^app_include_count/{gsub(/[ \t]/,"",$2); print $2}' \
                "$baseline_file")

files=()
while IFS= read -r f; do
  files+=( "$f" )
done < <(find src/repl -type f \( -name '*.c' -o -name '*.h' \) | sort)

if [ "${#files[@]}" -eq 0 ]; then
  echo "ERROR: src/repl/ contains no .c / .h files — guard cannot run." >&2
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
  echo "ERROR: src/repl/ app/ include count grew (${baseline} -> ${count}):" >&2
  echo "$hits" >&2
  echo >&2
  echo "       Move the symbol into a lower layer or pass it through an" >&2
  echo "       init parameter; src/repl/ must not depend on src/app/." >&2
  exit 1
fi

echo "repl-no-app OK (app/ includes=${count}/${baseline})"
