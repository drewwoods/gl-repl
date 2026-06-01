#!/bin/bash
# vendor-freeglut.sh - vendor a freeglut source tree into third_party/freeglut/
# at a given ref, so the build is self-contained (no out-of-tree checkout) and
# the native macOS Cocoa backend can be built statically and linked.
#
#   scripts/vendor-freeglut.sh [<ref>]
#   FREEGLUT_REPO=<url-or-path> scripts/vendor-freeglut.sh [<ref>]
#
# <ref> is any git ref or SHA in the source repo; default `master` (re-pins to
# the current tip). The source repo defaults to upstream freeglut on GitHub but
# can be overridden with the FREEGLUT_REPO env var, which accepts any value
# `git clone` accepts -- another remote URL or a LOCAL PATH to a working clone
# (e.g. a fork that carries the OSMesa backend):
#
#   FREEGLUT_REPO="$HOME/src/freeglut-fork" \
#     scripts/vendor-freeglut.sh osmesa-backend
#
# A local path is cloned by git, so only COMMITTED state is vendored -- commit
# your fork changes first. The resolved source + SHA are recorded in
# third_party/freeglut/VENDORED.txt for reproducibility (note: a local absolute
# path recorded there is machine-specific; push the fork and re-vendor from its
# URL for a portable pin).
#
# Only a curated ALLOWLIST of the tree is copied (build-essential sources +
# license files), not the full repo. The script HARD-FAILS if an allowlisted
# path is missing in the checkout, so a layout change surfaces loudly instead of
# silently producing a broken vendor tree. The allowlist guards only listed
# paths, not a needed-but-omitted one: if a later `make gl-repl` cmake configure
# errors on a missing CONFIGURE_FILE/include() input, add that path to ALLOWLIST
# below.
#
# After vendoring, run `make freeglut-clean` so the static library is rebuilt
# from the new source (the Makefile lib rule has no dependency on these files),
# and update the pinned SHA noted in THIRD_PARTY_LICENSES.md.

set -euo pipefail

REF="${1:-master}"
UPSTREAM_URL="${FREEGLUT_REPO:-https://github.com/freeglut/freeglut.git}"

ROOT="$(git rev-parse --show-toplevel)"
DEST="$ROOT/third_party/freeglut"

# Build-essential subset (all top-level names in the upstream tree).
ALLOWLIST=(
	CMakeLists.txt
	cmake
	src
	include
	COPYING
	AUTHORS
	ChangeLog
	README.md
	README.cmake
	README.macos
)
# Note: every configure_file() input (cmake/freeglut.pc.in,
# cmake/FreeGLUTConfig.cmake.in, cmake/config.h.in, src/fg_version.h.in, ...)
# lives under cmake/ or src/, so the recursive copies above already cover them.

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

echo "Cloning $UPSTREAM_URL ..."
git clone --quiet "$UPSTREAM_URL" "$TMP/freeglut"
git -C "$TMP/freeglut" checkout --quiet "$REF"
SHA="$(git -C "$TMP/freeglut" rev-parse HEAD)"
echo "Checked out '$REF' -> $SHA"

# Verify allowlisted paths exist before touching the destination.
for p in "${ALLOWLIST[@]}"; do
	if [ ! -e "$TMP/freeglut/$p" ]; then
		echo "ERROR: expected path missing in upstream checkout: $p" >&2
		echo "Upstream layout may have changed; update ALLOWLIST in $0." >&2
		exit 1
	fi
done

# Wipe and repopulate.
rm -rf "$DEST"
mkdir -p "$DEST"
for p in "${ALLOWLIST[@]}"; do
	cp -R "$TMP/freeglut/$p" "$DEST/"
done

cat > "$DEST/VENDORED.txt" <<EOF
freeglut vendored into this tree.

upstream: $UPSTREAM_URL
ref:      $REF
sha:      $SHA

Produced by scripts/vendor-freeglut.sh. Do NOT edit the vendored tree by hand;
re-run the script with a ref to update. After re-vendoring, run
\`make freeglut-clean\` so the static library is rebuilt from the new source.
EOF

echo
echo "Vendored freeglut -> third_party/freeglut (sha $SHA)"
echo "Reminders:"
echo "  - update the pinned SHA in THIRD_PARTY_LICENSES.md"
echo "  - run 'make freeglut-clean' before the next build"
