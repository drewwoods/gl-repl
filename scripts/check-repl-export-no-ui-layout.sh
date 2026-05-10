#!/bin/bash
# Hard guard: repl_export.c must not call ui_layout_* / ui_state_*.
# Step 7c of feature/decouple-repl-from-gl-repl-alt.md routes layout
# values through the opaque ReplExportLayout struct so the export
# pipeline never reaches into UI state. The CODE_PANEL_LAYOUT_* enum
# constants from ui/layout.h remain accessible — they're scalar
# values, not function calls.

set -euo pipefail

violations=$(grep -nE '\bui_layout_[a-z_]+\s*\(|\bui_state_viewport\s*\(|\bui_state_code_panel\s*\(' repl_export.c 2>/dev/null \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//)' || true)

if [ -z "$violations" ]; then
    echo "repl-export-no-ui-layout OK"
    exit 0
fi

echo "ERROR: repl_export.c must not call ui_layout_* / ui_state_*." >&2
echo "Step 7c routes layout values through ReplExportLayout opaquely." >&2
echo "Hits:" >&2
printf '%s\n' "$violations" >&2
exit 1
