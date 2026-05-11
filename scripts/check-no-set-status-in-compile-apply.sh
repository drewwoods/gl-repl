#!/bin/bash
# Hard guard: repl_compile.c and repl_apply.c never call set_status.
# Phase C established the contract that compile is a pure
# parser/validator (errors return through `err` buffers) and apply
# is pure mutation (no side-effecting status output). Phase I commit
# 42 locks that contract in so a future regression — adding a
# convenience set_status inside compile or apply — fails the build.
#
# Symmetry note: the parser core has its own guard
# (check-no-set-status-in-repl-parser, baseline 1 covering the
# legacy no-ctx wrappers' bridge). compile and apply have NO
# legitimate bridge — both are purely value-returning.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# Match call shape only; exclude lines that are clearly inside a
# C-style block comment (leading-asterisk prefix `^\s* \*`) so the
# Phase C "this file does NOT call set_status" doc comment doesn't
# trip the count.
violations=$(grep -nE '\bset_status\s*\(' repl_compile.c repl_apply.c 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+: \*' || true)

if [ -z "$violations" ]; then
    echo "no-set-status-in-compile-apply OK"
    exit 0
fi

echo "ERROR: repl_compile.c / repl_apply.c must not call set_status." >&2
echo "Phase C established compile = pure validator, apply = pure mutator." >&2
echo "Errors flow through return values; status decisions belong upstream" >&2
echo "(editor_commit / repl_editor / their callers). Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
