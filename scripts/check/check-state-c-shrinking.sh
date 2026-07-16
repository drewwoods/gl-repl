#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

baseline_file="${1:-scripts/baselines/state-c-lines.txt}"
state_file="${2:-src/repl/state.c}"

if ! command -v cloc >/dev/null 2>&1; then
  echo "WARNING: state-c-shrinking skipped: cloc is not installed" >&2
  exit 0
fi

if [ ! -f "$baseline_file" ]; then
  echo "ERROR: missing baseline file $baseline_file" >&2
  exit 1
fi

if [ ! -f "$state_file" ]; then
  echo "state-c-shrinking OK ($state_file deleted)"
  exit 0
fi

baseline="$(tr -d '[:space:]' < "$baseline_file")"
if ! [[ "$baseline" =~ ^[0-9]+$ ]]; then
  echo "ERROR: malformed baseline in $baseline_file: $baseline" >&2
  exit 1
fi

current="$(cloc --csv --quiet --by-file "$state_file" | awk -F, '$1 == "C" { print $5; exit }')"
if ! [[ "$current" =~ ^[0-9]+$ ]]; then
  echo "ERROR: could not determine C code line count for $state_file with cloc" >&2
  exit 1
fi

if [ "$current" -gt "$baseline" ]; then
  echo "ERROR: src/repl/state.c C code line count increased (current=$current baseline=$baseline)" >&2
  exit 1
fi

echo "state-c-shrinking OK (C code lines: current=$current baseline=$baseline)"
