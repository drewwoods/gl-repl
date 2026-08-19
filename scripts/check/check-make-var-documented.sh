#!/bin/bash
# Hard guard: the public Make variable interface and its `#? ` annotations
# agree, in both directions.
#
#   PUBLIC_MAKE_VARS   the declared interface (Makefile)
#   `#? NAME: text`    the documentation, written at the declaration site
#
# `#? ` is opt-in on purpose. Most of the Makefile's `?=` declarations are
# internals (per-test *_RUN definitions, *_CFLAGS, *_DIR); requiring an
# annotation on all of them would rebuild, in `make help-vars`, exactly the
# 70-line dump the generated help exists to delete.
#
# The annotation is its own comment line rather than a trailing comment on the
# assignment: GNU make (3.81 on macOS included) keeps the whitespace *before*
# a comment as part of the value, so `SAN ?= address  #? ...` would set SAN to
# "address  " and trip the `ifneq ($(SAN),address)` error arm. Verified.
#
# PUBLIC_MAKE_VARS arrives in the environment from the Make recipe, so the
# guard reads the evaluated list rather than re-parsing a backslash
# continuation out of the source.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

MK=Makefile
: "${PUBLIC_MAKE_VARS:?run this through 'make check-make-var-documented'}"

annotated=$(grep -oE '^#\? [A-Za-z_][A-Za-z0-9_]*:' "$MK" | sed 's/^#? //; s/:$//' | sort)
declared=$(printf '%s\n' $PUBLIC_MAKE_VARS | sort)

fail=0

dupes=$(printf '%s\n' "$annotated" | uniq -d)
if [ -n "$dupes" ]; then
    printf 'Variable annotated more than once (help-vars would list it twice):\n' >&2
    printf '%s\n' "$dupes" | sed 's/^/  /' >&2
    fail=1
fi

undocumented=$(comm -23 <(printf '%s\n' "$declared") <(printf '%s\n' "$annotated" | uniq) || true)
if [ -n "$undocumented" ]; then
    printf 'In PUBLIC_MAKE_VARS but carrying no "#? NAME: ..." annotation:\n' >&2
    printf '%s\n' "$undocumented" | sed 's/^/  /' >&2
    fail=1
fi

unlisted=$(comm -13 <(printf '%s\n' "$declared") <(printf '%s\n' "$annotated" | uniq) || true)
if [ -n "$unlisted" ]; then
    printf 'Annotated with "#? " but missing from PUBLIC_MAKE_VARS:\n' >&2
    printf '%s\n' "$unlisted" | sed 's/^/  /' >&2
    fail=1
fi

# A malformed annotation renders as a blank name or a blank description.
malformed=$(grep -nE '^#\? ' "$MK" | grep -vE '^[0-9]+:#\? [A-Za-z_][A-Za-z0-9_]*: .' || true)
if [ -n "$malformed" ]; then
    printf 'Malformed annotation (want `#? NAME: description`):\n' >&2
    printf '%s\n' "$malformed" | sed 's/^/  /' >&2
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "OK: PUBLIC_MAKE_VARS and the #? annotations agree ($(printf '%s\n' "$declared" | wc -l | tr -d ' ') variables)."
