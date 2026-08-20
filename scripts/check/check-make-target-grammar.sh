#!/bin/bash
# Hard guard: Make target names follow the grammar, and the exceptions shrink.
#
#   <verb>-<subject>   the rule. Verbs come from MAKE_TARGET_VERBS, so the
#                      grammar and `make help-<verb>` share one list.
#   <verb>             a bare verb is always legal (test, bench, check, ...),
#                      so the aggregates need no allow-listing.
#   ROOT_TARGETS       artifact-named: the target IS the thing it builds.
#                      Permanent, and allowed to stay any size.
#   DEPRECATED_ALIASES an old spelling kept as a forwarder after a rename.
#                      Only needed for one that does not already conform.
#   LEGACY_TARGETS     the ratchet: names that predate the grammar. Frozen.
#
# The ratchet is checked BOTH ways, which is the whole mechanism:
#   - a non-conforming target missing from LEGACY_TARGETS fails, so new drift
#     cannot land;
#   - an entry that no longer names an existing, non-conforming target fails,
#     so renaming or deleting one forces its line out. The list only shrinks.
# Same for ROOT_TARGETS in the "must still exist" direction.
#
# What this cannot check is the `show-` exit contract - that findings are not
# violations, while a report that cannot be produced still exits non-zero.
# That stays a review rule; see docs/plans/partial/makefile-target-conventions.md.
#
# Universe: SOURCE declarations carrying a `## ` doc (which is every public
# target - check-make-target-documented enforces that). internal-* is exempt by
# construction: `internal` is one of the verbs. Pattern rules (`help-%`), FORCE
# and dot-targets are outside the scraped name class.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

: "${MAKE_TARGET_VERBS:?run this through 'make check-make-target-grammar'}"
: "${ROOT_TARGETS:?run this through 'make check-make-target-grammar'}"
: "${LEGACY_TARGETS:?run this through 'make check-make-target-grammar'}"
DEPRECATED_ALIASES=${DEPRECATED_ALIASES-}

documented=$(awk -F: '/^[a-zA-Z0-9_.-]+:.*## / {print $1}' Makefile | sort -u)

conforms() {
    local name=$1 verb
    for verb in $MAKE_TARGET_VERBS; do
        [ "$name" = "$verb" ] && return 0
        case "$name" in "$verb"-*) return 0;; esac
    done
    return 1
}

in_list() {
    local name=$1 entry
    for entry in $2; do [ "$name" = "$entry" ] && return 0; done
    return 1
}

fail=0

# 1. Every documented target is legal.
ungrammatical=""
for t in $documented; do
    conforms "$t" && continue
    in_list "$t" "$ROOT_TARGETS" && continue
    in_list "$t" "$DEPRECATED_ALIASES" && continue
    in_list "$t" "$LEGACY_TARGETS" && continue
    ungrammatical="${ungrammatical}  ${t}"$'\n'
done
if [ -n "$ungrammatical" ]; then
    printf 'Make target names outside the grammar:\n%s' "$ungrammatical" >&2
    printf '\nA target name is <verb>-<subject>, with the verb one of:\n  %s\n' "$MAKE_TARGET_VERBS" >&2
    printf 'If the target IS the artifact it builds (gl-repl, web, a demo), add it\n' >&2
    printf 'to ROOT_TARGETS instead. LEGACY_TARGETS is closed - it only shrinks.\n' >&2
    fail=1
fi

# 2. No allow-list entry outlives its target.
stale=""
for t in $ROOT_TARGETS; do
    in_list "$t" "$documented" || stale="${stale}  ROOT_TARGETS: ${t} (no such documented target)"$'\n'
done
for t in $DEPRECATED_ALIASES; do
    in_list "$t" "$documented" || \
        stale="${stale}  DEPRECATED_ALIASES: ${t} (no such documented target - the forwarder is gone, so drop the line)"$'\n'
done
for t in $LEGACY_TARGETS; do
    if ! in_list "$t" "$documented"; then
        stale="${stale}  LEGACY_TARGETS: ${t} (no such documented target)"$'\n'
    elif conforms "$t"; then
        stale="${stale}  LEGACY_TARGETS: ${t} (now follows the grammar - drop the line)"$'\n'
    fi
done
if [ -n "$stale" ]; then
    printf 'Stale grammar allow-list entries:\n%s' "$stale" >&2
    printf '\nThe lists are ratchets: an entry that no longer names a non-conforming\n' >&2
    printf 'target has to go, so the exception count can only fall.\n' >&2
    fail=1
fi

# 3. A name in both lists makes the ratchet meaningless (the permanent list
#    would be hiding a debt entry).
both=""
for t in $ROOT_TARGETS; do
    in_list "$t" "$LEGACY_TARGETS" && both="${both}  ROOT_TARGETS + LEGACY_TARGETS: ${t}"$'\n'
done
for t in $DEPRECATED_ALIASES; do
    in_list "$t" "$LEGACY_TARGETS" && \
        both="${both}  DEPRECATED_ALIASES + LEGACY_TARGETS: ${t} (a renamed name is not grandfathered)"$'\n'
done
if [ -n "$both" ]; then
    printf 'Name in two grammar lists (pick one):\n%s' "$both" >&2
    fail=1
fi

[ "$fail" -eq 0 ] || exit 1

n_doc=$(printf '%s\n' $documented | wc -l | tr -d ' ')
n_legacy=$(printf '%s\n' $LEGACY_TARGETS | wc -l | tr -d ' ')
n_root=$(printf '%s\n' $ROOT_TARGETS | wc -l | tr -d ' ')
n_dep=$(printf '%s\n' $DEPRECATED_ALIASES | grep -c . || true)
printf 'OK: %s Make targets follow the grammar (%s artifact roots, %s deprecated aliases, %s legacy names left to rename).\n' \
    "$n_doc" "$n_root" "$n_dep" "$n_legacy"
