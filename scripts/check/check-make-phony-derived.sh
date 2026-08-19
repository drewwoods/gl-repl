#!/bin/bash
# Hard guard: every documented Make target is .PHONY.
#
# The Makefile derives .PHONY from the `## ` docs rather than from five
# hand-copied inventories. This guard is what keeps that derivation honest --
# and, more importantly, catches the reverse hazard: a target that is *not*
# phony is silently satisfied by a same-named file in the repo root, and this
# repo drops binaries there by design (./test_eval, ./gl-repl, ...).
#
# Universes: the documented set comes from the SOURCE text; the .PHONY set
# comes from Make's EVALUATED database (`make -pnR`), which is the only place
# the generated test-* / run-test-* aliases exist. The check runs one way --
# documented must be a subset of phony -- because the phony side legitimately
# holds hundreds of generated names the source scraper cannot see.
#
# Exempt: pattern rules (`help-%`). .PHONY has no effect on them at all, so
# they carry a FORCE prerequisite instead; demanding they be phony would be
# demanding something Make cannot honour. The source scraper cannot see them
# either (`%` is outside its name class), so no exemption list is needed.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

documented=$(awk -F: '/^[a-zA-Z0-9_.-]+:.*## / {print $1}' Makefile | sort -u)

# Unset MAKEFLAGS so the dry-run parse does not inherit a jobserver.
phony=$(MAKEFLAGS= MFLAGS= ${MAKE:-make} -pnR --no-print-directory 2>/dev/null \
        | awk '/^\.PHONY:/ {for (i = 2; i <= NF; i++) print $i}' | sort -u)

missing=$(comm -23 <(printf '%s\n' "$documented") <(printf '%s\n' "$phony") || true)

if [ -n "$missing" ]; then
    printf 'Documented Make targets missing from .PHONY:\n' >&2
    printf '%s\n' "$missing" | sed 's/^/  /' >&2
    printf '\n.PHONY is derived from the `## ` docs at the bottom of the Makefile;\n' >&2
    printf 'a generated or internal family has to be added there explicitly.\n' >&2
    exit 1
fi

echo "OK: all $(printf '%s\n' "$documented" | wc -l | tr -d ' ') documented Make targets are .PHONY."
