#!/bin/bash
# Hard guard: the parser core (parse_command + its set_incomplete_*
# helpers + the parser_emit_error_* helpers in repl_parser.c) does
# not call set_status directly. Diagnostics flow through
# `ReplParseContext.err_buf` and the caller decides whether and how
# to surface them.
#
# Phase H.5 commit 40 introduced the err_buf seam; Phase I commit 42a
# migrated every ctx caller to provide a buffer; Phase I commit 42b
# dropped the in-helper set_status fallback. The single intentional
# `set_status(` call site that survives is inside
# `parser_legacy_surface_to_status` — the bridge for the two no-ctx
# wrappers (repl_parser_parse_command / _with_vars) that the test
# harness still uses. Allow exactly that one call site; flag anything
# else.
#
# To migrate the bridge away too, drop both wrappers (or have the
# test harness consume err_buf directly) and lower the baseline to 0.

set -euo pipefail

baseline_file="${1:-scripts/baselines/repl-parser-set-status.txt}"
if [ ! -f "$baseline_file" ]; then
    echo "ERROR: missing baseline file $baseline_file" >&2
    exit 1
fi

baseline=$(awk -F: '/^count/{gsub(/[ \t]/,"",$2); print $2}' "$baseline_file")
if [ -z "${baseline:-}" ]; then
    echo "ERROR: baseline file $baseline_file missing 'count: N' entry" >&2
    exit 1
fi

# Match call shape only (open paren) so doc/comment references to
# the symbol don't trip the count.
hits=$(grep -nE '\bset_status\s*\(' repl_parser.c 2>/dev/null || true)

if [ -z "$hits" ]; then
    count=0
else
    count=$(printf '%s\n' "$hits" | wc -l | awk '{print $1}')
fi

if [ "$count" -gt "$baseline" ]; then
    echo "ERROR: repl_parser.c set_status() count grew past baseline (${baseline} -> ${count})." >&2
    echo "Parser diagnostics flow through ReplParseContext.err_buf — the caller" >&2
    echo "decides whether to surface them. See scripts/baselines/repl-parser-set-status.txt" >&2
    echo "for context. Hits:" >&2
    printf '%s\n' "$hits" >&2
    exit 1
fi

echo "no-set-status-in-repl-parser OK (count=${count}/${baseline})"
