#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
. scripts/lib/rg-fallback.sh

allowlist="${1:-scripts/allowlists/facade-includes-in-views.txt}"

hits="$(rg -n "#\s*include\s+\"repl/state(\.h|_views\.h|_owners\.h)\"" src/ui src/render3d -g '*.c' -g '*.h' 2>/dev/null || true)"

if [ -z "$hits" ]; then
  echo "no-facade-include-in-views OK"
  exit 0
fi

filtered="$hits"
if [ -f "$allowlist" ]; then
  filtered="$(printf '%s\n' "$hits" | awk 'NR==FNR { if ($0 !~ /^#/ && $0 != "") allow[$1]=1; next } { split($0, p, ":"); file=p[1]; if (!allow[file]) print $0 }' "$allowlist" -)"
fi

if [ -n "$filtered" ]; then
  echo "ERROR: facade includes remain in view files outside allowlist:" >&2
  printf '%s\n' "$filtered" >&2
  exit 1
fi

echo "no-facade-include-in-views OK"
