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
cd "$(git rev-parse --show-toplevel)"

# REPL pipeline TUs that the demo links — a subset of
# REPL_DEMO_DEP_SRCS. The list intentionally excludes the app-side
# `glr_*.c` modules (glr_camera, glr_camera_export) that also live in
# the demo link set: those are the legitimate consumers of glr_state /
# GlrState symbols. When REPL_DEMO_DEP_SRCS changes, this list must be
# updated alongside if a new repl_*.c TU is added; new glr_*.c TUs are
# exempt by construction.
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
    repl_autocomplete.c
    src/subsystems/replay/replay_annotations.c
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
