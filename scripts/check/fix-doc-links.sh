#!/bin/bash
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

echo "    fix-doc-links: attempting automatic repairs..."

if python3 scripts/check/check-doc-links.py --strip "$@"; then
    :
else
    strip_status=$?
    if [ "$strip_status" -ne 1 ]; then
        exit "$strip_status"
    fi
fi

python3 scripts/link-doc-identifiers.py --write "$@"

echo "    fix-doc-links: verifying repaired doc links..."
python3 scripts/check/check-doc-links.py "$@"
