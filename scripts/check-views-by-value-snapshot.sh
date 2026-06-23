#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
. scripts/lib/rg-fallback.sh

baseline_file="${1:-scripts/baselines/by-value-snapshot-pointer-returns.txt}"

pattern='^\s*const\s+[A-Za-z_][A-Za-z0-9_]*(View|State)\s*\*\s*[A-Za-z_][A-Za-z0-9_]*\s*\('

hits="$(rg -n "$pattern" src/repl src/ui src/render3d -g '*.h' 2>/dev/null || true)"
count=0
if [ -n "$hits" ]; then
  count="$(printf '%s\n' "$hits" | wc -l | tr -d ' ')"
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

if [ "$count" -gt "$baseline" ]; then
  echo "ERROR: by-value snapshot pointer-return count increased (current=$count baseline=$baseline)" >&2
  printf '%s\n' "$hits" >&2
  exit 1
fi

echo "views-by-value-snapshot OK (current=$count baseline=$baseline)"
