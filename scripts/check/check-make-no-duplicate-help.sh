#!/bin/bash
# Hard guard: no target carries a `## ` help doc on more than one declaration.
#
# This is NOT a duplicate-*target* guard. A target legitimately declares once
# per arm of a platform conditional (bench-code-panel-text does), and a source
# scraper cannot tell that from copy-paste. What is never legitimate is the
# same name carrying `## ` twice: help then prints it twice, on every platform.
#
# The fix is the shape bench-code-panel-text/-stencil already use -- branch the
# build rules inside the conditional, hoist the run rule and its single `## `
# above it, and gate the recipe with `ifdef <NAME>_OK`.
#
# Universe: SOURCE declarations. A duplicate *recipe* guard would have to read
# Make's evaluated database (`make -pnR`) and would then only prove the
# invariant for the host it ran on; both CI lanes exist, but that is a
# different guard.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# The status must come from the capture, not the pipeline: `... | uniq -d`
# exits 0 whether or not it printed anything, so a bare pipeline would be a
# guard that can never fail.
dupes=$(awk -F':.*## ' '/^[a-zA-Z0-9_.-]+:.*## /{print $1}' Makefile | sort | uniq -d)

if [ -n "$dupes" ]; then
    printf 'Make target documented more than once (help prints it twice):\n' >&2
    printf '%s\n' "$dupes" | sed 's/^/  /' >&2
    printf '\nKeep one `## ` per target: branch the build rules inside the\n' >&2
    printf 'conditional and hoist the run rule (see bench-vertex-labels).\n' >&2
    exit 1
fi

echo "OK: no Make target carries a duplicate ## description."
