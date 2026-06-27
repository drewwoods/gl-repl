#!/bin/bash
# Hard guard: REPL pipeline files may not link directly to the tutorial
# runner peer. Whole-document replacement paths request tutorial cleanup
# through ReplHostEffects (`repl_dispatch_tutorial_teardown`) so repl_demo
# can keep a stub-free link boundary. src/repl/host_effects.{c,h} owns the
# bridge declaration/dispatcher and is the only allowed bare tutorial_teardown
# name.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

bad_includes=$(rg -n '#include "subsystems/tutorial/tutorial\.h"' src/repl --glob '*.[ch]' || true)
bad_calls=$(rg -n '\btutorial_[A-Za-z0-9_]*\s*\(' src/repl \
    --glob '*.[ch]' \
    --glob '!host_effects.c' \
    --glob '!host_effects.h' \
    --glob '!tutorials.c' \
    --glob '!tutorials.h' || true)

if [ -n "$bad_includes" ] || [ -n "$bad_calls" ]; then
    echo "ERROR: src/repl must not include/call the tutorial runner directly." >&2
    echo "       Use repl_dispatch_tutorial_teardown() through ReplHostEffects." >&2
    if [ -n "$bad_includes" ]; then
        echo "" >&2
        echo "Direct tutorial runner includes:" >&2
        echo "$bad_includes" >&2
    fi
    if [ -n "$bad_calls" ]; then
        echo "" >&2
        echo "Direct tutorial_* calls:" >&2
        echo "$bad_calls" >&2
    fi
    exit 1
fi

echo "repl-no-direct-tutorial-runner OK"
