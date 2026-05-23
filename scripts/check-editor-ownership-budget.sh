#!/bin/bash
# Ratchet against transitional editor / UI ownership couplings becoming
# permanent. Both signals are *expected to decrease toward zero* as
# Phase 1 closes (typedef relocation) and Phase 4 lands (UiAction
# eliminates the direct-mutation call sites the forwarders serve).
#
# Tracked couplings:
#
#   ui_forwarder_count       Lines in src/repl/state.c implementing
#                            `repl_state_(status|help|variable_panel|
#                            profile_panel|viewport|pointer|code_panel)*`
#                            forwarders into ui_state_*. Each
#                            forwarder is one such line. They go away
#                            in Phase 4 once UI input handlers emit
#                            UiAction instead of calling state mutators
#                            directly, eliminating the callers that
#                            need the legacy names.
#
#   ui_state_includes_repl_views  Whether src/ui/app/state.h still includes
#                                 src/repl/state_views.h to import the
#                                 Repl*State typedefs. Phase 5's
#                                 typedef migration breaks this
#                                 coupling.
#
# The check fails if either count exceeds its baseline. Reductions are
# always allowed; the baseline is updated by hand when a commit
# legitimately drives the count down (so the next commit cannot
# silently regress past the new low).

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

baseline_file="${1:-scripts/baselines/editor-ownership-budget.txt}"

if [ ! -f "$baseline_file" ]; then
    echo "ERROR: missing baseline file $baseline_file" >&2
    exit 1
fi

baseline_forwarders=$(awk -F: '/^ui_forwarder_count/{gsub(/[ \t]/,"",$2); print $2}' \
                          "$baseline_file")
baseline_includes=$(awk -F: '/^ui_state_includes_repl_views/{gsub(/[ \t]/,"",$2); print $2}' \
                          "$baseline_file")

forwarders=$(grep -cE '^[[:space:]]*return[[:space:]]+ui_state_|^[[:space:]]*ui_state_[a-z_]+\(' \
                  src/repl/state.c 2>/dev/null || true)
includes=$(grep -cE '^[[:space:]]*#[[:space:]]*include[[:space:]]+"repl_state_views\.h"' \
                src/ui/app/state.h 2>/dev/null || true)

# Default to 0 when grep finds nothing (-c with no match prints "0" on
# macOS / GNU grep, but be defensive).
forwarders=${forwarders:-0}
includes=${includes:-0}

ok=1
if [ "$forwarders" -gt "$baseline_forwarders" ]; then
    echo "ERROR: src/repl/state.c ui-forwarder body line count grew (${baseline_forwarders} -> ${forwarders})." >&2
    echo "These forwarders are transitional; new ones should not appear. See" >&2
    echo "scripts/baselines/editor-ownership-budget.txt for context." >&2
    ok=0
fi
if [ "$includes" -gt "$baseline_includes" ]; then
    echo "ERROR: src/ui/app/state.h #include \"src/repl/state_views.h\" count grew (${baseline_includes} -> ${includes})." >&2
    echo "src/ui/app/state.h depending on repl_state's view header inverts ownership; the" >&2
    echo "coupling exists transitionally to import Repl*State typedefs and goes" >&2
    echo "away when those typedefs migrate to a ui-owned home." >&2
    ok=0
fi

if [ "$ok" -eq 1 ]; then
    echo "editor-ownership-budget OK (ui_forwarder_lines=${forwarders}/${baseline_forwarders}, ui_state_includes=${includes}/${baseline_includes})"
fi

exit $((1 - ok))
