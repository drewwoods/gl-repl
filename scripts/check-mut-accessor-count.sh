#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
. scripts/lib/rg-fallback.sh

baseline_file="${1:-scripts/baselines/mut-count.txt}"
shift || true

if [ "$#" -eq 0 ]; then
  echo "ERROR: check-mut-accessor-count requires source file arguments" >&2
  exit 1
fi

if [ ! -f "$baseline_file" ]; then
  echo "ERROR: missing baseline file $baseline_file" >&2
  exit 1
fi

baseline="$(tr -d '[:space:]' < "$baseline_file")"
if ! [[ "$baseline" =~ ^[0-9]+$ ]]; then
  echo "ERROR: malformed baseline in $baseline_file: $baseline" >&2
  exit 1
fi

count="$(rg -n "repl_state_[A-Za-z0-9_]*_mut\\s*\\(" "$@" 2>/dev/null | wc -l | tr -d ' ')"

if [ "$count" -gt "$baseline" ]; then
  echo "ERROR: repl_state_*_mut call count increased (current=$count baseline=$baseline)" >&2
  exit 1
fi

echo "mut-accessor-count OK (current=$count baseline=$baseline)"
