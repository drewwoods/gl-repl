#!/bin/bash
# Hard guard: REPL pipeline TUs must not call `editor_feed_line()`. Step 5b
# of feature/decouple-repl-from-gl-repl-alt.md introduced the lean
# non-editor source-load API (`repl_load_apply_line`), and step 7e
# migrated the last pipeline caller (src/repl/example_loader.c) onto it.
# `editor_feed_line` is editor input dispatch — calling it from the pipeline
# pulls the editor commit chain (try_commit_*, cursor mutations,
# insert-mode toggles, input buffer writes) into the demo link set.
# Any new pipeline TU that needs to apply a source line should call
# `repl_load_apply_line` from src/repl/load.h.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# REPL pipeline TUs that the demo links — same scope as
# check-repl-state-no-glr-state.sh. App-side glr_*.c modules,
# editor TUs, controllers, and tests may still reference editor_feed_line
# (it lives in src/editor/input.c).
files=(
    src/repl/core.c
    src/repl/state.c
    src/repl/parser.c
    src/repl/command_spec.c
    src/repl/command_store.c
    src/repl/compile.c
    src/repl/load.c
    src/repl/apply.c
    src/repl/flatten.c
    src/repl/executor.c
    src/repl/eval.c
    src/repl/source_scope.c
    src/repl/autonormal.c
    src/repl/scenes.c
    src/repl/example_loader.c
    src/repl/examples.c
    src/repl/export.c
    src/repl/import.c
    repl_autocomplete.c
    src/repl/replay_annotations.c
)

# Match `editor_feed_line(` as a function call. Skip pure comment lines so
# doc text mentioning the historical `editor_feed_line` path does not trip
# the guard.
violations=$(grep -nE '\beditor_feed_line\s*\(' "${files[@]}" 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//)' || true)

if [ -z "$violations" ]; then
    echo "no-feed-line-in-pipeline OK"
    exit 0
fi

echo "ERROR: REPL pipeline TUs must not call editor_feed_line()." >&2
echo "Step 5b/7e routed example/import line application through" >&2
echo "repl_load_apply_line (src/repl/load.h). New pipeline callers should" >&2
echo "do the same." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
