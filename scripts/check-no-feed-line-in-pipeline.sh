#!/bin/bash
# Hard guard: REPL pipeline TUs must not call `feed_line()`. Step 5b
# of feature/decouple-repl-from-gl-repl-alt.md introduced the lean
# non-editor source-load API (`repl_load_apply_line`), and step 7e
# migrated the last pipeline caller (repl_example_loader.c) onto it.
# `feed_line` is editor input dispatch — calling it from the pipeline
# pulls the editor commit chain (try_commit_*, cursor mutations,
# insert-mode toggles, input buffer writes) into the demo link set.
# Any new pipeline TU that needs to apply a source line should call
# `repl_load_apply_line` from repl_load.h.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# REPL pipeline TUs that the demo links — same scope as
# check-repl-state-no-glr-state.sh. App-side glr_*.c modules,
# editor TUs, controllers, and tests may still reference feed_line
# (it lives in src/editor/input.c).
files=(
    repl_core.c
    repl_state.c
    repl_parser.c
    repl_command_spec.c
    repl_command_store.c
    repl_compile.c
    repl_load.c
    repl_apply.c
    repl_flatten.c
    repl_executor.c
    repl_eval.c
    repl_source_scope.c
    repl_autonormal.c
    repl_scenes.c
    repl_example_loader.c
    repl_examples.c
    repl_export.c
    repl_autocomplete.c
    repl_replay_annotations.c
)

# Match `feed_line(` as a function call. Skip pure comment lines so
# doc text mentioning the historical `feed_line` path does not trip
# the guard.
violations=$(grep -nE '\bfeed_line\s*\(' "${files[@]}" 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//)' || true)

if [ -z "$violations" ]; then
    echo "no-feed-line-in-pipeline OK"
    exit 0
fi

echo "ERROR: REPL pipeline TUs must not call feed_line()." >&2
echo "Step 5b/7e routed example/import line application through" >&2
echo "repl_load_apply_line (repl_load.h). New pipeline callers should" >&2
echo "do the same." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
