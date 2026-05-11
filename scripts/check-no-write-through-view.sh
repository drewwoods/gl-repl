#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

allowlist="${1:-scripts/allowlists/write-through-view.txt}"
shift || true

if [ "$#" -gt 0 ]; then
  files=("$@")
else
  files=(ui_*.c scene_*.c)
fi

pattern='^\s*\*[A-Za-z_][A-Za-z0-9_]*->[A-Za-z0-9_]+\s*(\+\+|--|[+\-*/]?=)'

hits="$(rg -n "$pattern" "${files[@]}" 2>/dev/null || true)"
if [ -z "$hits" ]; then
  echo "write-through-view boundary OK"
  exit 0
fi

filtered="$hits"
if [ -f "$allowlist" ]; then
  filtered="$(printf '%s\n' "$hits" | awk 'NR==FNR { if ($0 !~ /^#/ && $0 != "") allow[$1]=1; next } { split($0, p, ":"); key=p[1] ":" p[2]; if (!allow[key]) print $0 }' "$allowlist" -)"
fi

if [ -n "$filtered" ]; then
  echo "ERROR: writes through view-pointer fields:" >&2
  printf '%s\n' "$filtered" >&2
  exit 1
fi

echo "write-through-view boundary OK"
