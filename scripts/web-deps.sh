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
#               third_party/web/gl4es. Skips the clone/checkout step entirely
#               (builds whatever is already there); default:
#               third_party/web/gl4es
#   GLU_DIR     Same, for the GLU checkout. Default: third_party/web/GLU
#
# Idempotent: skips the clone+build for a library whose static archive
# already exists. Delete the archive (or the whole third_party/web/<lib>
# dir) to force a rebuild.
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
# for the why). Idempotent: `git apply --check` skips a patch that's
# already applied -- so reusing an existing GL4ES_DIR that already carries
# the fix (e.g. a local working checkout) or re-running against an
# already-patched clone is a no-op, not a failure.
apply_gl4es_patches() {
	local patch
	for patch in "${GL4ES_PATCHES[@]}"; do
		if ( cd "$GL4ES_DIR" && git apply --check "$patch" 2>/dev/null ); then
			echo -e "${CYAN}Applying $(basename "$patch") ...${NC}"
			( cd "$GL4ES_DIR" && git apply "$patch" )
			GL4ES_PATCH_APPLIED=1
		else
			echo -e "${GREEN}$(basename "$patch") already applied (or not needed).${NC}"
		fi
	done
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

cat > "$WEB_DEPS_ROOT/PINNED.txt" <<EOF
Web build dependencies, fetched and built by scripts/web-deps.sh into this
gitignored directory (or reused from GL4ES_DIR / GLU_DIR overrides).

gl4es:
  upstream: $GL4ES_URL
  sha:      $GL4ES_RESOLVED_SHA
  dir:      $GL4ES_DIR

GLU:
  upstream: $GLU_URL
  sha:      $GLU_RESOLVED_SHA
  dir:      $GLU_DIR

Delete a library's static archive (or its whole directory here) to force
web-deps.sh to rebuild it on the next run.
EOF

echo
echo -e "${GREEN}Web deps ready.${NC}  GL4ES_DIR=$GL4ES_DIR  GLU_DIR=$GLU_DIR"
