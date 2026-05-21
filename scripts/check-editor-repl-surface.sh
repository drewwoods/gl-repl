#!/bin/bash
# Ratchet: src/editor/input.c and src/editor/commit.c must not grow
# their direct REPL-symbol surface. Counts unique `repl_*(` symbols
# called from each file and compares to a baseline.
#
# Editor → REPL coupling is the architectural debt this guard
# tracks. Every new `repl_*` call site in the editor needs a service
# callback added to EditorServices instead, so the editor's
# dependency on REPL stays narrow and the editor_demo (which links
# the editor data model without REPL) keeps working. The check
# fails loudly if either file gains a unique symbol since the
# baseline, forcing the new dependency to be considered explicitly.
#
# Ratchet down as new EditorServices callbacks land and call sites
# migrate. The plan target (plans/done/editor-demo.md) is ~5 per
# file; current baseline reflects post-edit-line-ownership reality
# (Phase 5 of plans/done/edit-line-ownership.md deleted the former
# tools/editor_demo/repl_shim.c after the storage flip removed its
# last forwarders).

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

baseline_file="${1:-scripts/baselines/editor-repl-surface.txt}"
if [ ! -f "$baseline_file" ]; then
    echo "ERROR: missing baseline file $baseline_file" >&2
    exit 1
fi

input_baseline=$(awk -F: '/^input_c/{gsub(/[ \t]/,"",$2); print $2}' "$baseline_file")
commit_baseline=$(awk -F: '/^commit_c/{gsub(/[ \t]/,"",$2); print $2}' "$baseline_file")
if [ -z "${input_baseline:-}" ] || [ -z "${commit_baseline:-}" ]; then
    echo "ERROR: baseline file $baseline_file missing 'input_c: N' or 'commit_c: N' entries" >&2
    exit 1
fi

count_unique_repl() {
    local file="$1"
    if [ ! -f "$file" ]; then
        echo 0
        return
    fi
    grep -oE '\brepl_[a-zA-Z_0-9]+\s*\(' "$file" 2>/dev/null \
        | sed 's/[[:space:]]*($//' \
        | sort -u \
        | wc -l \
        | awk '{print $1}'
}

input_count=$(count_unique_repl src/editor/input.c)
commit_count=$(count_unique_repl src/editor/commit.c)

fail=0
if [ "$input_count" -gt "$input_baseline" ]; then
    echo "ERROR: src/editor/input.c REPL surface grew past baseline (${input_baseline} -> ${input_count})." >&2
    echo "  Each new repl_* call needs either an EditorServices callback" >&2
    echo "  or a direct stub in tools/editor_demo/repl_shim.c." >&2
    fail=1
fi
if [ "$commit_count" -gt "$commit_baseline" ]; then
    echo "ERROR: src/editor/commit.c REPL surface grew past baseline (${commit_baseline} -> ${commit_count})." >&2
    echo "  Each new repl_* call needs either an EditorServices callback" >&2
    echo "  or a direct stub in tools/editor_demo/repl_shim.c." >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "editor-repl-surface OK (input.c=${input_count}/${input_baseline}, commit.c=${commit_count}/${commit_baseline})"
