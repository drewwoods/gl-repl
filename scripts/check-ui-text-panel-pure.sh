#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

violations=$(grep -nE \
    'repl/|src/editor/|GLCmd|CmdType|CMD_[A-Za-z0-9_]*' \
    src/ui/text_panel.c src/ui/text_panel.h 2>/dev/null \
    || true)

if [ -n "$violations" ]; then
    echo "ERROR: src/ui/text_panel.* picked up a REPL/editor dependency or command-model symbol." >&2
    echo "src/ui/text_panel.* must stay generic; the REPL adapter owns higher-level rows and routing." >&2
    echo "Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "ui-text-panel-pure OK"