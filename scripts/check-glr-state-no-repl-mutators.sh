#!/bin/bash
# Hard guard: glr_state.c does not reach back into REPL state. The
# four-owner contract (ReplState / EditorState / UiState / GlrState)
# stays clean only if the new owner is a pure storage module — it
# must not call repl_state_*_mut, repl_state_*_reset, or any other
# REPL-side mutator. Step 7a of feature/decouple-repl-from-gl-repl-alt.md.

set -euo pipefail

violations=$(grep -nE '\brepl_state_[a-z_]+_(mut|reset|set)\b' glr_state.c 2>/dev/null || true)

if [ -z "$violations" ]; then
    echo "glr-state-no-repl-mutators OK"
    exit 0
fi

echo "ERROR: glr_state.c must not call REPL-side mutators." >&2
echo "GlrState owns app-frame presentation/render storage; it must not reach" >&2
echo "into repl_state. The cfg bridge (glr_actions.c) and controller wiring" >&2
echo "are the legitimate paths between the two owners." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
