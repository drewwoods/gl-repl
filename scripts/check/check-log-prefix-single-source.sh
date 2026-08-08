#!/bin/bash
# Hard guard: gl-repl's log prefix is spelled in exactly one place.
#
# src/app/glr_log_prefix.h owns what our own log lines look like, and the
# spelling is not merely cosmetic on the web build: packaging/web/shell.html
# routes stderr by the tag, sending trace lines to console.log and leaving
# console.error for real diagnostics. A hand-rolled "[init +%6.3fs] " in some
# other TU silently opts that line out of the routing - it renders red on
# every single page load, which is exactly the noise the shared prefix
# exists to remove. (glr_audio.c had two such copies.)
#
# So: no literal init-trace stamp, and no literal tag, outside the header
# that defines them. Build the prefix with glr_log_prefix() or paste
# GLR_LOG_TAG instead.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

owner='src/app/glr_log_prefix.h'
[ -f "$owner" ] || { echo "ERROR: $owner is missing - guard cannot run." >&2; exit 1; }

# The literal forms the header is responsible for producing.
#   "[init +   -- the native timed stamp, in any printf spelling
#   "[gl-repl] -- the native bare tag
#   "GLREPL:   -- the web tag (C string; the JS side is shell.html's own)
forbidden='"\[init \+|"\[gl-repl\]|"GLREPL:'

# Everything that links into the app, plus the web packaging TU. Comments are
# filtered out below; they describe the format rather than emitting it.
hits="$( { grep -rnE "$forbidden" \
             --include='*.c' --include='*.h' \
             src gl_repl.c gl_repl.h packaging/web 2>/dev/null || true; } \
        | grep -v "^${owner}:" \
        | grep -vE ':[[:space:]]*(\*|//|/\*)' \
        || true )"

if [ -n "$hits" ]; then
    echo "ERROR: gl-repl's log prefix is spelled outside $owner:" >&2
    echo "$hits" >&2
    echo >&2
    echo "       Use glr_log_prefix(buf, sizeof buf, &elapsed) for a stamped" >&2
    echo "       line, or paste GLR_LOG_TAG for a bare one. A hand-written" >&2
    echo "       stamp drifts from the web build's console routing and" >&2
    echo "       renders as an error on every page load." >&2
    exit 1
fi

# The header must actually still define what it claims to own, or the grep
# above is guarding nothing.
for sym in GLR_LOG_TAG GLR_LOG_PREFIX_MAX glr_log_prefix; do
    grep -q "$sym" "$owner" || {
        echo "ERROR: $owner no longer defines $sym - guard is stale." >&2
        exit 1
    }
done

echo "log-prefix-single-source OK (prefix spelled only in $owner)"
