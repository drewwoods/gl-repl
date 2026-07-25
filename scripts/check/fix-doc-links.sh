#!/bin/bash
# Repair broken Markdown doc links, touching as little as possible.
#
# Repair order, narrowest first:
#   1. --repoint: rewrite drifted line anchors in place. This is what nearly
#      every failure is — an edit inserted lines above the target — and it
#      keeps the link, so nothing has to be re-derived afterwards.
#   2. --strip: for what repointing cannot resolve (the label is gone, the
#      file moved), drop the destination and leave the label.
#   3. link-doc-identifiers --write, scoped to JUST the files stripped in
#      step 2, to re-link those labels against the current tree.
#
# Step 3 is deliberately not run repo-wide: it links every bare identifier it
# recognizes, so an unscoped run buries a two-line anchor repair under a
# hundred unrelated new links in files the change never touched.
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

# Collect the files still failing, so the identifier relink stays scoped to
# them. Errors are reported as `<path>:<line>: <message>`.
stripped_files="$(python3 scripts/check/check-doc-links.py "$@" 2>&1 >/dev/null \
    | sed -n 's/^[[:space:]]*\([^[:space:]:]*\.md\):.*/\1/p' | sort -u || true)"

if python3 scripts/check/check-doc-links.py --strip "$@"; then
    :
else
    strip_status=$?
    if [ "$strip_status" -ne 1 ]; then
        exit "$strip_status"
    fi
fi

if [ -n "$stripped_files" ]; then
    echo "    fix-doc-links: relinking identifiers in stripped files:"
    printf '      %s\n' $stripped_files
    # shellcheck disable=SC2086
    python3 scripts/link-doc-identifiers.py --write $stripped_files
fi

echo "    fix-doc-links: verifying repaired doc links..."
python3 scripts/check/check-doc-links.py "$@"
