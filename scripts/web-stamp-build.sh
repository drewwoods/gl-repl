#!/usr/bin/env bash
# web-stamp-build.sh - stamp the source revision into a built web page.
#
#   scripts/web-stamp-build.sh <built-index.html>
#
# packaging/web/shell.html ships @GLR_BUILD_*@ placeholders; emcc copies it
# into the built index.html, and this fills them in afterwards. A deployed
# .wasm is otherwise unidentifiable - there is no way to tell which commit
# produced the page you are looking at.
#
# The commit *date* is used rather than the wall-clock build time, so two
# builds of the same commit stamp identically.
#
# ENVIRONMENT
#   GLR_BUILD_SHA    Override the full SHA (default: git rev-parse HEAD).
#   GLR_BUILD_DATE   Override the commit date (default: git show -s).
#   GLR_BUILD_REPO   owner/name used to build the commit URL.
#   GLR_ANALYTICS_ENDPOINT
#                    Collector URL for the page's usage beacon. UNSET IS THE
#                    DEFAULT AND MEANS OFF: the meta ships empty and the
#                    shell's shim makes no requests at all, so a local build,
#                    a fork, and a self-hosted copy never phone home. Only
#                    the Pages deploy sets it, from a repo variable.
#                    Example: https://<code>.goatcounter.com/count
#
# Never fails for lack of git: outside a checkout (a release tarball, say)
# the values resolve to "unknown" and CSS hides the badge. It *does* fail on
# an unsubstituted placeholder, which would otherwise ship literal
# "@GLR_BUILD_SHA@" text to users.
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $(basename "$0") <built-index.html>" >&2
    exit 2
fi

PAGE=$1
[ -f "$PAGE" ] || { echo "web-stamp-build: no such file: $PAGE" >&2; exit 1; }

REPO=${GLR_BUILD_REPO:-drewwoods/gl-repl}

# The endpoint lands in an HTML attribute and then in a JS string-concatenated
# URL, so validate rather than escape: a value carrying quotes, angle brackets
# or its own query separator would either break the page or silently change
# what the beacon sends. Failing the build is right - a misconfigured
# collector should be loud at deploy time, not a page that quietly reports
# nothing.
ANALYTICS=${GLR_ANALYTICS_ENDPOINT:-}
if [ -n "$ANALYTICS" ]; then
    case "$ANALYTICS" in
        https://*) ;;
        *) echo "web-stamp-build: GLR_ANALYTICS_ENDPOINT must be https://" >&2
           exit 1 ;;
    esac
    case "$ANALYTICS" in
        *[\<\>\"\'\&\ ]*)
           echo "web-stamp-build: GLR_ANALYTICS_ENDPOINT has forbidden characters" >&2
           exit 1 ;;
    esac
fi

sha=${GLR_BUILD_SHA:-}
date=${GLR_BUILD_DATE:-}
if [ -z "$sha" ] && git rev-parse --git-dir >/dev/null 2>&1; then
    sha=$(git rev-parse HEAD 2>/dev/null || echo "")
    # A dirty tree matches no commit, so say so rather than implying the page
    # was built from the named revision. Tracked changes only (-uno):
    # untracked scratch files never reach the build, and counting them would
    # mark every local build dirty forever.
    if [ -n "$sha" ] && [ -n "$(git status --porcelain -uno 2>/dev/null)" ]; then
        sha="$sha-dirty"
    fi
fi
if [ -z "$date" ] && git rev-parse --git-dir >/dev/null 2>&1; then
    date=$(git show -s --format=%cd --date=format-local:'%Y-%m-%dT%H:%MZ' \
           HEAD 2>/dev/null || echo "")
fi

if [ -z "$sha" ]; then
    sha="unknown"
    short="unknown"
    url="https://github.com/$REPO"
    tip="Built from an unknown revision (no git metadata available)."
    state="unknown"
else
    base=${sha%-dirty}
    short=${base:0:9}
    url="https://github.com/$REPO/commit/$base"
    tip="Built from commit $base"
    [ -n "$date" ] && tip="$tip ($date)"
    case "$sha" in
        *-dirty)
            short="$short+"
            tip="$tip, with uncommitted local changes"
            ;;
    esac
    tip="$tip. Click to view the source on GitHub."
    state="known"
fi

tmp="$PAGE.stamp"
SHA="$sha" SHORT="$short" URL="$url" TIP="$tip" STATE="$state" \
ANALYTICS="$ANALYTICS" \
DATE="${date:-unknown}" python3 - "$PAGE" "$tmp" <<'PYEOF'
import html
import os
import re
import sys

src, dst = sys.argv[1], sys.argv[2]

# emcc minifies the shell into index.html and drops quotes around attribute
# values that do not need them - and a bare @GLR_BUILD_TIP@ never needs them.
# Substituting a value containing spaces into a now-unquoted attribute turns
# the rest of the sentence into bogus attributes, so rewrite the whole
# name=value pair and always re-quote.
values = {
    "GLR_BUILD_SHA":   os.environ["SHA"],
    "GLR_BUILD_SHORT": os.environ["SHORT"],
    "GLR_BUILD_URL":   os.environ["URL"],
    "GLR_BUILD_TIP":   os.environ["TIP"],
    "GLR_BUILD_DATE":  os.environ["DATE"],
    "GLR_BUILD_STATE": os.environ["STATE"],
    # Empty unless the deploy configured a collector; see the header.
    "GLR_ANALYTICS_ENDPOINT": os.environ.get("ANALYTICS", ""),
}

text = open(src, encoding="utf-8").read()
for token, value in values.items():
    quoted = html.escape(value, quote=True)
    attr = re.compile(
        r'([a-zA-Z_:][-a-zA-Z0-9_:.]*)=("@TOK@"|\'@TOK@\'|@TOK@)'.replace("TOK", token)
    )
    text = attr.sub(lambda m: '%s="%s"' % (m.group(1), quoted), text)
    # element-text form, e.g. >@GLR_BUILD_SHORT@<
    text = text.replace("@%s@" % token, quoted)

# Every @GLR_*@ token, not just the build stamp's: an unsubstituted
# placeholder of any kind would ship as literal text to users.
leftover = sorted(set(re.findall(r"@GLR_[A-Z_]+@", text)))
if leftover:
    print("web-stamp-build: unsubstituted placeholders: %s" % leftover,
          file=sys.stderr)
    sys.exit(1)

open(dst, "w", encoding="utf-8").write(text)
PYEOF

mv -f "$tmp" "$PAGE" || { rm -f "$tmp"; exit 1; }

echo "web build stamp: ${sha}"
echo "web analytics:    ${ANALYTICS:-<disabled>}"
