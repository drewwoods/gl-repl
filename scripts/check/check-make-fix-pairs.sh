#!/bin/bash
# Hard guard: every `fix-X` target has a matching `check-X`.
#
# `fix-` is the mutating twin of a guard, not a place to park any mutating
# target: the pair is what makes "run the check, then run its fix" a rule a
# reader can predict. Two pairs exist today (doc-links, unicode); the guard
# keeps the family from becoming a junk drawer.
#
# Universe: source declarations.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

MK=Makefile

fixes=$(awk -F: '/^fix-[a-zA-Z0-9_.-]+:/ {print substr($1, 5)}' "$MK" | sort -u)
checks=$(awk -F: '/^check-[a-zA-Z0-9_.-]+:/ {print substr($1, 7)}' "$MK" | sort -u)

unpaired=$(comm -23 <(printf '%s\n' "$fixes") <(printf '%s\n' "$checks") || true)

if [ -n "$unpaired" ]; then
    printf 'fix-* target with no matching check-*:\n' >&2
    printf '%s\n' "$unpaired" | sed 's/^/  fix-/' >&2
    printf '\nA fix- target is the mutating twin of a guard. Add the check-, or\n' >&2
    printf 'name the target for what it does instead.\n' >&2
    exit 1
fi

echo "OK: every fix-* Make target pairs with a check-* ($(printf '%s\n' "$fixes" | wc -l | tr -d ' ') pairs)."
