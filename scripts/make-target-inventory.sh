#!/usr/bin/env bash
# Reproduce the counts quoted in docs/plans/not-started/makefile-target-conventions.md.
# Two universes -- keep them apart; a guard must say which one it scrapes.
#
#   source   = names literally declared in the Makefile text (what awk/grep see)
#   evaluated = names Make knows about after $(eval), incl. generated aliases
set -u
MK=${1:-Makefile}

src_decls()  { awk -F: '/^[a-zA-Z0-9_.-]+:/ {print $1}' "$MK" | grep -vE '^\.'; }
src_unique() { src_decls | sort -u; }

printf 'source declarations (incl. repeats under ifeq): %s\n' "$(src_decls | wc -l | tr -d ' ')"
printf 'source unique names:                            %s\n' "$(src_unique | wc -l | tr -d ' ')"
printf 'check-* unique (excludes the "check" aggregator): %s\n' \
  "$(awk -F: '/^check-[a-z0-9-]+:/{print $1}' "$MK" | sort -u | wc -l | tr -d ' ')"
printf 'demo targets (HEADLESS_DEMO_TARGETS):           %s\n' \
  "$(make -pnR 2>/dev/null | awk '/^HEADLESS_DEMO_TARGETS/{gsub(/.*= */,"");print;exit}' | wc -w | tr -d ' ')"

# Names declared more than once in source: legitimate under mutually exclusive
# ifeq arms. Only a repeated `## ` doc is a defect.
printf '\nnames declared >1x in source (ifeq arms -- legitimate):\n'
src_decls | sort | uniq -d | sed 's/^/  /'
printf '\nnames carrying a `## ` doc >1x (help prints twice -- DEFECT):\n'
awk -F':.*## ' '/^[a-zA-Z0-9_.-]+:.*## /{print $1}' "$MK" | sort | uniq -d | sed 's/^/  /'

# Genuinely undocumented: no ## on ANY of its declarations, and not internal.
printf '\nundocumented (no ## on any declaration, excl. internal-*/FORCE):\n'
comm -23 <(src_unique) \
         <(awk -F':.*## ' '/^[a-zA-Z0-9_.-]+:.*## /{print $1}' "$MK" | sort -u) \
  | grep -vE '^(internal-|FORCE$)' | sed 's/^/  /'

printf '\nevaluated universe (needs a real make parse):\n'
printf '  test/run-test generated names: %s\n' \
  "$(make -pnR 2>/dev/null | grep -oE '^(run-)?test[-_][a-z0-9_-]+:' | sort -u | wc -l | tr -d ' ')"
