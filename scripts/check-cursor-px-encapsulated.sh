#!/bin/bash
set -euo pipefail

allowlist="${1:-scripts/allowlists/cursor-px-encapsulated.txt}"

hits="$(rg -n "\bcursor_p[xy]\b" ui_*.c 2>/dev/null || true)"

if [ -z "$hits" ]; then
  echo "cursor-px-encapsulated OK"
  exit 0
fi

filtered="$hits"
if [ -f "$allowlist" ]; then
  filtered="$(printf '%s\n' "$hits" | awk 'NR==FNR { if ($0 !~ /^#/ && $0 != "") allow[$1]=1; next } { split($0, p, ":"); file=p[1]; if (!allow[file]) print $0 }' "$allowlist" -)"
fi

if [ -n "$filtered" ]; then
  echo "ERROR: cursor_px/cursor_py references exist outside allowlist:" >&2
  printf '%s\n' "$filtered" >&2
  exit 1
fi

echo "cursor-px-encapsulated OK"
