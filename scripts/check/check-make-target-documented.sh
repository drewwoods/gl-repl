#!/bin/bash
# Hard guard: every source-declared Make target carries a `## ` help doc.
#
# Universe: SOURCE declarations -- names literally written in the Makefile
# text, which is all an awk scraper can see. The `built_test_binary` /
# RUN_TEST_TARGETS machinery generates a few hundred further test-* /
# run-test-* aliases through $(eval); those are invisible here by design and
# are documented collectively, not one by one.
#
# This is the guard that lets `make help-details` be generated rather than
# hand-maintained: a target with no `## ` would simply vanish from help.
#
# Exempt: internal-* (implementation detail, never typed by a human), FORCE
# (a Make idiom), pattern rules, and .PHONY-style dot targets.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

MK=Makefile

# Names declared anywhere in the source text ...
declared=$(awk -F: '/^[a-zA-Z0-9_.-]+:/ {print $1}' "$MK" | sort -u)
# ... minus those carrying a `## ` on at least one of their declarations.
documented=$(awk -F':.*## ' '/^[a-zA-Z0-9_.-]+:.*## /{print $1}' "$MK" | sort -u)

missing=$(comm -23 <(printf '%s\n' "$declared") <(printf '%s\n' "$documented") \
          | grep -vE '^(internal-|\.|FORCE$)' || true)

if [ -n "$missing" ]; then
    printf 'Make targets with no `## ` help text:\n' >&2
    printf '%s\n' "$missing" | sed 's/^/  /' >&2
    printf '\nAdd `## <one-line description>` to the declaration, or name the\n' >&2
    printf 'target internal-* if it is not meant to be typed by a human.\n' >&2
    exit 1
fi

# A description may not itself contain the "## " marker: every help scraper
# splits on `:.*## `, and awk's leftmost-longest match then keeps only the text
# after the LAST marker, silently truncating the line.
selfref=$(grep -nE '^[a-zA-Z0-9_.-]+:.*## .*## ' "$MK" || true)
if [ -n "$selfref" ]; then
    printf 'Make help description contains a second "## " (help would truncate it):\n' >&2
    printf '%s\n' "$selfref" | sed 's/^/  /' >&2
    exit 1
fi

echo "OK: every documented-universe Make target carries a ## description."
