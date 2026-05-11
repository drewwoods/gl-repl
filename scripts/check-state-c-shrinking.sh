#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

baseline_file="${1:-scripts/baselines/state-c-lines.txt}"
state_file="${2:-repl_state.c}"

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

current="$(wc -l < "$state_file" | tr -d '[:space:]')"

if [ "$current" -gt "$baseline" ]; then
  echo "ERROR: repl_state.c line count increased (current=$current baseline=$baseline)" >&2
  exit 1
fi

echo "state-c-shrinking OK (current=$current baseline=$baseline)"
