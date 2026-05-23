#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
. scripts/lib/rg-fallback.sh

allowlist="${1:-scripts/allowlists/ui-renderers-signature.txt}"

if [ ! -f "$allowlist" ]; then
  echo "ERROR: missing allowlist $allowlist" >&2
  exit 1
fi

bad=0

while IFS= read -r fn; do
  [ -z "$fn" ] && continue
  [[ "$fn" =~ ^# ]] && continue

  sig_line="$(rg -n "^\s*void\s+${fn}\s*\(" src/ui/core/*.h src/ui/core/*.c src/ui/app/*.h src/ui/app/*.c 2>/dev/null | head -n1 || true)"
  if [ -z "$sig_line" ]; then
    echo "ERROR: renderer signature not found for $fn" >&2
    bad=1
    continue
  fi

  # Strip file:line prefix.
  sig="${sig_line#*:*:}"

  one_arg='^\s*void\s+'"$fn"'\s*\(\s*const\s+Ui[A-Za-z0-9_]+(View|State|Snapshot)\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)?\s*\)\s*[;{]\s*$'
  two_arg='^\s*void\s+'"$fn"'\s*\(\s*const\s+Ui[A-Za-z0-9_]+(View|State|Snapshot)\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)?\s*,\s*Ui[A-Za-z0-9_]+Output\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)?\s*\)\s*[;{]\s*$'
  trailing_arg='^\s*void\s+'"$fn"'\s*\(\s*const\s+Ui[A-Za-z0-9_]+(View|State|Snapshot)\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)?\s*,'

  if ! printf '%s\n' "$sig" | grep -Eq "$one_arg|$two_arg|$trailing_arg"; then
    echo "ERROR: renderer signature shape invalid for $fn" >&2
    echo "  found: $sig" >&2
    bad=1
  fi
done < "$allowlist"

if [ "$bad" -ne 0 ]; then
  exit 1
fi

echo "ui-renderer-signatures OK"
