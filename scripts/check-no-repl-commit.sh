#!/bin/bash
# Hard guard: repl_commit.c was deleted in Phase H.5 commit 41 and
# the dispatcher chain (try_commit_*, repl_commit_func_decl_resume_*,
# repl_commit_resolve_insert_exit_target, repl_commit_reset_transients)
# moved into editor_commit.c. The corrected M/V/C contract has the
# editor attempt commits and the REPL act as a pure parser/validator;
# commit dispatch is editor-side orchestration, not REPL grammar.
#
# This guard fails the build if a `repl_commit.{c,h}` file reappears
# anywhere in the tree.

set -euo pipefail

violations=$(find . \
    -path './.claude' -prune -o \
    -path './build' -prune -o \
    \( -name 'repl_commit.c' -o -name 'repl_commit.h' \) -print \
    2>/dev/null || true)

if [ -n "$violations" ]; then
    echo "ERROR: repl_commit.{c,h} reappeared (Phase H.5 commit 41 deleted it)." >&2
    echo "Commit dispatch belongs in editor_commit.c — the REPL is a parser /" >&2
    echo "validator, not the owner of commit attempts." >&2
    echo "Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "no-repl-commit OK"
