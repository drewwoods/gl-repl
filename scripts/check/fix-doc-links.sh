#!/bin/bash
# Repair broken Markdown doc links, touching as little as possible.
#
# Repair order, narrowest first:
#   1. --repoint: rewrite drifted line anchors in place. This is what nearly
#      every failure is — an edit inserted lines above the target — and it
#      keeps the link, so nothing has to be re-derived afterwards.
#   2. --strip: for what repointing cannot resolve (the label is gone, the
#      file moved), drop the destination and leave the label.
#   3. link-doc-identifiers --write --only, scoped to JUST the exact
#      file:line pairs stripped in step 2, to re-link those labels against
#      the current tree.
#
# Step 3 is deliberately not run repo-wide, or even file-wide: a plain
# `link-doc-identifiers --write <file>` links *every* bare identifier it
# recognizes in that file, so scoping only by filename still buries a
# two-line anchor repair under however many other pre-existing, never-linked
# identifiers happen to live in the same doc. `--only path:line[,line...]`
# restricts the scan to the exact lines step 2 just stripped, so nothing
# else in the file changes.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

if python3 scripts/check/check-doc-links.py "$@"; then
    echo "    fix-doc-links: no invalid doc links found"
    exit 0
else
    check_status=$?
fi

if [ "$check_status" -ne 1 ]; then
    exit "$check_status"
fi

echo "    fix-doc-links: repointing drifted line anchors..."

if python3 scripts/check/check-doc-links.py --repoint "$@"; then
    echo "    fix-doc-links: verifying repaired doc links..."
    exec python3 scripts/check/check-doc-links.py "$@"
else
    repoint_status=$?
fi

if [ "$repoint_status" -ne 1 ]; then
    exit "$repoint_status"
fi

echo "    fix-doc-links: stripping links that could not be repointed..."

# Collect the exact file:line pairs still failing, so the identifier relink
# stays scoped to just those lines. Errors are reported as
# `<path>:<line>: <message>`.
stripped_locations="$(python3 scripts/check/check-doc-links.py "$@" 2>&1 >/dev/null \
    | sed -n 's/^[[:space:]]*\([^[:space:]:]*\.md\):\([0-9][0-9]*\):.*/\1:\2/p' | sort -u || true)"

if python3 scripts/check/check-doc-links.py --strip "$@"; then
    :
else
    strip_status=$?
    if [ "$strip_status" -ne 1 ]; then
        exit "$strip_status"
    fi
fi

if [ -n "$stripped_locations" ]; then
    stripped_files="$(printf '%s\n' "$stripped_locations" | cut -d: -f1 | sort -u)"
    echo "    fix-doc-links: relinking identifiers stripped from:"
    printf '      %s\n' $stripped_files

    only_args=()
    while IFS= read -r file; do
        lines="$(printf '%s\n' "$stripped_locations" \
            | awk -F: -v f="$file" '$1 == f { print $2 }' | paste -sd, -)"
        only_args+=(--only "${file}:${lines}")
    done <<< "$stripped_files"

    # shellcheck disable=SC2086
    python3 scripts/link-doc-identifiers.py --write "${only_args[@]}" $stripped_files
fi

echo "    fix-doc-links: verifying repaired doc links..."
python3 scripts/check/check-doc-links.py "$@"
