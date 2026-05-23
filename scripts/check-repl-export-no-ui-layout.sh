#!/bin/bash
# Hard guard: src/repl/export.c must not depend on UI layout/state.
# Step 7c of feature/decouple-repl-from-gl-repl-alt.md routes layout
# values through the opaque ReplExportLayout struct so the export
# pipeline never reaches into UI state. Code-panel cfg slug aliases
# are app/controller semantics and must stay behind the cfg bridge.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

violations=$(grep -nE '#[[:space:]]*include[[:space:]]+"ui/core/layout\.h"|\bui_layout_[a-z_]+\s*\(|\bui_state_viewport\s*\(|\bui_state_code_panel\s*\(|\bCODE_PANEL_LAYOUT_|"top_code_panel"|"code_panel"' src/repl/export.c 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//)' || true)

if [ -z "$violations" ]; then
    echo "repl-export-no-ui-layout OK"
    exit 0
fi

echo "ERROR: src/repl/export.c must not depend on UI layout/state or code-panel cfg semantics." >&2
echo "Step 7c routes layout values through ReplExportLayout opaquely; cfg slug semantics belong behind the cfg bridge." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
