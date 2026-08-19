#!/bin/bash
# web-deps.sh - fetch & build gl4es + GLU for the Emscripten/wasm web build,
# into gitignored third_party/web/.
#
#   scripts/web-deps.sh
#   GL4ES_DIR=<path> GLU_DIR=<path> scripts/web-deps.sh
#
# Modeled on scripts/vendor-freeglut.sh's pin discipline, but these two
# libraries are *built* here (not just copied) since the wasm archives are
# toolchain-specific and can't be vendored as portable source-only copies of
# convenience.
#
# ENVIRONMENT
#   GL4ES_DIR   Reuse an existing gl4es checkout instead of cloning one into
#               third_party/web/gl4es. The managed clone is reset to the pin
#               when the patch stamp changes. An override is never reset:
#               current patches must apply or already reverse-apply, else
#               the script exits 1. Default: third_party/web/gl4es
#   GLU_DIR     Same, for the GLU checkout. Default: third_party/web/GLU
#
# Idempotent: skips the clone+build for a library whose static archive
# already exists *and* whose patch stamp still matches. Delete the
# archive (or the whole third_party/web/<lib> dir) to force a rebuild.
# Changing any file in GL4ES_PATCHES (or the pin SHA) also forces the
# managed gl4es clone to reset, reapply, and rebuild.
#
# Resolved SHAs are recorded in third_party/web/PINNED.txt.

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
WEB_DEPS_ROOT="$ROOT/third_party/web"

GL4ES_URL="https://github.com/ptitSeb/gl4es.git"
GL4ES_SHA="17f0894e19d1553e4176276c759915dab44c08e2"
GL4ES_PATCHES=(
	"$ROOT/packaging/web/patches/gl4es-rasterpos-perspective-divide.patch"
	"$ROOT/packaging/web/patches/gl4es-bitmap-dirty-clear.patch"
	"$ROOT/packaging/web/patches/gl4es-getter-client-state.patch"
	"$ROOT/packaging/web/patches/gl4es-color-material-face.patch"
	"$ROOT/packaging/web/patches/gl4es-pushattrib-gaps.patch"
	"$ROOT/packaging/web/patches/gl4es-pushattrib-texenv.patch"
	"$ROOT/packaging/web/patches/gl4es-accum-fbo.patch"
	"$ROOT/packaging/web/patches/gl4es-point-smooth.patch"
	"$ROOT/packaging/web/patches/gl4es-polygon-line-drawarrays.patch"
	"$ROOT/packaging/web/patches/gl4es-point-size-batch.patch"
	"$ROOT/packaging/web/patches/gl4es-polygon-offset-line.patch"
	"$ROOT/packaging/web/patches/gl4es-line-width-quads.patch"
)
GLU_URL="https://github.com/ptitSeb/GLU.git"
GLU_SHA="2fed2bda2b725d2b9e32c435b48d5141cc95827f"

GL4ES_DIR="${GL4ES_DIR:-$WEB_DEPS_ROOT/gl4es}"
GLU_DIR="${GLU_DIR:-$WEB_DEPS_ROOT/GLU}"

GL4ES_LIB="$GL4ES_DIR/lib/libGL.a"
GL4ES_INCLUDE="$GL4ES_DIR/include"
GL4ES_GL_H="$GL4ES_INCLUDE/GL/gl.h"
GLU_LIB="$GLU_DIR/.libs/libGLU.a"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'
GL4ES_PATCH_APPLIED=0

if ! command -v emcc >/dev/null 2>&1; then
	echo -e "${RED}Error: emcc not found on PATH.${NC}" >&2
	echo "" >&2
	echo "Install emsdk and source its environment first:" >&2
	echo "  git clone https://github.com/emscripten-core/emsdk.git ~/src/emsdk" >&2
	echo "  ~/src/emsdk/emsdk install latest" >&2
	echo "  ~/src/emsdk/emsdk activate latest" >&2
	echo "  source ~/src/emsdk/emsdk_env.sh" >&2
	echo "" >&2
	echo "Or run scripts/build-web.sh, which sources emsdk for you." >&2
	exit 1
fi

# Local fixes not yet in a public gl4es fork (see each patch file's header
# for the why). The patch *set* is stamped by hashing every file in
# GL4ES_PATCHES. A failed `git apply --check` is NOT treated as "already
# applied": that hid stale trees when a patch file was updated in place
# (the old version was applied, the new one no longer reverse-applies).
#
# The managed clone (third_party/web/gl4es) is reset to GL4ES_SHA and
# reapplied when the stamp changes. A GL4ES_DIR override is never reset;
# mismatch there is a hard error so an older applied version cannot
# silently keep building.
GL4ES_PATCH_STAMP="$GL4ES_DIR/.gl4es-patches.sha"

gl4es_patches_sha() {
	local patch_hash
	if command -v sha256sum >/dev/null 2>&1; then
		patch_hash="$(cat "${GL4ES_PATCHES[@]}" | sha256sum | awk '{print $1}')"
	else
		patch_hash="$(cat "${GL4ES_PATCHES[@]}" | shasum -a 256 | awk '{print $1}')"
	fi
	printf '%s:%s\n' "$GL4ES_SHA" "$patch_hash"
}

apply_gl4es_patches() {
	local patch want have
	want="$(gl4es_patches_sha)"
	have="$(cat "$GL4ES_PATCH_STAMP" 2>/dev/null || true)"
	if [ "$have" = "$want" ]; then
		echo -e "${GREEN}gl4es local patches already current.${NC}"
		return
	fi

	if [ "$GL4ES_DIR" = "$WEB_DEPS_ROOT/gl4es" ]; then
		echo -e "${CYAN}gl4es patch set changed; resetting $GL4ES_SHA and reapplying ...${NC}"
		git -C "$GL4ES_DIR" reset --hard --quiet "$GL4ES_SHA"
		# Applied files get a fresh mtime from git apply, but unpatched
		# sources take the pin's timestamp and can look older than
		# build_wasm/*.o from the previous patch set. Drop the archive
		# and the wasm build dir so the rebuild cannot keep old objects.
		rm -f "$GL4ES_LIB"
		rm -rf "$GL4ES_DIR/build_wasm"
		for patch in "${GL4ES_PATCHES[@]}"; do
			echo -e "${CYAN}Applying $(basename "$patch") ...${NC}"
			( cd "$GL4ES_DIR" && git apply "$patch" )
		done
		printf '%s\n' "$want" > "$GL4ES_PATCH_STAMP"
		GL4ES_PATCH_APPLIED=1
		return
	fi

	echo -e "${CYAN}GL4ES_DIR override $GL4ES_DIR: applying patches (will not reset) ...${NC}"
	for patch in "${GL4ES_PATCHES[@]}"; do
		if ( cd "$GL4ES_DIR" && git apply --check "$patch" 2>/dev/null ); then
			echo -e "${CYAN}Applying $(basename "$patch") ...${NC}"
			( cd "$GL4ES_DIR" && git apply "$patch" )
			GL4ES_PATCH_APPLIED=1
		elif ( cd "$GL4ES_DIR" && git apply --reverse --check "$patch" 2>/dev/null ); then
			echo -e "${GREEN}$(basename "$patch") already applied (current version).${NC}"
		else
			echo -e "${RED}Error: $(basename "$patch") neither applies nor reverse-applies.${NC}" >&2
			echo "The checkout likely still has an older version of this patch." >&2
			echo "Reset it to $GL4ES_SHA, or delete the managed clone and re-run." >&2
			exit 1
		fi
	done
	printf '%s\n' "$want" > "$GL4ES_PATCH_STAMP"
}

build_gl4es() {
	if [ ! -d "$GL4ES_DIR" ]; then
		echo -e "${CYAN}Cloning gl4es ($GL4ES_SHA) ...${NC}"
		git clone --quiet "$GL4ES_URL" "$GL4ES_DIR"
		git -C "$GL4ES_DIR" checkout --quiet "$GL4ES_SHA"
	fi

	apply_gl4es_patches
	if [ -f "$GL4ES_LIB" ] && [ "$GL4ES_PATCH_APPLIED" -eq 0 ]; then
		echo -e "${GREEN}gl4es already built -> $GL4ES_LIB${NC}"
		return
	fi

	echo -e "${CYAN}Building gl4es ...${NC}"
	mkdir -p "$GL4ES_DIR/build_wasm"
	( cd "$GL4ES_DIR/build_wasm" \
	  && emcmake cmake .. -DNOX11=ON -DNOEGL=ON -DSTATICLIB=ON \
	  && emmake make )

	if [ ! -f "$GL4ES_LIB" ]; then
		echo -e "${RED}gl4es build did not produce $GL4ES_LIB${NC}" >&2
		exit 1
	fi
	echo -e "${GREEN}gl4es built -> $GL4ES_LIB${NC}"
}

build_glu() {
	if [ -f "$GLU_LIB" ]; then
		echo -e "${GREEN}GLU already built -> $GLU_LIB${NC}"
		return
	fi

	if [ ! -d "$GLU_DIR" ]; then
		echo -e "${CYAN}Cloning GLU ($GLU_SHA) ...${NC}"
		git clone --quiet "$GLU_URL" "$GLU_DIR"
		git -C "$GLU_DIR" checkout --quiet "$GLU_SHA"
	fi

	echo -e "${CYAN}Building GLU ...${NC}"
	# GLU's configure falls back to AC_CHECK_LIB([GL], [glBegin], ...) when
	# no gl.pc is found (gl4es ships none). That probe force-includes
	# GL4ES_GL_H via CFLAGS below, whose gl_mangle.h renames glBegin to
	# gl4es_glBegin -- colliding with autoconf's own synthesized
	# `char glBegin();` declaration and failing the link test even though
	# gl4es's libGL.a is perfectly usable. Seed the cache variable the test
	# would have set, skipping the broken probe (same trick cross-compiling
	# toolchains use for tests that can't run/link correctly for the
	# target). A plain env var doesn't survive the `config.status --recheck`
	# that `make` triggers on its own, so persist it via --cache-file
	# instead -- config.status re-reads the same cache file.
	local glu_cache="$GLU_DIR/config.cache"
	printf 'ac_cv_lib_GL_glBegin=${ac_cv_lib_GL_glBegin=yes}\n' > "$glu_cache"
	# A fresh git checkout gives every autotools-generated file the same
	# checkout mtime, so make's maintainer-mode rules see aclocal.m4 /
	# configure / Makefile.in as "older than their sources" and try to
	# regenerate them via a `config.status --recheck` that does not carry
	# our --cache-file (or the CFLAGS/CXXFLAGS `emmake make` sets), which
	# then fails both the glBegin probe and an "environment changed"
	# guard. Touch them oldest-to-newest (before *and* after configure,
	# since configure itself rewrites config.status/Makefile) so nothing
	# ever looks stale to make.
	( cd "$GLU_DIR" \
	  && { [ -f configure ] || autoreconf -fi; } \
	  && touch aclocal.m4 configure config.h.in Makefile.am Makefile.in \
	  && emconfigure ./configure --disable-shared --enable-static --cache-file="$glu_cache" \
	  && touch aclocal.m4 configure config.h.in Makefile.am Makefile.in config.status Makefile \
	  && emmake make \
	       CFLAGS="-include $GL4ES_GL_H -I$GL4ES_INCLUDE -D__EMSCRIPTEN__ -DUSE_MGL_NAMESPACE" \
	       CXXFLAGS="-include $GL4ES_GL_H -I$GL4ES_INCLUDE -D__EMSCRIPTEN__ -DUSE_MGL_NAMESPACE -Wno-register" )

	if [ ! -f "$GLU_LIB" ]; then
		echo -e "${RED}GLU build did not produce $GLU_LIB${NC}" >&2
		exit 1
	fi
	echo -e "${GREEN}GLU built -> $GLU_LIB${NC}"
}

mkdir -p "$WEB_DEPS_ROOT"
build_gl4es
build_glu

GL4ES_RESOLVED_SHA="$(git -C "$GL4ES_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
GLU_RESOLVED_SHA="$(git -C "$GLU_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
GL4ES_PATCHES_STAMP="$(cat "$GL4ES_PATCH_STAMP" 2>/dev/null || gl4es_patches_sha)"

cat > "$WEB_DEPS_ROOT/PINNED.txt" <<EOF
Web build dependencies, fetched and built by scripts/web-deps.sh into this
gitignored directory (or reused from GL4ES_DIR / GLU_DIR overrides).

gl4es:
  upstream: $GL4ES_URL
  pin:      $GL4ES_SHA
  sha:      $GL4ES_RESOLVED_SHA
  patches:  $GL4ES_PATCHES_STAMP
  dir:      $GL4ES_DIR

GLU:
  upstream: $GLU_URL
  sha:      $GLU_RESOLVED_SHA
  dir:      $GLU_DIR

A stamp mismatch (pin SHA or any file in GL4ES_PATCHES) resets the managed
gl4es clone and rebuilds it. Delete a library's static archive (or its
whole directory here) to force a rebuild without changing the patches.
EOF

echo
echo -e "${GREEN}Web deps ready.${NC}  GL4ES_DIR=$GL4ES_DIR  GLU_DIR=$GLU_DIR"
