#!/bin/bash
# Hard guard: tools/repl_demo/stubs.c must shrink monotonically.
# Counts function-definition lines (lines with `^[a-zA-Z_].*(.*) {`)
# and rejects any commit that increases the count past a baseline.
#
# After step 7e cleared the last stub (`feed_line`), the baseline is
# 0: stubs.c contains no function bodies, only the documentation
# header. Any new pipeline reach-out that requires a stub here is a
# regression on the decoupling endpoint and must be addressed at the
# pipeline TU instead (introduce a bridge / sink / opaque parameter).

set -euo pipefail

file="tools/repl_demo/stubs.c"
baseline=0

if [ ! -f "$file" ]; then
    echo "repl-demo-stubs-shrinking OK (no stubs.c)"
    exit 0
fi

# Count function-definition lines: identifier(s)+, optional return
# type, identifier, parens, opening brace. Heuristic but stable for
# the simple stub shapes the demo has historically used.
count=$(grep -cE '^[a-zA-Z_][a-zA-Z0-9_ \*]*[a-zA-Z_][a-zA-Z0-9_]*\([^)]*\)[[:space:]]*\{' "$file" || true)

if [ "$count" -le "$baseline" ]; then
    echo "repl-demo-stubs-shrinking OK (count=$count <= baseline=$baseline)"
    exit 0
fi

echo "ERROR: tools/repl_demo/stubs.c grew past the ratchet baseline." >&2
echo "  current=$count   baseline=$baseline" >&2
echo "" >&2
echo "Adding a stub here means the pipeline acquired a new editor / UI /" >&2
echo "controller dependency. The decoupling endpoint requires the new" >&2
echo "edge to be addressed at the pipeline TU — install a bridge / sink" >&2
echo "/ opaque parameter — instead of stubbing the symbol in the demo." >&2
echo "" >&2
echo "If the increase is genuinely intentional (e.g. accepting a temporary" >&2
echo "regression while migrating), bump the baseline at the top of this" >&2
echo "script alongside the same commit." >&2
exit 1
