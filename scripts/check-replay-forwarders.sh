#!/bin/bash
# Ratchet against the replay peer's legacy forwarder API becoming
# permanent. Phase F commit 33 moved ReplReplayRuntimeState bytes
# into the replay peer (replay_state.c); commit 34 routed callers
# through `replay_state_view`/`replay_state_mut` and the
# `replay_handle_*` surface. This script tracks any call site that
# still uses `repl_state_replay*` so the count can only shrink.
#
# Tracked patterns (call shape only, with `(` to skip declarations):
#
#   repl_state_replay*(    — legacy forwarders on ReplState
#
# Sites inside the owner translation units are ignored:
#   - replay_state.c is the peer (forward-targets the legacy names).
#   - repl_state.c hosts the forwarder definitions.
#
# Today's count is exclusively benchmark + test fixture sites. The
# remaining work to drive this to zero is to migrate those call
# sites alongside the eventual `replay_active_var` / `replay_pc`-style
# encapsulation that hides the raw struct.

set -euo pipefail

baseline_file="${1:-scripts/baselines/replay-forwarders.txt}"

if [ ! -f "$baseline_file" ]; then
    echo "ERROR: missing baseline file $baseline_file" >&2
    exit 1
fi

baseline=$(awk -F: '/^count/{gsub(/[ \t]/,"",$2); print $2}' "$baseline_file")
if [ -z "${baseline:-}" ]; then
    echo "ERROR: baseline file $baseline_file missing 'count: N' entry" >&2
    exit 1
fi

violations=$(grep -REn \
    --include='*.c' \
    --exclude-dir='.claude' \
    --exclude-dir='build' \
    'repl_state_replay[a-z_]*\(' \
    . 2>/dev/null \
    | grep -vE '^(\./)?(replay_state|repl_state)\.c:' || true)

if [ -z "$violations" ]; then
    count=0
else
    count=$(printf '%s\n' "$violations" | wc -l | awk '{print $1}')
fi

if [ "$count" -gt "$baseline" ]; then
    echo "ERROR: replay legacy-forwarder count grew past baseline (${baseline} -> ${count})." >&2
    echo "New callers should use replay_state_view / replay_state_mut /" >&2
    echo "replay_handle_* / replay_active() directly. See" >&2
    echo "scripts/baselines/replay-forwarders.txt for context. Hits:" >&2
    printf '%s\n' "$violations" >&2
    exit 1
fi

echo "replay-forwarders OK (count=${count}/${baseline})"
