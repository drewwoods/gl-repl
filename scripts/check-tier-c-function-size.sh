#!/bin/bash
# Size ratchet for the two Tier C god-functions (audit #39 and #40).
#
# Counts lines from each function's signature to its closing ^} brace
# and fails if either exceeds its baseline.  Reductions are always
# allowed — lower the baseline by hand after extracting a sub-handler.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

baseline_file="${1:-scripts/baselines/tier-c-function-size.txt}"
if [ ! -f "$baseline_file" ]; then
    echo "ERROR: missing baseline file $baseline_file" >&2
    exit 1
fi

baseline_parse=$(awk -F: '/^parse_command/{gsub(/[ \t]/,"",$2); print $2}' "$baseline_file")
baseline_flatten=$(awk -F: '/^flatten_range/{gsub(/[ \t]/,"",$2); print $2}' "$baseline_file")

count_function_lines() {
    local file="$1" pattern="$2"
    awk "/${pattern}/{start=NR} start && /^}[[:space:]]*\$/{print NR-start+1; exit}" "$file"
}

parse_lines=$(count_function_lines src/repl/parser.c \
    '^static int parse_command\(')
flatten_lines=$(count_function_lines src/repl/flatten.c \
    '^static void flatten_range\(')

parse_lines=${parse_lines:-0}
flatten_lines=${flatten_lines:-0}

ok=1
if [ "$parse_lines" -gt "$baseline_parse" ]; then
    echo "ERROR: parse_command() in src/repl/parser.c grew past baseline (${baseline_parse} -> ${parse_lines} lines)." >&2
    echo "This function is audit Tier C #39 — it must not grow.  Extract a" >&2
    echo "sub-handler instead, or lower the baseline after decomposition." >&2
    ok=0
fi
if [ "$flatten_lines" -gt "$baseline_flatten" ]; then
    echo "ERROR: flatten_range() in src/repl/flatten.c grew past baseline (${baseline_flatten} -> ${flatten_lines} lines)." >&2
    echo "This function is audit Tier C #40 — it must not grow.  Extract a" >&2
    echo "sub-handler instead, or lower the baseline after decomposition." >&2
    ok=0
fi

if [ "$ok" -eq 1 ]; then
    echo "tier-c-function-size OK (parse_command=${parse_lines}/${baseline_parse}, flatten_range=${flatten_lines}/${baseline_flatten})"
fi

exit $((1 - ok))
