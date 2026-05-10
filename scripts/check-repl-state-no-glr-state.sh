#!/bin/bash
# Hard guard: REPL pipeline TUs (the REPL_DEMO_DEP_SRCS list) cannot
# include `glr_state.h` or reference `GlrState` / `glr_state_*`
# symbols. The presentation + render-config slices moved to glr_state
# in step 7a of feature/decouple-repl-from-gl-repl-alt.md, and the
# decoupling only holds if the pipeline keeps its hands off them.
#
# Editor TUs (`src/editor/*.c`), controller (`glr_*.c`), UI / scene
# renderers, and tests may include the header freely — this guard is
# scoped to the demo's REPL link set.

set -euo pipefail

# Mirrors REPL_DEMO_DEP_SRCS in the Makefile so the guard tracks what
# the demo actually links from the REPL pipeline.
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

# Match include lines + symbol references. Skip lines whose first
# non-whitespace tokens look like a comment so the doc comments
# explaining the boundary don't trip the guard.
violations=$(grep -nE '\bglr_state\.h\b|\bglr_state_[a-z_]+\b|\bGlrState\b|\bGlrPresentationState\b|\bGlrRenderState\b' "${files[@]}" 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//)' || true)

if [ -z "$violations" ]; then
    echo "repl-state-no-glr-state OK"
    exit 0
fi

echo "ERROR: REPL pipeline TUs must not reference glr_state." >&2
echo "Step 7a relocated presentation + render-config storage to glr_state.c;" >&2
echo "the pipeline reads/writes those fields through the cfg bridge or via" >&2
echo "an explicit parameter (see repl_recompute_autonormals)." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
