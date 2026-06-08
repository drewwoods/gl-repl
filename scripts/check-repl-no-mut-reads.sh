#!/bin/bash
# Hard ratchet: cap `_mut()` call sites in src/repl/ outside the
# legitimate owner files.
#
# Audit #7/#14 sweep dropped `_mut()` reads from several files.
# The 2026-06-08 final sweep removed the remaining 11 `_mut()` calls
# (all writers) by introducing explicit write surfaces (state_owners.h)
# and setters.
#
# NEW RULE: ZERO `_mut()` calls allowed in src/repl/ outside the
# legitimate owner files.
#
# Owner files (allowed to use `_mut()` freely): the typed-facade
# implementations themselves — state.c, apply.c, command_store.c,
# eval.c — own the underlying storage. Headers that DECLARE the
# `_mut()` API are also exempt.
#
# The ratchet counts `_mut(` call sites (not declarations) in
# src/repl/*.c outside the owner list. New additions must either
# - sit in an owner file,
# - genuinely write (then the ratchet count grows by 1 and the
#   baseline gets bumped intentionally), or
# - be rewritten as reads through the const accessor.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

baseline_file="${1:-scripts/baselines/repl-no-mut-reads.txt}"

if [ ! -f "$baseline_file" ]; then
    echo "ERROR: missing baseline file $baseline_file" >&2
    exit 1
fi

baseline=$(awk -F: '/^mut_call_count/{gsub(/[ \t]/,"",$2); print $2}' \
                "$baseline_file")

# Find all .c files in src/repl/ except owner implementations.
files=()
while IFS= read -r f; do
    case "$f" in
        src/repl/state.c|src/repl/apply.c|src/repl/command_store.c|src/repl/eval.c)
            ;;
        *)
            files+=( "$f" )
            ;;
    esac
done < <(find src/repl -maxdepth 1 -type f -name '*.c' | sort)

if [ "${#files[@]}" -eq 0 ]; then
    echo "ERROR: src/repl/ contains no non-owner .c files — guard cannot run." >&2
    exit 1
fi

# Match _mut(  (function call), not _mut without parens (decl/type).
# Strip comment-only matches (// ..., * ..., /* ...).
hits="$( { grep -nE '\b[a-zA-Z_][a-zA-Z0-9_]*_mut\(' "${files[@]}" 2>/dev/null || true; } \
        | grep -vE ':[[:space:]]*\*|:[[:space:]]*//|:[[:space:]]*/\*' \
        || true )"

count=0
if [ -n "$hits" ]; then
    count=$(echo "$hits" | wc -l | tr -d ' ')
fi

if [ "$count" -gt "$baseline" ]; then
    echo "ERROR: src/repl/ _mut() call count grew (${baseline} -> ${count}):" >&2
    echo "$hits" >&2
    echo >&2
    echo "       Either route reads through the const accessor (e.g." >&2
    echo "       repl_state_document_cmds(), repl_eval_predef_view()) or" >&2
    echo "       move the write into an owner file (state.c / apply.c /" >&2
    echo "       command_store.c / eval.c). See audit #7/#14 in" >&2
    echo "       plans/active/src-repl-code-smell-audit-2.md." >&2
    exit 1
fi

echo "repl-mut-reads OK (_mut() calls=${count}/${baseline})"
