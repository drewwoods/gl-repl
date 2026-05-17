#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
. scripts/lib/rg-fallback.sh

rules_file="${1:-scripts/allowlists/domain-owner-encapsulation.txt}"

if [ ! -f "$rules_file" ]; then
  echo "ERROR: missing rules file $rules_file" >&2
  exit 1
fi

active=0
bad=0

while IFS= read -r raw; do
  line="${raw%%#*}"
  line="$(echo "$line" | xargs)"
  [ -z "$line" ] && continue

  domain="${line%% *}"
  allowed_csv="${line#* }"
  if [ "$allowed_csv" = "$domain" ]; then
    echo "ERROR: malformed domain encapsulation rule: $raw" >&2
    bad=1
    continue
  fi

  active=1
  IFS=',' read -r -a allowed_files <<< "$allowed_csv"

  hits="$(rg -n "repl_state_${domain}_mut\\s*\\(" *.c 2>/dev/null || true)"
  [ -z "$hits" ] && continue

  while IFS= read -r hit; do
    [ -z "$hit" ] && continue
    file="${hit%%:*}"
    ok=0
    for af in "${allowed_files[@]}"; do
      af_trim="$(echo "$af" | xargs)"
      [ -z "$af_trim" ] && continue
      if [ "$file" = "$af_trim" ]; then
        ok=1
        break
      fi
    done
    if [ "$ok" -ne 1 ]; then
      echo "ERROR: domain '$domain' mut accessor used outside owner allowlist: $hit" >&2
      bad=1
    fi
  done <<< "$hits"
done < "$rules_file"

if [ "$bad" -ne 0 ]; then
  exit 1
fi

if [ "$active" -eq 0 ]; then
  echo "domain-owner-encapsulation OK (no enforced domains yet)"
else
  echo "domain-owner-encapsulation OK"
fi
