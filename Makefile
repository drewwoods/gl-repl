# Recipes use bash features (`set -o pipefail`, `$'...'` ANSI-C
# quoting for colorized check output). Without this, GNU make runs
# recipes via /bin/sh - which is dash on Debian/Ubuntu - and
# `set -e -o pipefail` aborts with "Illegal option -o pipefail",
# breaking check-state-ownership / test-stubs on Linux. bash is
# present on every supported dev box (macOS /bin/bash, Linux
# /bin/bash); pin it so recipe behavior is identical everywhere.
SHELL := /bin/bash

CC = gcc
ifeq ($(origin CC),command line)
MSAN_CC ?= $(CC)
else
MSAN_CC ?= clang
endif
PROJECT_ROOT := $(abspath .)
LOCAL_INCLUDE := $(abspath include)
SRC_DIR := $(abspath src)
GL_STUB_INCLUDE := $(abspath tests/gl-stubs/include)
TEST_DIR := tests
BENCH_DIR := bench
ZSHRC ?= $(HOME)/.zshrc
ZSH_COMPLETIONS_DIR := $(PROJECT_ROOT)/scripts/completions

# Concise, timed compile summaries are the default. Set either V=1 or
# VERBOSE=1 to restore the compiler command and its output for every source.
V ?=
VERBOSE ?=

# Parallel builds by default, but not too aggressively
ifeq ($(filter -j%,$(MAKEFLAGS)),)
  MAKEFLAGS += -j3
endif

# Color codes for output. ESC holds a real escape byte (not the two-char
# "\033" text) so plain `echo` in recipes - macOS /bin/sh echo does not
# interpret backslash escapes - emits real color, not literal "\033[...".
# printf call sites are unaffected (a literal ESC in the format is fine).
ESC := $(shell printf '\033')
RED := $(ESC)[0;31m
GREEN := $(ESC)[0;32m
YELLOW := $(ESC)[0;33m
CYAN := $(ESC)[0;36m
# No Color
NC := $(ESC)[0m

UNAME_S := $(shell uname -s)

# Declared (empty by default) so `make --warn-undefined-variables` stays
# quiet; empty still fails the `ifeq ($(USE_GL_STUBS),1)` gates below.
USE_GL_STUBS ?=

# Vendored freeglut (third_party/freeglut), built as a static library with the
# native macOS Cocoa backend and linked into the GL binaries. Default on macOS;
# Linux defaults to the system freeglut path. Re-vendor with
# scripts/vendor-freeglut.sh; the pinned commit is recorded in
# third_party/freeglut/VENDORED.txt.
FREEGLUT_SRC        := third_party/freeglut
# `make glut` (Apple GLUT framework fallback) passes FREEGLUT_VENDOR=0 to skip
# building/linking the vendored library.
FREEGLUT_VENDOR     ?= 1

# FREEGLUT_LIB_PATH=<path/to/libglut.a> links an *external* static freeglut
# instead of the one built from third_party/freeglut - for experimenting with a
# different fork/branch/build without re-vendoring. The path is used verbatim
# everywhere $(FREEGLUT_STATIC_LIB) is (link lines, link prerequisites,
# render3d-hot's -force_load, the GL benches), and the CMake build rule is
# replaced by an existence check: nothing under third_party/freeglut is built or
# consulted for the archive.
#
# Headers do NOT follow the archive - point FREEGLUT_INCLUDE_DIR at the matching
# <GL/freeglut.h> whenever the external build's headers differ from the vendored
# ones (a capture-less upstream freeglut is exactly that case: its header lacks
# the fork's declarations). Objects get their own build/*-fgext objdir so they
# never mix with vendored-header objects.
#
#   make gl-repl FREEGLUT_LIB_PATH=~/src/freeglut/build/lib/libglut.a \
#                FREEGLUT_INCLUDE_DIR=~/src/freeglut/include
#
# Everything the archive itself does not carry (frameworks on macOS, X11 libs on
# Linux) still comes from the platform link line below, so an external build has
# to be a static freeglut for the *same* backend as the arm you are building.
FREEGLUT_LIB_PATH   ?=
FREEGLUT_INCLUDE_DIR ?= $(FREEGLUT_SRC)/include

# FREEGLUT_VENDOR_LINUX=1 opts the *windowed Linux* build into the vendored
# freeglut (X11/GLX backend) instead of the distro's -lglut. Off by default:
# system freeglut is the right thing for an ordinary Linux build and needs no
# cmake. Turn it on when you need the fork's frame-capture support - SIGUSR1
# snapshots, FREEGLUT_CAPTURE_FRAMES, FREEGLUT_CAPTURE_STREAM - which upstream
# freeglut does not have, so scripts/docs-assets.sh, record-gif.sh and
# record-video.sh silently produce nothing against the system library.
# (The Linux OSMesa build vendors unconditionally; only this windowed path is
# a choice.) Needs cmake plus the X11/GL dev headers.
FREEGLUT_VENDOR_LINUX ?= 0

# An external archive is linked by path, which on Linux is the vendored-style
# arm (include dir first, no -lglut) - so asking for one implies it.
ifneq ($(FREEGLUT_LIB_PATH),)
  FREEGLUT_VENDOR_LINUX := 1
endif

# FREEGLUT_OSMESA=1 builds the vendored freeglut with its headless OSMesa
# (off-screen software) backend instead of the macOS Cocoa backend, and links
# the GL binaries against Mesa's libGL/libGLU + libOSMesa rather than Apple's
# OpenGL framework. This gives a windowless build that renders through swrast -
# usable for headless geometry/feedback tests (PLY export, the real-GL tests)
# with no display. The OSMesa backend lives in the vendored tree only after
# re-vendoring from a freeglut that carries it (see VENDORED.txt). Build dir and
# static-lib name are kept distinct from the Cocoa build so the two coexist.
FREEGLUT_OSMESA     ?= 0

# WEB=1 builds against Emscripten (emcc) for the browser/wasm target instead
# of a native GL backend: CC becomes emcc, and GL/GLU/freeglut all come from
# the toolchain-built archives in third_party/web/ (scripts/web-deps.sh) plus
# the vendored freeglut's Emscripten backend. Mutually exclusive with
# FREEGLUT_OSMESA -- don't combine them. See `make web`.
#
# WEB=1 USE_GL_STUBS=1 IS supported and is what `make test-web` uses: emcc
# against the no-op GL stubs, no gl4es, no browser. See WEB_TEST_LDFLAGS.
WEB                 ?= 0
ifeq ($(WEB),1)
  CC := emcc
endif

# Where scripts/web-deps.sh fetches/builds gl4es + GLU (WEB=1 only).
# Overridable to reuse an existing checkout -- see scripts/web-deps.sh.
GL4ES_DIR ?= third_party/web/gl4es
GLU_DIR   ?= third_party/web/GLU

ifeq ($(WEB),1)
  FREEGLUT_BUILD          := $(FREEGLUT_SRC)/build-wasm
  FREEGLUT_STATIC_LIB     := $(FREEGLUT_BUILD)/lib/libglut.a
  FREEGLUT_CMAKE_LAUNCHER := emcmake
  FREEGLUT_CMAKE_BACKEND  := -DFREEGLUT_REPLACE_GLUT=ON -DCMAKE_C_FLAGS="-include $(abspath $(GL4ES_DIR))/include/GL/gl.h -I$(abspath $(GL4ES_DIR))/include"
else ifeq ($(FREEGLUT_OSMESA),1)
  FREEGLUT_BUILD          := $(FREEGLUT_SRC)/build-osmesa
  FREEGLUT_STATIC_LIB     := $(FREEGLUT_BUILD)/lib/libglut_osmesa.a
  FREEGLUT_CMAKE_LAUNCHER :=
  FREEGLUT_CMAKE_BACKEND  := -DFREEGLUT_OSMESA=ON -DFREEGLUT_GLES=OFF
else
  FREEGLUT_BUILD          := $(FREEGLUT_SRC)/build
  FREEGLUT_STATIC_LIB     := $(FREEGLUT_BUILD)/lib/libglut.a
  FREEGLUT_CMAKE_LAUNCHER :=
  ifeq ($(UNAME_S),Darwin)
    FREEGLUT_CMAKE_BACKEND := -DFREEGLUT_COCOA=ON
  else
    # Linux windowed: the stock X11/GLX backend. Only reached under
    # FREEGLUT_VENDOR_LINUX=1 (see below) - the default Linux build links
    # system freeglut and never builds this.
    FREEGLUT_CMAKE_BACKEND := -DFREEGLUT_COCOA=OFF -DFREEGLUT_GLES=OFF
  endif
endif

# An external archive replaces the vendored one for every consumer at once.
# FREEGLUT_BUILD is left pointing at the vendored build dir so `make
# freeglut-clean` still cleans what it always cleaned.
ifneq ($(FREEGLUT_LIB_PATH),)
  FREEGLUT_STATIC_LIB := $(FREEGLUT_LIB_PATH)
endif

# FREEGLUT_HEADER_CFLAGS is set per-platform in the Darwin/Linux block below:
# the vendored include dir on macOS (placed first, so a stray homebrew freeglut
# can't shadow it), empty on Linux (system <GL/freeglut.h> is on the default path).
ifeq ($(USE_GL_STUBS),1)
GL_HEADER_CFLAGS = \
	-I$(GL_STUB_INCLUDE) \
	-DGL_STUBS
else
GL_HEADER_CFLAGS = \
	$(FREEGLUT_HEADER_CFLAGS) \
	-I/usr/include
endif

# Detect compiler type
COMPILER_OUTPUT := $(shell $(CC) --version 2>&1)
ifneq ($(findstring clang,$(COMPILER_OUTPUT)),)
    # It's Apple Clang or LLVM Clang (even if CC=gcc on Mac)
    COMPILER_TYPE := clang
else ifneq ($(findstring Free Software Foundation,$(COMPILER_OUTPUT)),)
    # It's genuine GNU GCC
    COMPILER_TYPE := gcc
else
    COMPILER_TYPE := unknown
endif

# Now if we are gcc add some gcc specific flags warnings
ifeq ($(COMPILER_TYPE),gcc)
  GCC_EXTRA_WARNINGS := -Wduplicated-cond -Wduplicated-branches -Wlogical-op
else
  GCC_EXTRA_WARNINGS :=
endif

# Language standard: C99, project-wide, no exceptions. Everything
# (gl-repl, tests, demos, bench, CI) compiles -std=c99 so the project
# runs on old machines / old GCC. Non-pedantic by default - GNU
# extensions GCC accepts in -std=c99 are fine; the goal is "old gcc
# compiles it", not pure ISO C99. The shipped/real binaries (gl-repl,
# bench, demos) are additionally held to -pedantic-errors by the
# `make check-c99` ratchet (syntax-only, in the standard gate); tests are
# plain -std=c99 (the pedantic delta there is real work, not a no-op,
# and tests are not the shipped artifact).

# -std=c99 sets __STRICT_ANSI__, so glibc hides POSIX-but-not-ISO
# functions (strdup, mkdtemp, ...) from its headers. Without a
# feature-test macro they'd be implicitly declared -> assumed to
# return int -> 64-bit pointer truncated -> segfault at runtime
# (only on glibc/Linux; macOS libc exposes them anyway). _GNU_SOURCE
# is the standard "build glibc code as -std=c99" switch and is benign
# on macOS. -Werror=implicit-function-declaration makes any future
# such hidden-symbol regression a hard compile error instead of a
# silent pointer-truncation crash.
#
# -Werror=switch (already warned by -Wall) upgrades "enumeration value not
# handled in switch" to an error, so a switch that opts out of a `default:`
# has to name every enumerator. It is worth the ceremony because CmdType is
# the project's widest enum and is dispatched on in a dozen TUs: a new command
# that a switch forgets is a silent behaviour gap, not a crash. Verified
# clean under both Apple clang and GCC 14 when introduced. A switch that
# genuinely does not care still keeps its `default:` and is unaffected;
# dropping one is how a fold declares "this list is exhaustive on purpose"
# (see gl_state_apply_cmd in src/repl/gl_state_inspector.c and
# repl_attrib_bits_for_cmd in src/repl/attrib_bits.c - the latter has the
# strongest claim to it, because its answer gates the coverage sweep that
# would otherwise catch an unclassified command).

# Debug info, deliberately not a blanket -ggdb -g3. Emscripten is the reason:
# emcc keeps DWARF inside the .wasm and, when it is present, falls back to
# "limited binaryen optimizations" at link. Measured on the shipped page:
# index.wasm went 5.44 MB -> 1.82 MB, i.e. 3.6 MB the browser no longer has
# to fetch and compile before the first frame. Runtime throughput was
# unaffected either way (bench_repl, within noise over three rounds), so this
# is a payload/startup fix and not a speed one -- don't expect it to move the
# CPU numbers.
#
# Native builds keep full DWARF: there it costs nothing at run time and it
# is what makes a crash report or an Instruments trace readable. Only the
# WEB=1 release link drops it.
#
# Deferred $(if ...) rather than a parse-time ifeq because BUILD is selected
# further down this file. Override to force either way, e.g.
# `make web DEBUG_INFO_CFLAGS=-g2` when you need named frames in a browser
# profile, or `make gl-repl DEBUG_INFO_CFLAGS=-g0` for a lean native binary.
DEBUG_INFO_CFLAGS ?= \
	$(if $(or $(filter quick,$(BUILD)),$(and $(filter 1,$(WEB)),$(filter release,$(BUILD)))),-g0,-ggdb -g3)

COMMON_CFLAGS = \
	-Wall $(DEBUG_INFO_CFLAGS) \
	-Wno-deprecated-declarations -Wfloat-conversion \
	-std=c99 -D_GNU_SOURCE -Werror=implicit-function-declaration \
	-Werror=switch \
	-DGL_SILENCE_DEPRECATION -DFREEGLUT_STATIC \
	$(GL_HEADER_CFLAGS) \
	-I$(PROJECT_ROOT) \
	-I$(SRC_DIR) \
	-I$(LOCAL_INCLUDE) \
	-Wshadow \
	$(GCC_EXTRA_WARNINGS)

RELEASE_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O2

# Defence-in-depth checks that are unreachable by contract but sit on a hot
# path (config.h GLR_DEBUG_CHECKS). On in every non-release configuration -
# the test suite runs in a debug build, so the checks keep their coverage
# exactly where a contract violation would be caught. Diagnostics only:
# results must not differ between the two builds.
DEBUG_CHECK_CFLAGS = -DGLR_DEBUG_CHECKS=1

ifeq ($(NOSAN),1)
NO_SAN := 1
endif

# `ASAN=0` is the positive-polarity spelling of the more precise project-wide
# `NO_SAN=1`; both select the no-sanitizer debug build. `ASAN=1` is the
# explicit affirmative (sanitizers on, which is also the default).
ASAN ?=
ifeq ($(ASAN),0)
NO_SAN := 1
endif

SAN ?= address
DEBUG_SAN_SUFFIX =

ifeq ($(NO_SAN),1)
DEBUG_SAN_SUFFIX = -nosan
DEBUG_CFLAGS = \
	$(COMMON_CFLAGS) \
	$(DEBUG_CHECK_CFLAGS) \
	-O0
else ifeq ($(SAN),memory)
DEBUG_SAN_SUFFIX = -msan
DEBUG_CFLAGS = \
	$(COMMON_CFLAGS) \
	$(DEBUG_CHECK_CFLAGS) \
	-O1 \
	-fsanitize=memory -fsanitize-memory-track-origins=2 \
	-fno-omit-frame-pointer \
	-Qunused-arguments \
	-mllvm -fast-isel=false
else ifneq ($(SAN),address)
$(error unsupported SAN=$(SAN); use SAN=address, SAN=memory, or NO_SAN=1)
else
DEBUG_CFLAGS = \
	$(COMMON_CFLAGS) \
	$(DEBUG_CHECK_CFLAGS) \
	-O0 \
	-fsanitize=address -fno-omit-frame-pointer \
	-fsanitize=undefined -fno-sanitize-recover=undefined
endif

COVERAGE_CFLAGS = \
	$(COMMON_CFLAGS) \
	$(DEBUG_CHECK_CFLAGS) \
	-O0 \
	--coverage -fprofile-arcs -ftest-coverage

QUICK_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O0

# Test runners build & run under the debug sanitizer build
# (AddressSanitizer + UBSan by default, or MemorySanitizer with SAN=memory);
# gl-repl, bench and the demos stay release. The public `test` target enters
# `test-stubs`, whose default NO_SAN=1 keeps the broad headless suite quick;
# use `make test NO_SAN=0` or `make test-asan-ubsan` for its sanitized form.
# An explicit `BUILD=...` on the command line or in the environment always
# wins, so `make coverage` (BUILD=coverage) and `make test BUILD=release`
# keep working.
ifeq ($(origin BUILD),command line)
# explicit BUILD - honor it
else ifeq ($(origin BUILD),environment)
# explicit BUILD - honor it
else ifneq ($(filter test test-detailed test-stubs test-no-checks test-only test-msan test-full test-scenes,$(MAKECMDGOALS)),)
BUILD := debug
else
BUILD := release
endif

ifeq ($(BUILD),debug)
BUILD_CFLAGS = $(DEBUG_CFLAGS)
else ifeq ($(BUILD),coverage)
BUILD_CFLAGS = $(COVERAGE_CFLAGS)
else ifeq ($(BUILD),quick)
BUILD_CFLAGS = $(QUICK_CFLAGS)
else
BUILD_CFLAGS = $(RELEASE_CFLAGS)
endif

# The report shows the flags that distinguish this build mode (plus explicit
# user flags), once at startup. The common warning/include flags are stable
# across modes and would drown the useful part of the line; DEBUG_INFO_CFLAGS
# is added back because it is selected by BUILD and is the reason a quick
# build reports `-g0 -O0`, for example.
BUILD_REPORT_FLAGS = $(DEBUG_INFO_CFLAGS) \
	$(filter-out $(COMMON_CFLAGS),$(BUILD_CFLAGS)) \
	$(CPPFLAGS) $(CFLAGS)

# Keep the parameter line compact and meaningful: BUILD is always useful,
# while the remaining entries are shown when the caller selected them. This
# preserves the spelling used by the caller (`ASAN` versus `NO_SAN`).
BUILD_REPORT_PARAMS = $(strip \
	BUILD=$(BUILD) \
	$(if $(filter command line environment,$(origin ASAN)),ASAN=$(ASAN),) \
	$(if $(filter command line environment,$(origin NO_SAN)),NO_SAN=$(NO_SAN),) \
	$(if $(filter command line environment,$(origin NOSAN)),NOSAN=$(NOSAN),) \
	$(if $(filter command line environment,$(origin SAN)),SAN=$(SAN),) \
	$(if $(filter command line environment,$(origin USE_GL_STUBS)),USE_GL_STUBS=$(USE_GL_STUBS),) \
	$(if $(filter command line environment,$(origin WEB)),WEB=$(WEB),) \
	$(if $(filter command line environment,$(origin FREEGLUT_OSMESA)),FREEGLUT_OSMESA=$(FREEGLUT_OSMESA),) \
	$(if $(filter command line environment,$(origin FREEGLUT_VENDOR)),FREEGLUT_VENDOR=$(FREEGLUT_VENDOR),) \
	$(if $(FREEGLUT_LIB_PATH),FREEGLUT_LIB_PATH=$(FREEGLUT_LIB_PATH),) \
	$(if $(filter command line environment,$(origin CC)),CC=$(CC),) \
	)

ifeq ($(UNAME_S),Darwin)
  ifeq ($(FREEGLUT_OSMESA),1)
    # macOS headless: vendored static freeglut (OSMesa backend) + Mesa GL/GLU +
    # libOSMesa, no Apple OpenGL/Cocoa frameworks. Mesa comes from Homebrew
    # (`brew install mesa mesa-glu`); the rpaths let the binary find the dylibs
    # without DYLD_LIBRARY_PATH. Audio frameworks stay (unchanged from the Cocoa
    # build). The vendored freeglut include dir is first so its <GL/freeglut.h>
    # wins; <GL/gl.h>/<GL/glu.h> resolve from the Mesa includes.
    MESA_PREFIX     := $(shell brew --prefix mesa 2>/dev/null)
    MESA_GLU_PREFIX := $(shell brew --prefix mesa-glu 2>/dev/null)
    FREEGLUT_HEADER_CFLAGS = -DFREEGLUT_OSMESA=1 -I$(FREEGLUT_INCLUDE_DIR) -I$(MESA_PREFIX)/include -I$(MESA_GLU_PREFIX)/include
    ifeq ($(FREEGLUT_VENDOR),1)
      FREEGLUT_LIB := $(FREEGLUT_STATIC_LIB)
    endif

    # -lOSMesa must precede -lGL: macOS two-level namespace binds each gl*
    # symbol to the first library on the link line that exports it. Mesa's
    # libOSMesa carries its own self-contained dispatch wired up by
    # OSMesaMakeCurrent; libGL is the GLX/X11 dispatcher, which never has a
    # current context here - gl* bound to it are silent no-ops (glGetString
    # returns NULL, nothing rasterizes, captures come out black).
    OSMESA_GL_LDFLAGS = \
	$(FREEGLUT_LIB) \
	-L$(MESA_PREFIX)/lib -lOSMesa -lGL -L$(MESA_GLU_PREFIX)/lib -lGLU \
	-Wl,-rpath,$(MESA_PREFIX)/lib -Wl,-rpath,$(MESA_GLU_PREFIX)/lib \
	-lm -lpthread \
	-framework CoreAudio -framework CoreFoundation -framework AudioToolbox

    GLUT_GL_LDFLAGS = $(OSMESA_GL_LDFLAGS)
    GL_LDFLAGS      = $(OSMESA_GL_LDFLAGS)

    GL_STUB_LDFLAGS = \
	-lm -lpthread \
	-framework CoreAudio -framework CoreFoundation -framework AudioToolbox
  else
    # macOS: system frameworks + vendored static freeglut (Cocoa backend).
    FREEGLUT_HEADER_CFLAGS = -I$(FREEGLUT_INCLUDE_DIR)
    ifeq ($(FREEGLUT_VENDOR),1)
      FREEGLUT_LIB := $(FREEGLUT_STATIC_LIB)
    endif

    GLUT_GL_LDFLAGS = \
	-lm -lpthread \
	-framework IOKit -framework Cocoa -framework OpenGL -framework GLUT \
	-framework CoreAudio -framework CoreFoundation -framework AudioToolbox

    # Vendored freeglut linked by archive path (no -lglut / no rpath). The Cocoa
    # backend pulls in CoreVideo; a static archive carries no framework deps of its
    # own, so the consumer must list them. FREEGLUT_LIB is empty under `make glut`
    # (FREEGLUT_VENDOR=0), where GLUT_GL_LDFLAGS overrides this anyway.
    GL_LDFLAGS = \
	$(FREEGLUT_LIB) -lm -lpthread \
	-framework IOKit -framework Cocoa -framework OpenGL -framework CoreVideo \
	-framework CoreAudio -framework CoreFoundation -framework AudioToolbox

    GL_STUB_LDFLAGS = \
	-lm -lpthread \
	-framework CoreAudio -framework CoreFoundation -framework AudioToolbox
  endif
else
  ifeq ($(FREEGLUT_OSMESA),1)
    # Linux headless: vendored static freeglut (OSMesa backend) + libOSMesa for the
    # GL entry points, system GLU for the tessellator/quadrics. Needs
    # `libosmesa6-dev` (GL/osmesa.h + osmesa.pc + libOSMesa). Unlike the default
    # Linux path, this DOES vendor freeglut (system freeglut has no OSMesa backend),
    # so its include dir supplies <GL/freeglut.h>; <GL/gl.h>/<GL/glu.h> resolve from
    # the system Mesa headers on the default path. libGLU pulls libGL via DT_NEEDED;
    # both share Mesa's libglapi dispatch with libOSMesa, so they coexist.
    FREEGLUT_HEADER_CFLAGS = -DFREEGLUT_OSMESA=1 -I$(FREEGLUT_INCLUDE_DIR)
    ifeq ($(FREEGLUT_VENDOR),1)
      FREEGLUT_LIB := $(FREEGLUT_STATIC_LIB)
    endif

    OSMESA_GL_LDFLAGS = \
      $(FREEGLUT_LIB) -lOSMesa -lGLU -lGL -lm -lpthread -ldl

    GLUT_GL_LDFLAGS = $(OSMESA_GL_LDFLAGS)
    GL_LDFLAGS      = $(OSMESA_GL_LDFLAGS)
    GL_STUB_LDFLAGS = \
      -lm -lpthread -ldl
  else
    # Linux: GL/GLU from the system; miniaudio dlopen()s pulseaudio/alsa at
    # runtime, so we only need -ldl (plus the existing -lpthread -lm).
    #
    # freeglut itself is the system one by default (<GL/freeglut.h> is already
    # on the default include path, no cmake needed). FREEGLUT_VENDOR_LINUX=1
    # switches to the vendored static X11/GLX build instead - the include dir
    # goes first so it wins over the system header, and the archive is linked
    # by path with no -lglut, mirroring the macOS arm. That is the build that
    # has the fork's frame capture; see the flag's comment up top.
    ifeq ($(FREEGLUT_VENDOR_LINUX),1)
      FREEGLUT_HEADER_CFLAGS = -I$(FREEGLUT_INCLUDE_DIR)
      ifeq ($(FREEGLUT_VENDOR),1)
        FREEGLUT_LIB := $(FREEGLUT_STATIC_LIB)
      endif
      # The vendored static lib does not carry its X11 dependencies, so the
      # consumer lists them (freeglut's own CMake links exactly these).
      LINUX_GL_LDFLAGS = \
        $(FREEGLUT_LIB) -lGL -lGLU -lX11 -lXi -lXrandr -lXxf86vm \
        -lm -lpthread -ldl
    else
      FREEGLUT_HEADER_CFLAGS =
      LINUX_GL_LDFLAGS = \
        -lglut -lGL -lGLU -lm -lpthread -ldl
    endif

    GLUT_GL_LDFLAGS = $(LINUX_GL_LDFLAGS)
    GL_LDFLAGS      = $(LINUX_GL_LDFLAGS)

    GL_STUB_LDFLAGS = \
      -lm -lpthread -ldl
  endif
endif

ifeq ($(USE_GL_STUBS),1)
GLUT_GL_LDFLAGS = $(GL_STUB_LDFLAGS)
GL_LDFLAGS = $(GL_STUB_LDFLAGS)
endif

# WEB=1: swap native GL headers/libs for gl4es + the vendored freeglut's
# Emscripten backend + GLU, all built by scripts/web-deps.sh /
# $(FREEGLUT_STATIC_LIB) into third_party/web/ and third_party/freeglut/.
# The windowing layer at runtime is Emscripten's own JS GLUT
# (library_glut.js); the patched freeglut only supplies solids/fonts (see
# packaging/web/README.md).
ifeq ($(WEB),1)
GL_HEADER_CFLAGS = \
	-include $(GL4ES_DIR)/include/GL/gl.h \
	-I$(GL4ES_DIR)/include -I$(GLU_DIR)/include -I$(FREEGLUT_INCLUDE_DIR) \
	-DUSE_MGL_NAMESPACE -DCFG_DEFAULT_VERTEX_OUTLINES=0 \
	-DCFG_DEFAULT_VERTEX_POINTS=1 -std=gnu99
# The trailing -std=gnu99 lands after COMMON_CFLAGS' -std=c99 on the compile
# line (GL_HEADER_CFLAGS is folded in after -std=c99 there), so it wins for
# this build only -- miniaudio's WebAudio backend needs EM_ASM, a gnu-mode
# extension. The native -std=c99 check-c99 ratchet never sees WEB=1 objects.

# The wasm GL stack and the runtime sizing, split out from GL_LDFLAGS so the
# benchmark link (BENCH_WEB_LDFLAGS, below) can reuse them without inheriting
# the app-page glue.
WEB_GL_ARCHIVES = \
	$(GL4ES_DIR)/lib/libGL.a $(GLU_DIR)/.libs/libGLU.a $(FREEGLUT_STATIC_LIB)
WEB_RUNTIME_LDFLAGS = \
	-sUSE_WEBGL2=1 -sFULL_ES2=1 -sINITIAL_MEMORY=805306368 \
	-sSTACK_SIZE=8388608 -sGL_MAX_TEMP_BUFFER_SIZE=67108864

GL_LDFLAGS = \
	packaging/web/gl4es_bootstrap.c $(WEB_GL_ARCHIVES) \
	--shell-file packaging/web/shell.html \
	$(WEB_RUNTIME_LDFLAGS) \
	-sEXPORTED_FUNCTIONS=_main,_glr_web_new_scene,_glr_web_load_scene_text,_glr_web_export_scene,_glr_web_cfg_share_text,_glr_web_apply_cfg_text,_glr_web_clipboard_copy,_glr_web_clipboard_cut,_glr_web_clipboard_text,_glr_web_clipboard_kind,_glr_web_clipboard_paste_text,_glr_audio_web_manifest_begin,_glr_audio_web_manifest_add,_glr_audio_web_manifest_finish \
	-sEXPORTED_RUNTIME_METHODS=ccall,FS
GLUT_GL_LDFLAGS = $(GL_LDFLAGS)

# Link set for `make bench-web`: the same wasm GL stack, minus everything
# that only means something on the app page. Three deliberate differences:
#
#   - no packaging/web/gl4es_bootstrap.c. Its constructor calls
#     document.querySelector, which throws before main() ever runs outside a
#     browser. The benchmark emits no GL, so it needs none of that glue.
#   - no --shell-file / -sEXPORTED_FUNCTIONS. There is no HTML page and no JS
#     caller here, and the glr_web_* symbols aren't even in this link.
#   - -sEXIT_RUNTIME=1, so main()'s return value reaches node as an exit
#     status. That is what lets bench-web fail a run loudly.
BENCH_WEB_LDFLAGS = \
	$(WEB_GL_ARCHIVES) $(WEB_RUNTIME_LDFLAGS) -sEXIT_RUNTIME=1

# WEB=1 USE_GL_STUBS=1 -- the `make test-web` lane. Re-assert the stub headers
# and drop the whole gl4es stack, because the two flags above set GL_HEADER_CFLAGS
# and this block runs second. That combination is deliberate, not an accident:
# `make test` is already a stubs build, so compiling the same TUs under emcc
# against the same no-op headers is the wasm twin of the native gate, and it
# needs neither scripts/web-deps.sh nor a browser. It compiles the
# __EMSCRIPTEN__ side of every #ifdef in the tree, which is the coverage the
# native suite structurally cannot reach.
ifeq ($(USE_GL_STUBS),1)
GL_HEADER_CFLAGS = -I$(GL_STUB_INCLUDE) -DGL_STUBS -std=gnu99
# Three link differences from BENCH_WEB_LDFLAGS:
#   - NODERAWFS=1: tests fopen() real paths (build/*_trace.txt, tests/testdata
#     fixtures, /tmp workspaces). MEMFS has none of them, and fopen() failing
#     silently turns into confusing assertion diffs rather than an error.
#   - EXIT_RUNTIME=1 on EVERY binary, not just the ones that opt in: without it
#     main()'s non-zero return never reaches node and a failing test exits 0.
#   - no -sUSE_WEBGL2/FULL_ES2: the stubs are the GL layer here.
WEB_TEST_LDFLAGS = \
	-sEXIT_RUNTIME=1 -sNODERAWFS=1 \
	-sINITIAL_MEMORY=536870912 -sSTACK_SIZE=8388608
# All three, not just GL_LDFLAGS: the test binaries that sit outside
# CORE_TEST_BINS name GL_STUB_LDFLAGS directly, and a link that misses
# NODERAWFS above fails as a handful of stub-trace assertions reading 0 rather
# than as a link or a startup error.
GL_STUB_LDFLAGS = $(WEB_TEST_LDFLAGS)
GL_LDFLAGS = $(WEB_TEST_LDFLAGS)
GLUT_GL_LDFLAGS = $(WEB_TEST_LDFLAGS)
endif
endif

ifeq ($(BUILD),coverage)
COVERAGE_LDFLAGS = --coverage
else
COVERAGE_LDFLAGS =
endif

all: gl-repl

# Used by generated files that must always refresh their checked output.
FORCE:

SUPPORT_SRCS = $(wildcard src/support/*.c)

APP_CONTROLLER_SRCS = $(wildcard src/app/*.c) $(wildcard src/app/boot/*.c)

EDITOR_SRCS = $(wildcard src/editor/*.c)

REPL_SRCS = $(wildcard src/repl/*.c)

RENDER3D_SRCS = $(wildcard src/render3d/*.c src/render3d/guides/*.c)

UI_CORE_SRCS = $(wildcard src/ui/core/*.c)

UI_APP_SRCS = $(wildcard src/ui/app/*.c)

UI_SUPPORT_SRCS = $(wildcard src/ui/support/*.c)

UI_SUBSYSTEMS_SRCS = $(wildcard src/ui/subsystems/*.c)

SUBSYSTEMS_SRCS = $(wildcard src/subsystems/*/*.c)

GL_STUB_COUNTS_SRCS = \
	tests/gl-stubs/gl_stub_counts.c

SRCS = \
	$(SUPPORT_SRCS) \
	$(APP_CONTROLLER_SRCS) \
	$(EDITOR_SRCS) \
	$(RENDER3D_SRCS) \
	$(REPL_SRCS) \
	$(UI_CORE_SRCS) \
	$(UI_APP_SRCS) \
	$(UI_SUPPORT_SRCS) \
	$(UI_SUBSYSTEMS_SRCS) \
	$(SUBSYSTEMS_SRCS) \
	$(GL_STUB_COUNTS_SRCS) \
	gl_repl.c

HDRS = \
	$(wildcard src/app/*.h) \
	$(wildcard src/app/boot/*.h) \
	$(wildcard src/editor/*.h) \
	$(wildcard src/render3d/*.h) \
	$(wildcard src/render3d/guides/*.h) \
	$(wildcard src/repl/*.h) \
	$(wildcard src/ui/core/*.h) \
	$(wildcard src/ui/app/*.h) \
	$(wildcard src/ui/support/*.h) \
	$(wildcard src/ui/subsystems/*.h) \
	$(wildcard src/subsystems/*/*.h) \
	$(wildcard src/support/*.h) \
	gl_repl.h \
	source_document.h

# The trimmed emscripten catalog belongs to the shipping web build only. The
# `make test-web` lane (WEB=1 USE_GL_STUBS=1) keeps the native catalog: the
# example goldens are keyed by catalog index, so the trimmed set would fail
# test_repl_core_examples for a reason that has nothing to do with wasm.
ifeq ($(WEB)$(USE_GL_STUBS),1)
EXAMPLES_CATALOG ?= examples/catalog-emscripten.ini
else
EXAMPLES_CATALOG ?= examples/catalog.ini
endif
EXAMPLE_SCENE_SRCS = $(wildcard $(dir $(EXAMPLES_CATALOG))scenes/*.glr) $(wildcard $(dir $(EXAMPLES_CATALOG))scenes/*.c)
GENERATED_EXAMPLES_INC = build/generated/repl_examples_data.inc
ifeq ($(WEB)$(USE_GL_STUBS),1)
TOURS_CATALOG = tours/catalog-emscripten.ini
else
TOURS_CATALOG = tours/catalog.ini
endif
TOUR_SCRIPT_SRCS = $(wildcard tours/*.pointer)
GENERATED_TOURS_INC = build/generated/glr_tours_data.inc
COMMAND_DESCRIPTIONS_SOURCE = src/repl/command_descriptions.txt
GENERATED_COMMAND_DESCRIPTIONS_INC = build/generated/repl_command_descriptions_data.inc

UI_SRCS = $(UI_CORE_SRCS) $(UI_APP_SRCS)
RENDER3D_HDRS = $(filter src/render3d/%.h,$(HDRS))
UI_HDRS = $(filter src/ui/core/%.h src/ui/app/%.h,$(HDRS))
STATE_NEUTRAL_SRCS = src/repl/format.c src/support/memprof.c src/support/cpuprof.c src/support/histogram.c src/support/runstats.c src/support/gpuprof.c tests/gl-stubs/gl_stub_counts.c

# Object lists used to build the standalone render3d_demo without dragging in
# any REPL editor/controller code. The scene and profiling objects are
# independent of the REPL pipeline; controller callbacks provide replay
# baseline restore and geometry-guide argument parsing.
RENDER3D_DEMO_DEP_SRCS = $(RENDER3D_SRCS) src/support/cpuprof.c src/support/histogram.c src/support/runstats.c \
                      tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone repl_demo. It proves that the REPL pipeline
# links without editor input dispatch, the controller, or the UI. Per-line text
# comes from the static source_document adapter in tools/repl_demo. Controller
# bridges and opaque parameters provide the app/editor state the pipeline
# needs, so tools/repl_demo/stubs.c contains no function bodies. Adding a new
# pipeline source that pulls in app or editor symbols is a regression; the
# check-repl-demo-stubs-shrinking and check-repl-no-direct-editor guards catch
# it.
REPL_DEMO_DEP_SRCS = src/repl/format.c \
				     src/support/cpuprof.c src/support/histogram.c src/support/runstats.c \
                     src/subsystems/replay/replay.c \
                     src/subsystems/replay/replay_fade.c \
                     src/subsystems/replay/replay_input.c \
                     src/subsystems/replay/replay_playback.c \
                     src/subsystems/replay/replay_state.c \
                     src/subsystems/tutorial/tutorial_state.c \
					 src/repl/apply.c \
                     src/repl/attrib_bits.c \
                     src/repl/autonormal.c \
                     src/repl/bootstrap.c \
                     src/repl/camera_header.c \
                     src/repl/cfg_baseline.c \
                     src/repl/command_spec.c \
                     src/repl/command_store.c \
                     src/repl/compile.c \
                     src/repl/doc_order.c \
                     src/repl/eval.c \
                     src/repl/example_loader.c \
                     src/repl/examples.c \
                     src/repl/executor.c \
                     src/repl/export.c \
                     src/repl/export_cmd_writer.c \
                     src/repl/export_display.c \
                     src/repl/export_glr.c \
                     src/repl/export_prologue.c \
                     src/repl/export_setup.c \
                     src/repl/expr_program.c \
                     src/repl/flatten.c \
                     src/repl/flatten_expr.c \
                     src/repl/flatten_query.c \
                     src/repl/geometry_query.c \
                     src/repl/host_effects.c \
                     src/repl/import.c \
                     src/repl/keymap_format.c \
                     src/repl/line_scan.c \
                     src/repl/load.c \
                     src/repl/normalize.c \
                     src/repl/parser.c \
                     src/repl/program_query.c \
                     src/repl/reformat.c \
                     src/subsystems/replay/replay_annotations.c \
                     src/repl/scenes.c \
                     src/repl/scene_snapshot.c \
                     src/repl/source_scope.c \
                     src/repl/state.c \
                     src/repl/text_helpers.c \
                     src/repl/time.c \
                     src/repl/tutorials.c \
                     src/repl/visible_vars.c \
                     src/repl/workspace_io.c \
                     tools/repl_demo/source_document.c \
                     tests/gl-stubs/gl_stub_counts.c
# Object list for the standalone editor_demo. It links the generic editor data
# model and edit primitives with the REPL-free ui/core render layer. The demo
# supplies its own input dispatcher and File menu, and deliberately excludes
# the REPL-flavored editor controllers, src/ui/app, and src/app.
EDITOR_DEMO_DEP_SRCS = src/editor/edit_ops.c \
                       src/editor/state.c \
                       src/ui/core/text_layout.c \
                       src/ui/core/text_panel.c \
                       src/ui/core/text_search.c \
                       src/ui/core/theme.c \
				      src/support/cpuprof.c src/support/histogram.c src/support/runstats.c \
                       tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone memprof_demo. It proves the memory-profiling
# subsystem links cleanly from {support, ui/support, ui/core} alone, with no
# src/ui/app, src/app, src/repl, or src/editor. The panel consumes
# UiMemoryPanelView, and check-memprof-demo-isolation.sh enforces this link set.
MEMPROF_DEMO_DEP_SRCS = src/support/memprof.c \
                        src/ui/support/memprof.c \
                        src/ui/core/theme.c \
                        tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone cpuprof_demo. Like memprof_demo, it links
# from {support, ui/support, ui/core} alone. The CPU profile panel consumes
# UiProfilePanelView; support/cpuprof.c is the wall-time sampler and
# support/gpuprof.c is its GL-free GPU twin. The GPU sampler is not initialized
# here, so the panel's GPU column stays "--".
CPUPROF_DEMO_DEP_SRCS = src/support/cpuprof.c src/support/histogram.c src/support/runstats.c \
                        src/support/gpuprof.c \
                        src/ui/support/cpuprof.c \
                        src/ui/core/theme.c \
                        tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone variable_panel_demo. It proves the
# variable-panel subsystem links from {subsystems, ui/subsystems, ui/core}
# alone. The renderer consumes UiVariablePanelView, and drag handlers read
# name and value through an installed VariablePanelValueSource, so neither
# src/repl, src/editor, nor src/ui/app is in the link set. The demo builds the
# view directly and installs its own in-memory value source.
# check-variable-panel-demo-isolation enforces the link set.
VARIABLE_PANEL_DEMO_DEP_SRCS = src/subsystems/variable_panel/variable_panel_state.c \
                               src/subsystems/variable_panel/variable_panel_drag.c \
                               src/ui/subsystems/variable_panel.c \
                               src/ui/core/theme.c \
                               tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone color_picker_demo. It proves the color-picker
# subsystem links from {subsystems, ui/subsystems, ui/core} alone. The peer
# reads the document, writes color edits, and answers geometry through an
# installed ColorPickerHostBridge. The demo therefore needs none of src/repl,
# src/editor, or src/ui/app; check-color-picker-demo-isolation enforces it.
COLOR_PICKER_DEMO_DEP_SRCS = src/subsystems/color_picker/color_picker_state.c \
                             src/ui/subsystems/color_picker.c \
                             src/ui/core/theme.c \
                             tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone assign_plot_demo. It proves the
# assignment-plot subsystem links from {subsystems, ui/support, support,
# ui/core} alone. The peer reads the program it plots as an indexed execution
# trace through an installed AssignPlotHostBridge, so the flat program, GLCmd
# and the CMD_VAR_ASSIGN arg slots stay on the app side of the seam; the demo
# supplies a trace it generated itself. runstats.c is the statistics backing
# (min/max/mean/stddev), and the panel renderer is in ui/support rather than
# ui/subsystems - test_ui_assign_plot links exactly that half already.
# check-assign-plot-demo-isolation enforces the link set.
ASSIGN_PLOT_DEMO_DEP_SRCS = src/subsystems/assign_plot/assign_plot.c \
                            src/ui/support/assign_plot.c \
                            src/support/runstats.c \
                            src/ui/core/theme.c \
                            tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone repl_live_demo. The *composition* counterpart
# to repl_demo: where repl_demo proves the REPL pipeline links with no editor /
# controller / UI, repl_live_demo proves the REPL pipeline and the variable-panel
# peer wire together under a one-file host controller - an external editor (vim)
# owns the scene .c files, the demo watches their mtime, re-imports on save, and
# surfaces predefined vars in the floating slider panel. It is the full
# REPL_DEMO_DEP_SRCS set (pipeline + tools/repl_demo/source_document.c static
# backend + gl_stub_counts + cpuprof) plus the four variable-panel TUs. Still no
# src/editor, src/app, src/render3d, or src/ui/app - check-repl-live-demo-no-editor
# enforces the editor exclusion.
REPL_LIVE_DEMO_DEP_SRCS = $(REPL_DEMO_DEP_SRCS) \
                          src/subsystems/variable_panel/variable_panel_state.c \
                          src/subsystems/variable_panel/variable_panel_drag.c \
                          src/ui/subsystems/variable_panel.c \
                          src/ui/core/theme.c

# The -fgvendor suffix keeps the opt-in Linux vendored build (FREEGLUT_VENDOR_LINUX=1)
# from sharing objects with the default system-glut build: the two compile against
# different <GL/freeglut.h> headers (FREEGLUT_HEADER_CFLAGS differs), so a shared
# OBJDIR would reuse objects built against the other one. Only meaningful on the
# native windowed Linux path -- the flag is inert under OSMesa/web/stubs, and those
# arms already have their own suffix, so it is not applied there.
#
# -fgext does the same job for FREEGLUT_LIB_PATH: an external freeglut usually
# comes with its own headers (FREEGLUT_INCLUDE_DIR), and those objects must not
# be reused by - or reuse - a vendored-header build. It applies on every arm,
# since the header swap is platform-independent.
OBJDIR = build/$(BUILD)$(if $(filter debug,$(BUILD)),$(DEBUG_SAN_SUFFIX),)$(if $(filter 1,$(USE_GL_STUBS)),-gl-stubs,)$(if $(filter 1,$(FREEGLUT_OSMESA)),-osmesa,)$(if $(filter 0,$(FREEGLUT_VENDOR)),-glut,)$(if $(filter 1,$(WEB)),-web,)$(if $(filter 1,$(USE_GL_STUBS))$(filter 1,$(FREEGLUT_OSMESA))$(filter 1,$(WEB))$(filter Darwin,$(UNAME_S)),,$(if $(filter 1,$(FREEGLUT_VENDOR_LINUX)),-fgvendor,))$(if $(FREEGLUT_LIB_PATH),-fgext,)
BINDIR = $(OBJDIR)
# Each Make process gets its own report directory. Recursive test/build
# invocations therefore print and summarize only their own source files.
COMPILE_REPORT_DIR := build/.compile-report-$(shell date +%s)-$(shell printf '%s' "$$PPID")-$(MAKELEVEL)
COMPILE_REPORT_START := $(COMPILE_REPORT_DIR)/start
COMPILE_REPORT_VERBOSE = $(if $(filter 1 yes true,$(V) $(VERBOSE)),1,0)
# Fixed web-build bindir, independent of the ambient WEB flag -- lets
# `make web-serve` find the output without the caller having to repeat
# WEB=1 (mirrors how `make web` itself re-invokes with WEB=1 internally).
WEB_BINDIR = build/$(BUILD)-web
# Shared music source: prefer assets/favorite when it holds any tracks, else
# the flat assets/ dir. Used by the web build below and the release packaging
# (see the release targets). Override with MUSIC_SRC_DIR=<dir> on any target.
FAVORITE_MP3S := $(wildcard assets/favorite/*.mp3)
MUSIC_SRC_DIR ?= $(if $(FAVORITE_MP3S),assets/favorite,assets)

# Copy browser music as ordinary static files instead of putting it into an
# Emscripten .data preload bundle. The web audio backend streams these URLs via
# the browser's media stack, so startup no longer waits for the playlist.
WEB_MUSIC_SRC_DIR ?= $(MUSIC_SRC_DIR)
# Reaches every compile: the pattern rules at $(OBJDIR)/%.o and
# $(HOT_OBJDIR)/%.o are the only places a .c is compiled.
#
# **CFLAGS is the documented hook** for the compile-time knobs
# (UI_THEME_DEFAULT, GLR_AUDIO_NO_THREAD, ...) - this is a C project, so
# `make gl-repl CFLAGS=-DUI_THEME_DEFAULT=1` is what help and the docs
# advertise and what a reader will not misread as a C++ setting.
#
# CPPFLAGS is honored but deliberately not advertised: it is the GNU-standard
# home for preprocessor flags, so a toolchain that injects it through the
# environment (emmake forwards it into the web build) still gets its defines
# through. It used to be advertised while nothing referenced it, which meant
# `make gl-repl CPPFLAGS=-DFOO` silently dropped the define and built the
# default. Both append in order, so an explicit CFLAGS wins on a conflict.
OBJ_CFLAGS = $(BUILD_CFLAGS) $(CPPFLAGS) $(CFLAGS) -include config.h -include prof_sections.h
DEPFLAGS = -MMD -MP

SAMPLE_OBJS = $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
SUPPORT_OBJS          = $(addprefix $(OBJDIR)/,$(SUPPORT_SRCS:.c=.o))
APP_CONTROLLER_OBJS   = $(addprefix $(OBJDIR)/,$(APP_CONTROLLER_SRCS:.c=.o))
EDITOR_OBJS           = $(addprefix $(OBJDIR)/,$(EDITOR_SRCS:.c=.o))
REPL_OBJS             = $(addprefix $(OBJDIR)/,$(REPL_SRCS:.c=.o))
RENDER3D_OBJS            = $(addprefix $(OBJDIR)/,$(RENDER3D_SRCS:.c=.o))
UI_CORE_OBJS          = $(addprefix $(OBJDIR)/,$(UI_CORE_SRCS:.c=.o))
UI_APP_OBJS           = $(addprefix $(OBJDIR)/,$(UI_APP_SRCS:.c=.o))
UI_SUPPORT_OBJS       = $(addprefix $(OBJDIR)/,$(UI_SUPPORT_SRCS:.c=.o))
UI_SUBSYSTEMS_OBJS    = $(addprefix $(OBJDIR)/,$(UI_SUBSYSTEMS_SRCS:.c=.o))
SUBSYSTEMS_OBJS       = $(addprefix $(OBJDIR)/,$(SUBSYSTEMS_SRCS:.c=.o))
GL_STUB_COUNTS_OBJS   = $(addprefix $(OBJDIR)/,$(GL_STUB_COUNTS_SRCS:.c=.o))

CORE_TEST_OBJS = \
	$(SUPPORT_OBJS) \
	$(APP_CONTROLLER_OBJS) \
	$(EDITOR_OBJS) \
	$(REPL_OBJS) \
	$(RENDER3D_OBJS) \
	$(UI_CORE_OBJS) \
	$(UI_APP_OBJS) \
	$(UI_SUPPORT_OBJS) \
	$(UI_SUBSYSTEMS_OBJS) \
	$(SUBSYSTEMS_OBJS) \
	$(GL_STUB_COUNTS_OBJS)

TEST_BINS = \
	test_eval \
	test_format \
	test_mesh_ply \
	test_memprof \
	test_gpuprof \
	test_repl_state \
	test_repl_code_panel_layout \
	test_ui_theme \
	test_render3d_palette \
	test_repl_code_panel_document \
	test_repl_code_panel_syntax \
	test_render3d_transition \
	test_overlay_layout \
	test_ui_scene_tabs \
	test_ui_tabbed_overlay \
	test_scene_file_menu \
	test_repl_core_parse \
	test_repl_core_format \
	test_repl_core_commit \
	test_repl_core_io \
	test_scene_load \
	test_glr_extedit \
	test_repl_export_all_commands \
	test_repl_export_lights \
	test_repl_export_clearcolor \
	test_repl_tune \
	test_camera_header \
	test_camera_apply_modes \
	test_camera_header_parity \
	test_repl_core_examples \
	test_repl_core_search \
	test_repl_core_search_extra \
	test_repl_replace \
	test_editor_completion \
	test_editor_input_selection \
	test_ui_menu_bar \
	test_audio \
	test_repl_core_internal \
	test_repl_autocomplete \
	test_repl_command_store \
	test_repl_var_drag \
	test_render3d_guides \
	test_render3d_render \
	test_depth_viz \
	test_stencil_viz \
	test_repl_editor \
	test_repl_core_extra \
	test_repl_autonormal \
	test_repl_replay \
	test_repl_compile \
	test_repl_flatten_differential \
	test_repl_flatten_deps \
	test_repl_flatten_rebake \
	test_repl_locals \
	test_expr_program \
	test_tutorial_match \
	test_tutorial_runner \
	test_glr_camera \
	test_glr_capture_env \
	test_glr_cli \
	test_glr_init_trace \
	test_glr_tour_snapshot \
	test_glr_tour_transport \
	test_glr_frame_pacer \
	test_splash


TEST_BINS += test_ui
TEST_BINS += test_ui_text_panel
TEST_BINS += test_glr_actions
TEST_BINS += test_glr_ctrl
TEST_BINS += test_repl_executor
# Cross-checks that REPL execution and the exported C produce the same
# gl_stub_counts trace. Needs USE_GL_STUBS=1 on both legs (the REPL side
# to count, and the child binary the test compiles at runtime to count
# the same way). The test shells out to cc using paths relative to CWD;
# `make test` runs from the repo root which is what the test expects.
TEST_BINS += test_export_trace_parity
# Walker invariants the cursor-guide stack relies on. Drives
# replay_walk_user_vertices directly, which calls glPushMatrix /
# glTranslatef, so it needs the no-op GL stubs to run without a
# real GL context.
TEST_BINS += test_replay_walk
TEST_BINS += test_ui_panels
TEST_BINS += test_ui_status_history
TEST_BINS += test_edit_overlays
TEST_BINS += test_ui_memprof
TEST_BINS += test_ui_cpuprof
TEST_BINS += test_hidden_lines
# Stencil legend: the controller's row-selection policy plus the panel's
# pure geometry solve. Links CORE_TEST_OBJS for glr_ctrl's view builder.
TEST_BINS += test_buffer_viz_legend
# Assignment-value plot: the capture engine (core test - it drives the REPL
# pipeline) and the panel renderer (GL stubs, explicit object list below).
TEST_BINS += test_assign_plot
TEST_BINS += test_ui_assign_plot

# `make test-web` runs TEST_BINS as wasm under node. These two do not survive
# that move; the other 75 do. Every exclusion here is a test asserting behavior
# the web build deliberately does not have -- none is a wasm defect. Prefer
# making a test web-aware (a `#if defined(__EMSCRIPTEN__)` arm around the
# affected assertions, as test_ui / test_glr_ctrl / test_ui_scene_tabs /
# test_repl_core_extra now carry) over extending this list.
#
#   test_audio        miniaudio's device backend is native-only; the web build
#                     swaps in an entirely separate HTMLAudioElement
#                     implementation, so the hitch-threshold and device
#                     assertions have no web counterpart at all. Guarding it
#                     out assertion-by-assertion would leave an empty binary.
#   test_ui_menu_bar  25 assertions across ~8 sites drive the File menu that
#                     menu_visible() hides under __EMSCRIPTEN__. Guarding each
#                     is mechanical but would gut the binary's File-menu
#                     coverage for no web gain; a browser lane covers the
#                     shell's replacement chrome instead.
#
# test_edit_overlays was excluded until the vertex-point markers stopped
# forking by target: both now draw the same attenuated GL_POINTS, so its
# world-coordinate trace assertions hold under wasm too.
WEB_TEST_EXCLUDE = \
	test_audio \
	test_ui_menu_bar
WEB_TEST_BINS = $(filter-out $(WEB_TEST_EXCLUDE),$(TEST_BINS))

CORE_TEST_BINS = $(filter-out test_eval test_format test_mesh_ply test_memprof test_gpuprof test_repl_code_panel_layout test_ui_theme test_render3d_palette test_audio test_render3d_guides test_render3d_transition test_render3d_render test_depth_viz test_stencil_viz test_scene_file_menu test_editor_completion test_glr_camera test_glr_init_trace test_ui_cpuprof test_ui_memprof test_ui_text_panel test_tutorial_match test_ui_assign_plot,$(TEST_BINS))

# Benchmark binaries follow the same linking pattern as core test binaries
# (they reuse CORE_TEST_OBJS so they work in both real-GL and stubs builds),
# but they are intentionally NOT in TEST_BINS so `make test` does not run
# them - benchmarks are timing-sensitive and should be invoked explicitly.
BENCH_BINS = bench_repl bench_extedit

ROOT_BIN_LINKS = gl-repl gl-repl-unchained render3d_demo render3d_hot_demo repl_demo repl_live_demo editor_demo memprof_demo variable_panel_demo color_picker_demo assign_plot_demo cpuprof_demo render3d-asset-builder

HEADLESS_DEMO_TARGETS = \
	render3d-demo \
	repl-demo \
	repl-live-demo \
	editor-demo \
	memprof-demo \
	cpuprof-demo \
	variable-panel-demo \
	color-picker-demo \
	assign-plot-demo

DEMO_TARGETS = $(HEADLESS_DEMO_TARGETS) render3d-hot

SAMPLE_BIN = $(BINDIR)/gl-repl
ifeq ($(WEB),1)
SAMPLE_BIN = $(BINDIR)/index.html
endif
RENDER3D_DEMO_BIN = $(BINDIR)/render3d_demo
REPL_DEMO_BIN = $(BINDIR)/repl_demo
REPL_LIVE_DEMO_BIN = $(BINDIR)/repl_live_demo
EDITOR_DEMO_BIN = $(BINDIR)/editor_demo
MEMPROF_DEMO_BIN = $(BINDIR)/memprof_demo
CPUPROF_DEMO_BIN = $(BINDIR)/cpuprof_demo
VARIABLE_PANEL_DEMO_BIN = $(BINDIR)/variable_panel_demo
COLOR_PICKER_DEMO_BIN = $(BINDIR)/color_picker_demo
ASSIGN_PLOT_DEMO_BIN = $(BINDIR)/assign_plot_demo

define core_test_binary
$(1)_OBJS = $$(OBJDIR)/$$(TEST_DIR)/$(1).o $$(CORE_TEST_OBJS)
$(1)_LDLIBS = $$(GL_LDFLAGS)
$(1)_RUN ?= $$(BINDIR)/$(1)
endef

define bench_binary
$(1)_OBJS = $$(OBJDIR)/$$(BENCH_DIR)/$(1).o $$(CORE_TEST_OBJS)
$(1)_LDLIBS = $$(GL_LDFLAGS)
$(1)_RUN ?= $$(BINDIR)/$(1)
endef

$(foreach test,$(CORE_TEST_BINS),$(eval $(call core_test_binary,$(test))))
$(foreach bin,$(BENCH_BINS),$(eval $(call bench_binary,$(bin))))

# WEB=1: benchmarks link the wasm GL stack without the app-page glue, so they
# can run headless under node. See BENCH_WEB_LDFLAGS and `make bench-web`.
ifeq ($(WEB),1)
$(foreach bin,$(BENCH_BINS),$(eval $(bin)_LDLIBS = $$(BENCH_WEB_LDFLAGS)))
endif

test_eval_OBJS = $(OBJDIR)/$(TEST_DIR)/test_eval.o $(OBJDIR)/src/repl/eval.o
test_eval_LDLIBS = -lm -lpthread
test_eval_RUN = $(BINDIR)/test_eval --run-tests

test_format_OBJS = $(OBJDIR)/$(TEST_DIR)/test_format.o $(OBJDIR)/src/repl/format.o
test_format_LDLIBS = -lm
test_format_RUN ?= $(BINDIR)/test_format

test_mesh_ply_OBJS = $(OBJDIR)/$(TEST_DIR)/test_mesh_ply.o $(OBJDIR)/src/support/mesh_ply.o
test_mesh_ply_LDLIBS = -lm
test_mesh_ply_RUN ?= $(BINDIR)/test_mesh_ply

test_memprof_OBJS = $(OBJDIR)/$(TEST_DIR)/test_memprof.o $(OBJDIR)/src/support/memprof.o
test_memprof_LDLIBS = -lm
test_memprof_RUN ?= $(BINDIR)/test_memprof

# Include-as-unit (the test #includes support/gpuprof.c with a tiny segment
# cap to reach the overflow path and the internal counters), so gpuprof.o
# itself must not be linked. GL-free: queries are scripted fakes.
test_gpuprof_OBJS = $(OBJDIR)/$(TEST_DIR)/test_gpuprof.o
test_gpuprof_LDLIBS = -lm
test_gpuprof_RUN ?= $(BINDIR)/test_gpuprof

# Exercise gpuprof's multiword ProfSectionSet with a synthetic 67-section
# catalog. The production catalog remains the force-include everywhere else.
$(OBJDIR)/$(TEST_DIR)/test_gpuprof.o: OBJ_CFLAGS := \
	$(subst -include prof_sections.h,-include tests/support/prof_sections_wide.h,$(OBJ_CFLAGS))

test_repl_code_panel_layout_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_code_panel_layout.o $(OBJDIR)/src/ui/core/text_layout.o
test_repl_code_panel_layout_LDLIBS =
test_repl_code_panel_layout_RUN ?= $(BINDIR)/test_repl_code_panel_layout

# Now needs theme.o to resolve externs.
test_ui_theme_OBJS = $(OBJDIR)/$(TEST_DIR)/test_ui_theme.o $(OBJDIR)/src/ui/core/theme.o
test_ui_theme_LDLIBS =
test_ui_theme_RUN ?= $(BINDIR)/test_ui_theme

# Header-only: src/render3d/palette.h pulls in no project objects.
test_render3d_palette_OBJS = $(OBJDIR)/$(TEST_DIR)/test_render3d_palette.o
test_render3d_palette_LDLIBS =
test_render3d_palette_RUN ?= $(BINDIR)/test_render3d_palette

test_audio_OBJS = $(OBJDIR)/$(TEST_DIR)/test_audio.o $(OBJDIR)/src/app/glr_audio.o $(OBJDIR)/src/app/glr_paths.o
test_audio_LDLIBS = $(GL_LDFLAGS)
test_audio_RUN ?= $(BINDIR)/test_audio

# The init trace is self-contained (stdio + gettimeofday), so it links alone
# rather than dragging in CORE_TEST_OBJS.
test_glr_init_trace_OBJS = $(OBJDIR)/$(TEST_DIR)/test_glr_init_trace.o $(OBJDIR)/src/app/boot/glr_init_trace.o
test_glr_init_trace_LDLIBS = $(GL_LDFLAGS)
test_glr_init_trace_RUN ?= $(BINDIR)/test_glr_init_trace

# Pure timer-deadline calculation; keep this focused unit test independent
# from the controller and rendering stacks.
test_glr_frame_pacer_OBJS = $(OBJDIR)/$(TEST_DIR)/test_glr_frame_pacer.o \
	$(OBJDIR)/src/app/boot/glr_frame_pacer.o
test_glr_frame_pacer_LDLIBS = -lm
test_glr_frame_pacer_RUN ?= $(BINDIR)/test_glr_frame_pacer

# Splash rendering relies only on its theme table and the GL-stub counters.
test_splash_OBJS = $(OBJDIR)/$(TEST_DIR)/test_splash.o \
	$(OBJDIR)/src/app/boot/splash.o \
	$(OBJDIR)/src/ui/core/theme.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_splash_LDLIBS = -lm
test_splash_RUN ?= $(BINDIR)/test_splash

test_render3d_guides_OBJS = $(OBJDIR)/$(TEST_DIR)/test_render3d_guides.o \
	$(OBJDIR)/src/render3d/overlays.o \
	$(OBJDIR)/src/render3d/guides/geometry_guides.o \
	$(OBJDIR)/src/render3d/guides/transform_guides.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_render3d_guides_LDLIBS = $(GL_LDFLAGS)
test_render3d_guides_RUN ?= $(BINDIR)/test_render3d_guides

test_render3d_transition_OBJS = $(OBJDIR)/$(TEST_DIR)/test_render3d_transition.o \
	$(OBJDIR)/src/render3d/render3d_transition.o
test_render3d_transition_LDLIBS =
test_render3d_transition_RUN ?= $(BINDIR)/test_render3d_transition

test_render3d_render_OBJS = $(OBJDIR)/$(TEST_DIR)/test_render3d_render.o \
	$(RENDER3D_OBJS) \
	$(OBJDIR)/src/support/cpuprof.o \
	$(OBJDIR)/src/support/histogram.o \
	$(OBJDIR)/src/support/runstats.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_render3d_render_LDLIBS = $(GL_LDFLAGS)
test_render3d_render_RUN ?= $(BINDIR)/test_render3d_render

# Synthetic-buffer tests for the pure conversion cores
# (buffer_viz_depth_map, buffer_viz_stencil_scan/_map). The viz .o files
# carry GL calls for their capture/render shells, hence GL_LDFLAGS on
# real-GL builds (no-op inline stubs under USE_GL_STUBS=1, like
# test_render3d_guides); cpuprof.o because the shells bracket themselves
# with prof_begin/prof_accum_end now that render3d's neutral buffer hooks
# name no viz section. Both binaries link the whole subsystem -
# buffer_viz.o owns the shared range smoothing, and its hook fan-out
# references both viz modules.
BUFFER_VIZ_TEST_OBJS = \
	$(OBJDIR)/src/subsystems/buffer_viz/buffer_viz.o \
	$(OBJDIR)/src/subsystems/buffer_viz/depth_viz.o \
	$(OBJDIR)/src/subsystems/buffer_viz/stencil_viz.o \
	$(OBJDIR)/src/render3d/postprocess_filter.o \
	$(OBJDIR)/src/render3d/postprocess_surface.o \
	$(OBJDIR)/src/support/cpuprof.o \
	$(OBJDIR)/src/support/histogram.o \
	$(OBJDIR)/src/support/runstats.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o

test_depth_viz_OBJS = $(OBJDIR)/$(TEST_DIR)/test_depth_viz.o \
	$(BUFFER_VIZ_TEST_OBJS)
test_depth_viz_LDLIBS = $(GL_LDFLAGS)
test_depth_viz_RUN ?= $(BINDIR)/test_depth_viz

test_stencil_viz_OBJS = $(OBJDIR)/$(TEST_DIR)/test_stencil_viz.o \
	$(BUFFER_VIZ_TEST_OBJS)
test_stencil_viz_LDLIBS = $(GL_LDFLAGS)
test_stencil_viz_RUN ?= $(BINDIR)/test_stencil_viz

test_scene_file_menu_OBJS = $(OBJDIR)/$(TEST_DIR)/test_scene_file_menu.o $(CORE_TEST_OBJS)
test_scene_file_menu_LDLIBS = $(GL_LDFLAGS)
test_scene_file_menu_RUN ?= $(BINDIR)/test_scene_file_menu

test_editor_completion_OBJS = $(OBJDIR)/$(TEST_DIR)/test_editor_completion.o \
	$(OBJDIR)/src/editor/completion.o \
	$(OBJDIR)/src/editor/state.o
test_editor_completion_LDLIBS = $(GL_LDFLAGS)
test_editor_completion_RUN ?= $(BINDIR)/test_editor_completion

test_glr_camera_OBJS = $(OBJDIR)/$(TEST_DIR)/test_glr_camera.o \
	$(OBJDIR)/src/app/glr_camera.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_glr_camera_LDLIBS = $(GL_LDFLAGS)
test_glr_camera_RUN ?= $(BINDIR)/test_glr_camera

test_ui_cpuprof_OBJS = $(OBJDIR)/$(TEST_DIR)/test_ui_cpuprof.o \
	$(OBJDIR)/src/app/glr_prof.o \
	$(OBJDIR)/src/support/cpuprof.o \
	$(OBJDIR)/src/support/histogram.o \
	$(OBJDIR)/src/support/runstats.o \
	$(OBJDIR)/src/support/gpuprof.o \
	$(OBJDIR)/src/ui/support/cpuprof.o \
	$(OBJDIR)/src/ui/core/theme.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_ui_cpuprof_LDLIBS = $(GL_LDFLAGS)
test_ui_cpuprof_RUN ?= $(BINDIR)/test_ui_cpuprof

# No assign_plot.o here on purpose: the panel renderer consumes AssignPlotView
# as data and calls nothing on the capture subsystem, so the test builds views
# by hand. If this list ever needs the subsystem object, the renderer has
# grown a dependency it should not have.
test_ui_assign_plot_OBJS = $(OBJDIR)/$(TEST_DIR)/test_ui_assign_plot.o \
	$(OBJDIR)/src/support/runstats.o \
	$(OBJDIR)/src/ui/support/assign_plot.o \
	$(OBJDIR)/src/ui/core/theme.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_ui_assign_plot_LDLIBS = $(GL_LDFLAGS)
test_ui_assign_plot_RUN ?= $(BINDIR)/test_ui_assign_plot

test_ui_memprof_OBJS = $(OBJDIR)/$(TEST_DIR)/test_ui_memprof.o \
	$(OBJDIR)/src/support/memprof.o \
	$(OBJDIR)/src/ui/support/memprof.o \
	$(OBJDIR)/src/ui/core/theme.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_ui_memprof_LDLIBS = $(GL_LDFLAGS)
test_ui_memprof_RUN ?= $(BINDIR)/test_ui_memprof

test_ui_text_panel_OBJS = $(OBJDIR)/$(TEST_DIR)/test_ui_text_panel.o \
	$(OBJDIR)/src/ui/core/text_layout.o \
	$(OBJDIR)/src/ui/core/text_panel.o \
	$(OBJDIR)/src/ui/core/text_search.o \
	$(OBJDIR)/src/ui/core/theme.o \
	$(OBJDIR)/src/support/cpuprof.o \
	$(OBJDIR)/src/support/histogram.o \
	$(OBJDIR)/src/support/runstats.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_ui_text_panel_LDLIBS = $(GL_LDFLAGS)
test_ui_text_panel_RUN ?= $(BINDIR)/test_ui_text_panel

test_tutorial_match_OBJS = $(OBJDIR)/$(TEST_DIR)/test_tutorial_match.o \
	$(OBJDIR)/src/subsystems/tutorial/tutorial_match.o
test_tutorial_match_LDLIBS = $(GL_LDFLAGS)
test_tutorial_match_RUN ?= $(BINDIR)/test_tutorial_match
# For tests using the "include-as-unit" pattern (e.g., `#include "file.c"` to test
# internal static functions), we must filter out the original object file from
# the CORE_TEST_OBJS link list to prevent duplicate symbol errors.
test_glr_ctrl_OBJS = $(OBJDIR)/$(TEST_DIR)/test_glr_ctrl.o $(filter-out $(OBJDIR)/src/app/glr_ctrl.o $(OBJDIR)/src/app/glr_ctrl_router.o,$(CORE_TEST_OBJS))

test_repl_executor_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_executor.o $(filter-out $(OBJDIR)/src/repl/executor.o,$(CORE_TEST_OBJS))

# The export test includes export_cmd_writer.c as a unit so it can drive the
# static per-command writer with one probe for every CmdType. Keep the normal
# object out of this link or every writer symbol would be defined twice.
test_repl_export_all_commands_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_export_all_commands.o \
	$(filter-out $(OBJDIR)/src/repl/export_cmd_writer.o,$(CORE_TEST_OBJS))

test_repl_replay_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_replay.o $(filter-out $(OBJDIR)/src/subsystems/replay/replay.o,$(CORE_TEST_OBJS))

# test_replay_walk includes app/glr_ctrl.c as a translation unit to reach
# the static cursor_guide_snapshot_with_flat_args helper, so the object
# must be filtered out the same way test_glr_ctrl does.
test_replay_walk_OBJS = $(OBJDIR)/$(TEST_DIR)/test_replay_walk.o $(filter-out $(OBJDIR)/src/app/glr_ctrl.o,$(CORE_TEST_OBJS))

test_edit_overlays_OBJS = $(OBJDIR)/$(TEST_DIR)/test_edit_overlays.o $(filter-out $(OBJDIR)/src/subsystems/edit_overlays/edit_overlays.o,$(CORE_TEST_OBJS))

TEST_OBJS = $(foreach test,$(TEST_BINS),$($(test)_OBJS))

# Longest-processing-time-first (LPT) scheduling for the parallel runner. The
# test-binary durations are heavily skewed (a handful of multi-second binaries,
# then a cliff to <0.5s), so with a small pool a long binary discovered late
# leaves one worker finishing it alone. Dispatching the slowest
# binaries first (they occupy the pool from t=0 while the many tiny ones
# backfill) cuts tail latency / makespan. Durations profiled via
# `make test-stubs TEST_JOBS=3` (fewer jobs than performance cores). Only the
# run ORDER changes; $(TEST_BINS) (build set) is untouched, and $(filter ...)
# keeps stub-only entries out of the ordering when USE_GL_STUBS is off.
TEST_SLOW_FIRST = \
	test_ui_menu_bar \
	test_repl_core_examples \
	test_audio \
	test_export_trace_parity \
	test_glr_ctrl \
	test_repl_flatten_differential
TEST_BINS_ORDERED = \
	$(foreach test,$(TEST_SLOW_FIRST),$(filter $(test),$(TEST_BINS))) \
	$(filter-out $(TEST_SLOW_FIRST),$(TEST_BINS))
TEST_RUNNER_CASES = $(foreach test,$(TEST_BINS_ORDERED),'$(test):::$($(test)_RUN)')

# Same runner, same LPT ordering, with `node ` spliced in front of each case.
# The *_RUN values already carry per-test arguments (test_eval --run-tests), so
# reusing them is what keeps the two lanes from drifting apart.
WEB_TEST_BINS_ORDERED = $(filter $(WEB_TEST_BINS),$(TEST_BINS_ORDERED))
WEB_TEST_RUNNER_CASES = \
	$(foreach test,$(WEB_TEST_BINS_ORDERED),'$(test):::node $($(test)_RUN)')

TEST_TARGET_NAMES = $(subst _,-,$(TEST_BINS))
RUN_TEST_TARGETS = $(addprefix run-,$(TEST_TARGET_NAMES))
# Keep an alias matching the test binary/source filename for callers that do
# not want the public kebab-case spelling.
RUN_TEST_FILE_TARGETS = $(addprefix run-,$(TEST_BINS))
BENCH_TARGET_NAMES = $(subst _,-,$(BENCH_BINS))
BENCH_OBJS = $(foreach bin,$(BENCH_BINS),$($(bin)_OBJS))

TEST_JOBS ?=
TEST_ARGS ?=

COMPILE_REPORT_TEST_SUMMARY := $(COMPILE_REPORT_DIR)/tests-summary
$(COMPILE_REPORT_TEST_SUMMARY): $(addprefix $(BINDIR)/,$(TEST_BINS))
	@bash scripts/compile-report.sh summary "$(COMPILE_REPORT_DIR)"
	@touch $@

COMPILE_REPORT_BENCH_SUMMARY := $(COMPILE_REPORT_DIR)/bench-summary
$(COMPILE_REPORT_BENCH_SUMMARY): $(addprefix $(BINDIR)/,$(BENCH_BINS))
	@bash scripts/compile-report.sh summary "$(COMPILE_REPORT_DIR)"
	@touch $@

COMPILE_REPORT_WEB_TEST_SUMMARY := $(COMPILE_REPORT_DIR)/web-tests-summary
$(COMPILE_REPORT_WEB_TEST_SUMMARY): $(addprefix $(BINDIR)/,$(WEB_TEST_BINS))
	@bash scripts/compile-report.sh summary "$(COMPILE_REPORT_DIR)"
	@touch $@

ALL_OBJS = $(sort $(SAMPLE_OBJS) $(TEST_OBJS) $(BENCH_OBJS))

DEPS = $(ALL_OBJS:.o=.d)

$(GENERATED_EXAMPLES_INC): FORCE scripts/gen_examples.py $(EXAMPLES_CATALOG) $(EXAMPLE_SCENE_SRCS)
	@mkdir -p $(dir $@)
	python3 scripts/gen_examples.py --catalog $(EXAMPLES_CATALOG) --out $@

$(OBJDIR)/src/repl/examples.o: $(GENERATED_EXAMPLES_INC)

$(GENERATED_TOURS_INC): FORCE scripts/gen_tours.py $(TOURS_CATALOG) $(TOUR_SCRIPT_SRCS)
	@mkdir -p $(dir $@)
	python3 scripts/gen_tours.py --catalog $(TOURS_CATALOG) --out $@

$(OBJDIR)/src/app/glr_tours.o: $(GENERATED_TOURS_INC)

$(GENERATED_COMMAND_DESCRIPTIONS_INC): FORCE scripts/gen_command_descriptions.py \
		$(COMMAND_DESCRIPTIONS_SOURCE) src/repl/command.h src/repl/command_spec.c
	@mkdir -p $(dir $@)
	python3 scripts/gen_command_descriptions.py \
		--catalog $(COMMAND_DESCRIPTIONS_SOURCE) --out $@

$(OBJDIR)/src/repl/command_descriptions.o: $(GENERATED_COMMAND_DESCRIPTIONS_INC)

# One startup line replaces the repeated build-mode flags that used to appear
# on every compiler command. The small shell loop preserves first-seen order
# while removing duplicate flags.
$(COMPILE_REPORT_START):
	@mkdir -p $(COMPILE_REPORT_DIR)
	@{ \
		printf '\n  $(ESC)[1;36m%s$(NC)\n' '$(BUILD_REPORT_PARAMS)'; \
		printf '  $(YELLOW)flags:$(NC)'; \
		seen=''; \
		for flag in $(BUILD_REPORT_FLAGS); do \
			case " $$seen " in \
				*" $$flag "*) ;; \
				*) printf ' %s' "$$flag"; seen="$$seen $$flag" ;; \
			esac; \
		done; \
		printf '\n\n'; \
	}
	@touch $@

$(OBJDIR)/%.o: %.c | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh compile "$(COMPILE_REPORT_DIR)" "$<" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(DEPFLAGS) -c -o $@ $<

# Vendored freeglut static library. Built once via CMake into $(FREEGLUT_BUILD),
# which lives under third_party/ so the top-level `make clean` (rm -rf ./build)
# leaves it intact. The only source prerequisite is VENDORED.txt, which the
# vendor script rewrites on every re-vendor (nothing else touches it) -- so a
# re-vendor marks the archive stale and the next build reconfigures + rebuilds
# it automatically. `make freeglut-clean` still forces a full from-scratch
# rebuild (e.g. after a hand-edit, or to switch backends cleanly). The backend
# (Cocoa by default, OSMesa under FREEGLUT_OSMESA=1) and build dir/lib name are
# selected by FREEGLUT_CMAKE_BACKEND / FREEGLUT_BUILD up top. The OSMesa backend
# resolves libOSMesa via pkg-config; point it at Homebrew's mesa .pc files.
# On macOS, OSMesa's pkg-config (osmesa.pc) lives under Homebrew's mesa prefix;
# on Linux libosmesa6-dev puts it on the default pkg-config path, so this stays
# empty there.
FREEGLUT_PKG_CONFIG_PATH := $(if $(filter 1,$(FREEGLUT_OSMESA)),$(if $(filter Darwin,$(UNAME_S)),$(shell brew --prefix mesa 2>/dev/null)/lib/pkgconfig),)
#
# Under FREEGLUT_LIB_PATH the archive is somebody else's build product: make
# never creates it, so the rule degrades to an existence check with a useful
# message (an unbuildable prerequisite would otherwise fail as "no rule to make
# target"). A rule with no prerequisites does not re-run for a file that exists.
ifneq ($(FREEGLUT_LIB_PATH),)
$(FREEGLUT_STATIC_LIB):
	@printf 'error: FREEGLUT_LIB_PATH=%s does not exist\n' '$(FREEGLUT_LIB_PATH)' >&2; \
	 printf '       build that freeglut first, or unset FREEGLUT_LIB_PATH to use the vendored one.\n' >&2; \
	 exit 1
else
$(FREEGLUT_STATIC_LIB): $(FREEGLUT_SRC)/VENDORED.txt
	PKG_CONFIG_PATH="$(FREEGLUT_PKG_CONFIG_PATH):$$PKG_CONFIG_PATH" \
	$(FREEGLUT_CMAKE_LAUNCHER) cmake -S $(FREEGLUT_SRC) -B $(FREEGLUT_BUILD) \
	  $(FREEGLUT_CMAKE_BACKEND) -DFREEGLUT_BUILD_STATIC_LIBS=ON \
	  -DFREEGLUT_BUILD_SHARED_LIBS=OFF -DFREEGLUT_BUILD_DEMOS=OFF \
	  -DCMAKE_BUILD_TYPE=Release
	cmake --build $(FREEGLUT_BUILD) --target freeglut_static
endif

freeglut-clean: ## Remove the vendored freeglut CMake build (forces a rebuild).
	rm -rf $(FREEGLUT_BUILD) $(FREEGLUT_SRC)/build-osmesa-shared

# WEB=1: shell.html, gl4es_bootstrap.c, and the static archives are
# link-time inputs (not objects in $(SAMPLE_OBJS) -- see GL_LDFLAGS above),
# so add them as prerequisites here or editing/rebuilding any of them
# (e.g. a repatched gl4es) would silently not trigger a relink.
ifeq ($(WEB),1)
$(SAMPLE_BIN): packaging/web/shell.html packaging/web/gl4es_bootstrap.c \
	$(GL4ES_DIR)/lib/libGL.a $(GLU_DIR)/.libs/libGLU.a $(FREEGLUT_STATIC_LIB)
endif

# EXTRA_LDFLAGS is an append-only hook for build variants that need a link
# flag the platform block does not supply (currently only gl-repl-unchained's
# larger main-thread stack). Overriding GL_LDFLAGS from a sub-make would
# replace the whole per-platform list instead of adding to it.
EXTRA_LDFLAGS ?=

$(SAMPLE_BIN): $(SAMPLE_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(SAMPLE_OBJS) $(GL_LDFLAGS) $(EXTRA_LDFLAGS) -o $@

gl-repl: $(SAMPLE_BIN) ## Build the main gl-repl binary using release flags by default.
	ln -sfn $(SAMPLE_BIN) $@

COMPILE_REPORT_SAMPLE_SUMMARY := $(COMPILE_REPORT_DIR)/sample-summary
$(COMPILE_REPORT_SAMPLE_SUMMARY): $(SAMPLE_BIN)
	@bash scripts/compile-report.sh summary "$(COMPILE_REPORT_DIR)"
	@touch $@
gl-repl: $(COMPILE_REPORT_SAMPLE_SUMMARY)

# gl-repl-unchained -- the same app with the capacity ceilings lifted, for
# EXPERIMENTATION ONLY. It exists because machine-generated documents (a
# glprobe extraction, a mesh conversion) blow straight past limits that were
# sized for hand-typed scenes; it is not a supported configuration and nothing
# in the test suite runs against it.
#
# MAX_EDITOR_COMMANDS is the expensive one: ~33 KB per unit, because every
# source command is duplicated across the 32x2 undo/redo rings and 8 user-scene
# slots (`make capacity-matrix` prints the fan-out). That memory is real and
# resident, not lazily-faulted BSS -- 16x measured 556 MB -- so the ring depth
# is cut to 8 to pay for it. Shorter undo history is the trade that keeps a
# 32x document cap from costing over a gigabyte.
#
# The flatten visit budget is raised alongside, or the target would just swap
# one ceiling for another on the first large document.
#
# And the main thread needs a bigger stack. Document-sized snapshots are taken
# as ordinary locals all over the controller and editor -- 267 KB each at the
# default cap, which is unremarkable, but 8.5 MB at 32x. glr_ctrl_display_frame
# alone overflows the 8 MB macOS main stack on the FIRST frame. Raising the
# stack is the one-flag fix; moving ~20 functions' snapshots off the stack is
# the real one, and is not something an experimental target should force on the
# shipping build. On Linux the main stack comes from the shell, so raise it
# with `ulimit -s` there instead.
# Linux gets no flag, and that is not an oversight: the main-thread stack there
# comes from RLIMIT_STACK, not the executable. GNU ld accepts -z stacksize and
# then says so -- "warning: -z stacksize=... ignored" -- and the binary still
# dies at the same 8 MB (Ubuntu 24.04's default `ulimit -s` is 8192 KB, same as
# macOS). Only the shell can raise it, so the target says so after linking.
ifeq ($(UNAME_S),Darwin)
  GLR_UNCHAINED_LDFLAGS = -Wl,-stack_size,0x8000000
else
  GLR_UNCHAINED_LDFLAGS =
endif
GLR_UNCHAINED_DIR = build/unchained$(if $(filter 1,$(FREEGLUT_OSMESA)),-osmesa,)
GLR_UNCHAINED_BIN = $(GLR_UNCHAINED_DIR)/gl-repl-unchained

.PHONY: gl-repl-unchained
gl-repl-unchained: ## Build an experimental gl-repl with 32x source-command and 8x flat-command capacity, 8-deep undo (not tested, not supported).
	$(MAKE) BUILD=unchained \
		SAMPLE_BIN=$(GLR_UNCHAINED_BIN) \
		CFLAGS="$(CFLAGS) -DMAX_EDITOR_COMMANDS=65536 \
			-DMAX_FLAT_COMMANDS=65536 \
			-DMAX_FLATTEN_VISIT_BUDGET=2000000 \
			-DREPL_UNDO_DEPTH=8" \
		EXTRA_LDFLAGS="$(GLR_UNCHAINED_LDFLAGS)" \
		$(GLR_UNCHAINED_BIN)
	ln -sfn $(GLR_UNCHAINED_BIN) $@
ifneq ($(UNAME_S),Darwin)
	@echo "$(YELLOW)note:$(NC) raise the stack before running, or the first frame segfaults:"
	@echo "  $(CYAN)ulimit -s 131072 && ./$@ scene.glr$(NC)"
endif

render3d-asset-builder: ## Build separate render3d-asset-builder binary with high flat-command capacity and asset catalog.
	$(MAKE) BUILD=render3d_asset_builder EXAMPLES_CATALOG=tools/render3d_asset_builder/catalog.ini SAMPLE_BIN=build/render3d_asset_builder/render3d-asset-builder CFLAGS="$(CFLAGS) -DMAX_FLAT_COMMANDS=32768" build/render3d_asset_builder/render3d-asset-builder
	ln -sfn build/render3d_asset_builder/render3d-asset-builder $@

# Shared by `web` and `bench-web`. A prerequisite rather than a copy of the
# check in each recipe -- the advice is long enough that two copies would
# drift apart.
require-emcc:
	@command -v emcc >/dev/null 2>&1 || { \
		echo "ERROR: emcc not found on PATH."; \
		echo ""; \
		echo "Either:"; \
		echo "  1. Run 'source <your-emsdk-checkout>/emsdk_env.sh' (as 'source',"; \
		echo "     not by executing the file) in THIS shell, then re-run"; \
		echo "     'make web'. scripts/build-web.sh only sources emsdk inside"; \
		echo "     its own subprocess, so running it does not put emcc on"; \
		echo "     PATH for your shell's later 'make web' calls."; \
		echo "  2. Or just run scripts/build-web.sh instead of 'make web' --"; \
		echo "     it sources emsdk and builds in one shot, every time."; \
		echo "     Defaults to ~/src/emsdk; override with EMSDK=<path>."; \
		exit 1; \
	}

web: require-emcc ## Build the Emscripten/wasm web target (needs emcc on PATH -- see scripts/build-web.sh for a cold start).
	scripts/web-deps.sh
	$(MAKE) WEB=1 $(WEB_BINDIR)/index.html
	bash scripts/web-audio-assets.sh "$(WEB_MUSIC_SRC_DIR)" "$(WEB_BINDIR)/assets"
	@echo "Built $(WEB_BINDIR)/index.html -- run 'make web-serve' to try it."

web-serve: web ## Serve the built web target over HTTP (builds it first if needed).
	python3 scripts/web-serve.py $(WEB_BINDIR)

# macOS .app bundle so the Dock/Finder show the gl-repl cube icon instead of
# the launching terminal's icon. Pure packaging - no source changes, so the
# -std=c99 / Linux-portable build stays untouched. Needs rsvg-convert
# (brew install librsvg) and the Xcode-shipped iconutil.
MACOS_PKG = packaging/macos
APP_BUNDLE = gl-repl.app
APP_ICNS = $(MACOS_PKG)/gl-repl.icns
APP_ICONSET = $(MACOS_PKG)/gl-repl.iconset
# Icon source SVG. Alternatives in packaging/macos/: gl-repl.svg (flat cube),
# gl-repl-retro-A.svg (chrome synthwave), -B (gold), -C/-C2 (silver OpenGL),
# -D (cube+wordmark), gl-repl-soft-cube.svg (soft pastel perspective cube),
# gl-repl-soft-cube-strong.svg (same, stronger perspective), and
# gl-repl-open-cube.svg (the adopted mark: lit-cube.glr's open cube seen down
# its diagonal, azure/magenta interior; used here).
APP_ICON_SVG = $(MACOS_PKG)/gl-repl-open-cube.svg

# Regenerate the perspective chrome wordmark SVG from its parametric generator.
# The committed .svg is the source of truth; this just makes it reproducible.
# Tune the look by passing args, e.g.: python3 packaging/macos/gen_retro_a.py 430
icon-regen: ## Regenerate gl-repl-retro-A.svg from gen_retro_a.py (then `make app`).
	python3 $(MACOS_PKG)/gen_retro_a.py

# Regenerate the soft pastel perspective-cube SVGs (DIST tunes perspective).
icon-cube: ## Regenerate gl-repl-soft-cube.svg (subtle perspective) then `make app`.
	python3 $(MACOS_PKG)/gen_soft_cube.py 8 gl-repl-soft-cube.svg

icon-cube-strong: ## Regenerate gl-repl-soft-cube-strong.svg (stronger perspective).
	python3 $(MACOS_PKG)/gen_soft_cube.py 5.5 gl-repl-soft-cube-strong.svg

$(APP_ICNS): $(APP_ICON_SVG)
	@command -v rsvg-convert >/dev/null 2>&1 || { echo "need rsvg-convert: brew install librsvg" >&2; exit 1; }
	rm -rf $(APP_ICONSET)
	mkdir -p $(APP_ICONSET)
	rsvg-convert -w 16   -h 16   $< -o $(APP_ICONSET)/icon_16x16.png
	rsvg-convert -w 32   -h 32   $< -o $(APP_ICONSET)/icon_16x16@2x.png
	rsvg-convert -w 32   -h 32   $< -o $(APP_ICONSET)/icon_32x32.png
	rsvg-convert -w 64   -h 64   $< -o $(APP_ICONSET)/icon_32x32@2x.png
	rsvg-convert -w 128  -h 128  $< -o $(APP_ICONSET)/icon_128x128.png
	rsvg-convert -w 256  -h 256  $< -o $(APP_ICONSET)/icon_128x128@2x.png
	rsvg-convert -w 256  -h 256  $< -o $(APP_ICONSET)/icon_256x256.png
	rsvg-convert -w 512  -h 512  $< -o $(APP_ICONSET)/icon_256x256@2x.png
	rsvg-convert -w 512  -h 512  $< -o $(APP_ICONSET)/icon_512x512.png
	rsvg-convert -w 1024 -h 1024 $< -o $(APP_ICONSET)/icon_512x512@2x.png
	iconutil -c icns $(APP_ICONSET) -o $@
	rm -rf $(APP_ICONSET)

app: gl-repl $(APP_ICNS) $(MACOS_PKG)/Info.plist ## Bundle gl-repl into gl-repl.app with the cube icon (macOS).
	rm -rf $(APP_BUNDLE)
	mkdir -p $(APP_BUNDLE)/Contents/MacOS $(APP_BUNDLE)/Contents/Resources
	cp "$$(readlink gl-repl || echo gl-repl)" $(APP_BUNDLE)/Contents/MacOS/gl-repl
	cp $(APP_ICNS) $(APP_BUNDLE)/Contents/Resources/gl-repl.icns
	cp $(MACOS_PKG)/Info.plist $(APP_BUNDLE)/Contents/Info.plist
	# Bundle a sample track so a Finder-launched .app has music (cwd is /
	# there, so the binary finds it via <exe>/../Resources/assets). Users
	# drop more into ~/Library/Application Support/gl-repl/Music.
	mkdir -p $(APP_BUNDLE)/Contents/Resources/assets
	cp assets/sample.mp3 $(APP_BUNDLE)/Contents/Resources/assets/
	# Ad-hoc sign so a downloaded copy gets the ordinary "unidentified
	# developer" Gatekeeper prompt (right-click -> Open) instead of the
	# "damaged" hard block macOS shows for a fully unsigned, quarantined app.
	# No Apple account needed. (The release flow re-signs after it swaps in the
	# full music pack, since editing Resources/ invalidates this signature.)
	# On a mac this is load-bearing, so it hard-fails rather than warning: a
	# silently-unsigned bundle is exactly what ships as "damaged". Off-mac
	# (cross-packaging) there is no codesign, so it degrades to a warning.
	@if [ "$$(uname -s)" = "Darwin" ]; then \
		codesign --force --deep --sign - $(APP_BUNDLE) || exit 1; \
		codesign --verify --deep --strict --verbose=2 $(APP_BUNDLE) || exit 1; \
		echo "ad-hoc signed + verified $(APP_BUNDLE)"; \
	else \
		echo "warning: not macOS - $(APP_BUNDLE) left unsigned"; \
	fi
	touch $(APP_BUNDLE)
	@echo "Built $(APP_BUNDLE) - run: open $(APP_BUNDLE)"

# ---- Release packaging -------------------------------------------------------
# `make release` opens an arrow-key plan menu (per-platform skip/local/remote,
# ssh hosts, repo, music source - persisted to .release.ini), builds the macOS
# .app and the Linux binary accordingly, bundles the music pack into each,
# stages them under dist/<tag>/, then asks for confirmation before uploading to
# the GitHub release. `release-build` stops after staging; `release-upload`
# pushes the already-staged artifacts; `release-config` just edits/saves the
# plan. scripts/release.py owns config precedence (CLI/env > .release.ini >
# defaults); knobs below are forwarded only when set on the command line, so
# `make release LINUX_MODE=local TAG=v1.0.0` works and unset knobs fall through
# to the ini / defaults. See scripts/release.py.
RELEASE_ENV = \
	$(if $(TAG),TAG='$(TAG)') \
	$(if $(REPO),REPO='$(REPO)') \
	$(if $(PIN),PIN='$(PIN)') \
	$(if $(REMOTE_BRANCH),REMOTE_BRANCH='$(REMOTE_BRANCH)') \
	$(if $(NOTES),NOTES='$(NOTES)') \
	$(if $(MACOS_MODE),MACOS_MODE='$(MACOS_MODE)') \
	$(if $(MACOS_HOST),MACOS_HOST='$(MACOS_HOST)') \
	$(if $(MACOS_PATH),MACOS_PATH='$(MACOS_PATH)') \
	$(if $(LINUX_MODE),LINUX_MODE='$(LINUX_MODE)') \
	$(if $(LINUX_HOST),LINUX_HOST='$(LINUX_HOST)') \
	$(if $(LINUX_PATH),LINUX_PATH='$(LINUX_PATH)') \
	$(if $(ASSUME_YES),ASSUME_YES='$(ASSUME_YES)') \
	$(if $(ALLOW_DIRTY),ALLOW_DIRTY='$(ALLOW_DIRTY)')

release: ## Build macOS + Linux release artifacts (plan menu), then confirm before uploading to GitHub.
	@$(RELEASE_ENV) python3 scripts/release.py all

release-build: ## Build + stage release artifacts under dist/<tag>/ without uploading.
	@$(RELEASE_ENV) python3 scripts/release.py build

release-upload: ## Upload the already-staged dist/<tag>/ artifacts to the GitHub release.
	@$(RELEASE_ENV) python3 scripts/release.py upload

release-config: ## Edit + save the release build plan (.release.ini) via the arrow-key menu.
	@$(RELEASE_ENV) python3 scripts/release.py config

# Download the music pack from the GitHub release into the local assets folder
# (MUSIC_DEST, default assets/). Pull from a specific release with MUSIC_TAG=...
# (scripts/fetch-music.sh defaults to the assets-v1 asset release otherwise).
MUSIC_DEST ?= assets
fetch-music: ## Download the music pack from the GitHub release into MUSIC_DEST (default assets/).
	bash scripts/fetch-music.sh --dir "$(MUSIC_DEST)" $(if $(MUSIC_TAG),--tag "$(MUSIC_TAG)",)

# Standalone demo binary that drives the render3d module with a teapot callback.
# Proves the src/render3d/ subtree links cleanly without the editor/UI/controller code.
RENDER3D_DEMO_OBJS = $(OBJDIR)/tools/render3d_demo/render3d_demo.o \
                   $(addprefix $(OBJDIR)/,$(RENDER3D_DEMO_DEP_SRCS:.c=.o))

$(RENDER3D_DEMO_BIN): $(RENDER3D_DEMO_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(RENDER3D_DEMO_OBJS) $(GL_LDFLAGS) -o $@

render3d-demo: $(RENDER3D_DEMO_BIN) ## Build the standalone scene demo.
	ln -sfn $(RENDER3D_DEMO_BIN) render3d_demo

# --- Hot-reloadable render3d demo ------------------------------------------
# `make render3d-hot` builds the same teapot harness, but the reloadable
# src/render3d subtree lives in a shared library that the host dlopen()s
# instead of static-linking. The running host watches src/render3d; on a save
# it rebuilds just that library (make render3d-hot-lib) and re-dlopen()s a
# fresh copy, so grid.c / backdrop.c / lights.c / render.c can be tweaked and
# seen live without relaunching.
#
# State survives the reload because every piece of demo state (camera, view,
# grid theme, lighting, backdrop, projection blend) lives in the host TU
# (render3d_demo.c), which is never reloaded - only the src/render3d .c bodies
# are. The one struct that crosses the boundary, Render3dState, is owned by the
# host too; its layout is fixed by render.h at host-compile time, so changing
# that layout (a header edit) is the one case that needs a relaunch.
#
# The library is deliberately linked WITHOUT freeglut/GL of its own: it
# resolves glut*/gl*/glu* from the host process at load time (macOS: two-level
# `-undefined dynamic_lookup`; Linux: normal shared-object lazy binding against
# the already-loaded libglut/libGL). Linking a second freeglut copy into the
# library would give it an uninitialised fgState and glutSolidSphere() would
# abort "called before glutInit". So the host must carry - and export - every
# freeglut symbol src/render3d might call. On macOS the vendored static
# freeglut is -force_load'ed into the host so ALL of it is present regardless
# of what the host TU itself references (a normal archive pull would only bring
# in the objects the host directly needs, silently breaking a live edit that
# calls a new glut primitive). The plain $(FREEGLUT_LIB) still on GL_LDFLAGS is
# then a no-op. On Linux -lglut is a shared object, so every symbol is already
# available to a dlopen'd module at load - no force-load needed.
hot_comma := ,
ifeq ($(UNAME_S),Darwin)
  HOT_SHARED_LDFLAGS = -dynamiclib -Wl,-undefined,dynamic_lookup
  # Escaped commas: bare commas would be parsed as $(if) argument separators.
  HOT_HOST_LDFLAGS   = $(if $(FREEGLUT_LIB),-Wl$(hot_comma)-force_load$(hot_comma)$(FREEGLUT_LIB))
else
  HOT_SHARED_LDFLAGS = -shared
  # -rdynamic exports the host's symbols so a dlopen'd module can bind to them;
  # -ldl for dlopen/dlsym/dlclose.
  HOT_HOST_LDFLAGS   = -rdynamic -ldl
endif

RENDER3D_HOT_LIB      = $(OBJDIR)/librender3dhot.so
RENDER3D_HOT_DEMO_BIN = $(BINDIR)/render3d_hot_demo
HOT_OBJDIR            = $(OBJDIR)/hot
# Position-independent objects for the reloadable subtree, kept in their own
# dir so the static render3d_demo's objects (same sources, no -fPIC) coexist.
RENDER3D_HOT_OBJS = $(addprefix $(HOT_OBJDIR)/,$(RENDER3D_DEMO_DEP_SRCS:.c=.o))

$(HOT_OBJDIR)/%.o: %.c | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh compile "$(COMPILE_REPORT_DIR)" "$<" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) -fPIC $(DEPFLAGS) -c -o $@ $<

$(RENDER3D_HOT_LIB): $(RENDER3D_HOT_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(HOT_SHARED_LDFLAGS) -fPIC $(RENDER3D_HOT_OBJS) -lm -o $@

render3d-hot-lib: $(RENDER3D_HOT_LIB) ## Rebuild just the reloadable render3d shared library (invoked by the running hot host).

# The host: render3d_demo.c compiled with -DRENDER3D_HOT_RELOAD=1 so its
# render3d_* call sites route through dlsym'd pointers. Absolute paths + an
# explicit rebuild command are baked in so the watcher/rebuild work regardless
# of the cwd the binary is launched from.
RENDER3D_HOT_DEMO_CFLAGS = -DRENDER3D_HOT_RELOAD=1 \
	-DRENDER3D_HOT_LIB_PATH='"$(CURDIR)/$(RENDER3D_HOT_LIB)"' \
	-DRENDER3D_HOT_SRC_DIR='"$(CURDIR)/src/render3d"' \
	-DRENDER3D_HOT_BUILD_CMD='"cd $(CURDIR) && $(MAKE) --no-print-directory render3d-hot-lib BUILD=$(BUILD)"'

$(RENDER3D_HOT_DEMO_BIN): tools/render3d_demo/render3d_demo.c $(RENDER3D_HOT_LIB) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh compile "$(COMPILE_REPORT_DIR)" "$<" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(RENDER3D_HOT_DEMO_CFLAGS) $(DEPFLAGS) $< $(GL_LDFLAGS) $(HOT_HOST_LDFLAGS) -o $@

render3d-hot: $(RENDER3D_HOT_DEMO_BIN) ## Build the hot-reloadable render3d demo (dlopen + live rebuild of src/render3d).
	ln -sfn $(RENDER3D_HOT_DEMO_BIN) render3d_hot_demo

HOT_DEPS = $(RENDER3D_HOT_OBJS:.o=.d) $(RENDER3D_HOT_DEMO_BIN).d
-include $(HOT_DEPS)

# Standalone REPL pipeline demo. Inverse of render3d_demo: proves the
# REPL pipeline links without editor input dispatch / controller / UI.
REPL_DEMO_OBJS = $(OBJDIR)/tools/repl_demo/repl_demo.o \
                 $(OBJDIR)/tools/repl_demo/stubs.o \
                 $(addprefix $(OBJDIR)/,$(REPL_DEMO_DEP_SRCS:.c=.o))

$(REPL_DEMO_BIN): $(REPL_DEMO_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(REPL_DEMO_OBJS) $(GL_LDFLAGS) -o $@

repl-demo: $(REPL_DEMO_BIN) ## Build the standalone REPL pipeline demo.
	ln -sfn $(REPL_DEMO_BIN) repl_demo

# Standalone generic text editor demo. Inverse of repl_demo: proves
# that the editor data model (src/editor/state.c) and the generic
# text-editing primitives (src/editor/edit_ops.c) link cleanly into a
# non-REPL controller using src/ui/core only, with no src/ui/app or
# src/app. The demo's own dispatcher
# (tools/editor_demo/input.c) and File menu (tools/editor_demo/menu.c)
# stand in for the REPL-flavored controller files
# (src/editor/{input,commit,clipboard,undo,reformat,search,completion}.c
# and the inline overlays), which are not part of this generic demo.
EDITOR_DEMO_OBJS = $(OBJDIR)/tools/editor_demo/editor_demo.o \
                   $(OBJDIR)/tools/editor_demo/menu.o \
                   $(OBJDIR)/tools/editor_demo/input.o \
                   $(addprefix $(OBJDIR)/,$(EDITOR_DEMO_DEP_SRCS:.c=.o))

$(EDITOR_DEMO_BIN): $(EDITOR_DEMO_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(EDITOR_DEMO_OBJS) $(GL_LDFLAGS) -o $@

editor-demo: $(EDITOR_DEMO_BIN) ## Build the standalone editor demo.
	ln -sfn $(EDITOR_DEMO_BIN) editor_demo

# Standalone memory-profiling demo (isolation demo #4). Drives the
# memprof sampler + overlay panel from {support, ui/support, ui/core}
# with no editor/repl/app/ui-app code linked in.
MEMPROF_DEMO_OBJS = $(OBJDIR)/tools/memprof_demo/memprof_demo.o \
                    $(addprefix $(OBJDIR)/,$(MEMPROF_DEMO_DEP_SRCS:.c=.o))

$(MEMPROF_DEMO_BIN): $(MEMPROF_DEMO_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(MEMPROF_DEMO_OBJS) $(GL_LDFLAGS) -o $@

memprof-demo: $(MEMPROF_DEMO_BIN) ## Build the standalone memory-profiling demo.
	ln -sfn $(MEMPROF_DEMO_BIN) memprof_demo

# Standalone CPU-profiling demo (isolation demo #7). Twin of memprof_demo:
# a spinning teapot bracketed by prof sections + the live CPU profile panel,
# from {support, ui/support, ui/core} with no editor/repl/app/ui-app linked in.
CPUPROF_DEMO_OBJS = $(OBJDIR)/tools/cpuprof_demo/cpuprof_demo.o \
                    $(addprefix $(OBJDIR)/,$(CPUPROF_DEMO_DEP_SRCS:.c=.o))

$(CPUPROF_DEMO_BIN): $(CPUPROF_DEMO_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(CPUPROF_DEMO_OBJS) $(GL_LDFLAGS) -o $@

cpuprof-demo: $(CPUPROF_DEMO_BIN) ## Build the standalone CPU-profiling demo.
	ln -sfn $(CPUPROF_DEMO_BIN) cpuprof_demo

# Standalone variable-panel demo (isolation demo #5). Drives the variable
# slider panel + drag math from {subsystems, ui/subsystems, ui/core} with no
# editor/repl/app/ui-app code linked in.
VARIABLE_PANEL_DEMO_OBJS = $(OBJDIR)/tools/variable_panel_demo/variable_panel_demo.o \
                           $(addprefix $(OBJDIR)/,$(VARIABLE_PANEL_DEMO_DEP_SRCS:.c=.o))

$(VARIABLE_PANEL_DEMO_BIN): $(VARIABLE_PANEL_DEMO_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(VARIABLE_PANEL_DEMO_OBJS) $(GL_LDFLAGS) -o $@

variable-panel-demo: $(VARIABLE_PANEL_DEMO_BIN) ## Build the standalone variable-panel demo.
	ln -sfn $(VARIABLE_PANEL_DEMO_BIN) variable_panel_demo

# Standalone color-picker demo (isolation demo #6). Drives the floating color
# picker over a row of GLUT shapes from {subsystems, ui/subsystems, ui/core}
# with no editor/repl/app/ui-app code linked in.
COLOR_PICKER_DEMO_OBJS = $(OBJDIR)/tools/color_picker_demo/color_picker_demo.o \
                         $(addprefix $(OBJDIR)/,$(COLOR_PICKER_DEMO_DEP_SRCS:.c=.o))

$(COLOR_PICKER_DEMO_BIN): $(COLOR_PICKER_DEMO_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(COLOR_PICKER_DEMO_OBJS) $(GL_LDFLAGS) -o $@

color-picker-demo: $(COLOR_PICKER_DEMO_BIN) ## Build the standalone color-picker demo.
	ln -sfn $(COLOR_PICKER_DEMO_BIN) color_picker_demo

# Standalone assignment-plot demo (isolation demo #8). Drives the value-capture
# peer + its panel over a trace the demo generates itself, from {subsystems,
# ui/support, support, ui/core} with no editor/repl/app/ui-app code linked in.
ASSIGN_PLOT_DEMO_OBJS = $(OBJDIR)/tools/assign_plot_demo/assign_plot_demo.o \
                        $(addprefix $(OBJDIR)/,$(ASSIGN_PLOT_DEMO_DEP_SRCS:.c=.o))

$(ASSIGN_PLOT_DEMO_BIN): $(ASSIGN_PLOT_DEMO_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(ASSIGN_PLOT_DEMO_OBJS) $(GL_LDFLAGS) -o $@

assign-plot-demo: $(ASSIGN_PLOT_DEMO_BIN) ## Build the standalone assignment-plot demo.
	ln -sfn $(ASSIGN_PLOT_DEMO_BIN) assign_plot_demo

# Standalone live REPL demo (composition proof). Bootstraps the REPL pipeline +
# the variable-panel peer from a one-file controller: imports scene .c files
# (edited externally in vim), watches their mtime to re-import on save, applies
# each scene's // camera block through a demo-local camera bridge, and drives
# predefined-variable sliders live. No editor / controller / app / render3d / ui-app.
REPL_LIVE_DEMO_OBJS = $(OBJDIR)/tools/repl_live_demo/repl_live_demo.o \
                      $(addprefix $(OBJDIR)/,$(REPL_LIVE_DEMO_DEP_SRCS:.c=.o))

$(REPL_LIVE_DEMO_BIN): $(REPL_LIVE_DEMO_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(REPL_LIVE_DEMO_OBJS) $(GL_LDFLAGS) -o $@

repl-live-demo: $(REPL_LIVE_DEMO_BIN) ## Build the standalone live REPL (file-watch) demo.
	ln -sfn $(REPL_LIVE_DEMO_BIN) repl_live_demo

demos: $(DEMO_TARGETS) ## Build all demos.

COMPILE_REPORT_DEMOS_SUMMARY := $(COMPILE_REPORT_DIR)/demos-summary
$(COMPILE_REPORT_DEMOS_SUMMARY): $(DEMO_TARGETS)
	@bash scripts/compile-report.sh summary "$(COMPILE_REPORT_DIR)"
	@touch $@
demos: $(COMPILE_REPORT_DEMOS_SUMMARY)

.SECONDEXPANSION:

# GNU make normally expands prerequisites before it knows the concrete target.
# .SECONDEXPANSION adds a second pass after `$@` is known, so this one rule can
# turn `test_eval` into `$(test_eval_OBJS)`, `test_repl_core_io` into
# `$(test_repl_core_io_OBJS)`, etc. The doubled dollars delay that lookup until
# the second pass.
define built_binary
$(BINDIR)/$(1): $$($(1)_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $$(dir $$@)
	@bash scripts/compile-report.sh link "$$(COMPILE_REPORT_DIR)" "$$@" "$$(COMPILE_REPORT_VERBOSE)" -- $$(CC) $$(OBJ_CFLAGS) $$($(1)_OBJS) $$($(1)_LDLIBS) $$(COVERAGE_LDFLAGS) -o $$@

$(subst _,-,$(1)): $$(BINDIR)/$(1)
endef

# Ordinary tests are always built against the no-op GL/GLU/GLUT stubs.  Keep
# their public aliases self-contained too: `make test-glr-ctrl` must not
# accidentally select the native GL link path just because it bypasses
# `make test`.  Tests that need a real context are listed separately in
# GL_TEST_BINS and run only through `make gl-tests`.
define built_test_binary
$(BINDIR)/$(1): $$($(1)_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $$(dir $$@)
	@bash scripts/compile-report.sh link "$$(COMPILE_REPORT_DIR)" "$$@" "$$(COMPILE_REPORT_VERBOSE)" -- $$(CC) $$(OBJ_CFLAGS) $$($(1)_OBJS) $$($(1)_LDLIBS) $$(COVERAGE_LDFLAGS) -o $$@

ifeq ($$(USE_GL_STUBS),1)
$(subst _,-,$(1)): $$(BINDIR)/$(1) $(COMPILE_REPORT_DIR)/$(1)-summary
else
$(subst _,-,$(1)):
	+$$(MAKE) --no-print-directory $$@ USE_GL_STUBS=1
endif

$(COMPILE_REPORT_DIR)/$(1)-summary: $$(BINDIR)/$(1)
	@bash scripts/compile-report.sh summary "$$(COMPILE_REPORT_DIR)"
	@touch $$@
endef

$(foreach test,$(TEST_BINS),$(eval $(call built_test_binary,$(test))))
$(foreach bin,$(BENCH_BINS),$(eval $(call built_binary,$(bin))))

# Real-GL tests: need an actual GL context (created via GLUT), which the
# no-op stub harness cannot model and headless CI cannot provide. These
# are intentionally NOT in TEST_BINS, so `make test` / `make test-stubs`
# never build or run them. Run locally with a display via `make gl-tests`.
GL_TEST_BINS = test_ui_gl_state test_scene_underwater_fill_gl test_attrib_bits_gl \
	test_tour_overlay_feedback test_gl_state_inspector_gl

$(BINDIR)/test_ui_gl_state: $(OBJDIR)/$(TEST_DIR)/test_ui_gl_state.o | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(OBJDIR)/$(TEST_DIR)/test_ui_gl_state.o $(GL_LDFLAGS) -o $@

# Differential oracle: drives one GLCmd program through both the real executor
# (against a live context) and the pure gl_state_inspector fold, then compares
# each report row with glGet*. Needs the executor + generated-setup enumeration,
# so it links CORE_TEST_OBJS against real GL like the tour feedback test.
$(BINDIR)/test_gl_state_inspector_gl: \
	$(OBJDIR)/$(TEST_DIR)/test_gl_state_inspector_gl.o $(CORE_TEST_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(OBJDIR)/$(TEST_DIR)/test_gl_state_inspector_gl.o $(CORE_TEST_OBJS) $(GL_LDFLAGS) -o $@

# Captures the controlled-tour overlay + HUD passes with GL_FEEDBACK and asserts
# on the drawn geometry (ring suppression on seek, HUD containment). Needs the
# whole controller graph, so it links CORE_TEST_OBJS like the stub transport
# test but against a real GL context.
$(BINDIR)/test_tour_overlay_feedback: $(OBJDIR)/$(TEST_DIR)/test_tour_overlay_feedback.o $(CORE_TEST_OBJS) | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(OBJDIR)/$(TEST_DIR)/test_tour_overlay_feedback.o $(CORE_TEST_OBJS) $(GL_LDFLAGS) -o $@

# Real-GL oracle: proves the attrib_bits.c cell->bit table matches what the
# driver's glPushAttrib/glPopAttrib actually save/restore. Links the pure
# mapping module (attrib_bits) + its spec-table dependency (command_spec); the
# two collector-only state accessors are stubbed inside the test.
$(BINDIR)/test_attrib_bits_gl: $(OBJDIR)/$(TEST_DIR)/test_attrib_bits_gl.o \
	$(OBJDIR)/src/repl/attrib_bits.o $(OBJDIR)/src/repl/command_spec.o | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(OBJDIR)/$(TEST_DIR)/test_attrib_bits_gl.o \
		$(OBJDIR)/src/repl/attrib_bits.o $(OBJDIR)/src/repl/command_spec.o \
		$(GL_LDFLAGS) -o $@

# Drives scene_grid_render(GRID_THEME_OCEAN) with cam_world_y < 0 and
# nv_fog_distance_supported = 1, then glReadPixels and checks corner
# pixels. Reproduces the post-fb976f0 underwater-fill regression on
# drivers that advertise GL_NV_fog_distance.
$(BINDIR)/test_scene_underwater_fill_gl: $(OBJDIR)/$(TEST_DIR)/test_scene_underwater_fill_gl.o $(OBJDIR)/src/render3d/grid.o | $(COMPILE_REPORT_START)
	@mkdir -p $(dir $@)
	@bash scripts/compile-report.sh link "$(COMPILE_REPORT_DIR)" "$@" "$(COMPILE_REPORT_VERBOSE)" -- $(CC) $(OBJ_CFLAGS) $(OBJDIR)/$(TEST_DIR)/test_scene_underwater_fill_gl.o \
		$(OBJDIR)/src/render3d/grid.o $(GL_LDFLAGS) -o $@

gl-tests: $(addprefix $(BINDIR)/,$(GL_TEST_BINS)) ## Run real-GL UI state tests (needs a display; excluded from `make test`).
	@bash scripts/compile-report.sh summary "$(COMPILE_REPORT_DIR)"
	@for b in $(addprefix $(BINDIR)/,$(GL_TEST_BINS)); do \
	  printf '$(CYAN)==> %s$(NC)\n' "$$b"; "$$b" || exit $$?; \
	done

# Public targets use kebab-case. Binary names retain their existing underscores,
# so the demo targets above create links with the names users execute.
BUILD_TARGETS = \
	all gl-repl app demos $(DEMO_TARGETS) render3d-asset-builder \
	web web-serve glut debug debug-msan

PACKAGE_TARGETS = \
	release release-build release-upload release-config fetch-music \
	icon-regen icon-cube icon-cube-strong

TEST_TARGETS = \
	test test-detailed test-stubs test-asan-ubsan test-msan test-full \
	test-scenes internal-test-scenes \
	test-web internal-test-suite-web \
	rebuild-golden internal-test-suite internal-test-case \
	internal-rebuild-golden gl-tests $(TEST_TARGET_NAMES) \
	$(RUN_TEST_TARGETS) $(RUN_TEST_FILE_TARGETS)

BENCH_TARGETS = \
	bench bench-csv bench-web bench-web-csv bench-web-gl4es $(BENCH_TARGET_NAMES) \
	bench-glut-bitmap bench-glut-bitmap-build bench-glut-bitmap-apple \
	bench-glut-bitmap-freeglut glut-bitmap-freeglut-lib \
	bench-code-panel-text bench-code-panel-stencil bench-vertex-labels

MAINTENANCE_TARGETS = \
	check audit-editor-ownership fix-doc-links find-trailing-whitespace \
	keymap-list config-list check-config-slugs palette-list unicode-count fix-unicode capacity-matrix lines lines-test coverage analyze \
	clean distclean freeglut-clean install-hooks install-completions \
	render3d-hot-lib require-emcc \
	callgraph-static callgraph-static-entry callgraph-profile \
	callgraph-graphviz callgraph-html callgraph-files \
	help help-details FORCE

# The vendored static freeglut (macOS) is a build-time artifact, so every binary
# whose link line embeds its archive path through $(GL_LDFLAGS) must order-only
# depend on it - otherwise those links run before the archive exists and fail.
# Placed here, after every referenced target variable is defined, because a static
# target list expands at parse time, so an earlier placement would silently
# attach the prereq to nothing. This is a NORMAL prerequisite (not order-only):
# the archive's mtime now bumps only when it is missing or re-vendored (the
# VENDORED.txt prereq above), so a fresh archive must relink its consumers -
# an order-only `|` would build the lib first but skip the relink, stranding
# the binary on a stale archive after a re-vendor. Skipped under `make glut` (FREEGLUT_VENDOR=0)
# and on the default Linux path / GL stubs (FREEGLUT_LIB is empty there too).
# Link recipes must use their declared object lists rather than `$^`, otherwise
# this prerequisite duplicates the archive already supplied by GL_LDFLAGS.
# Vendored freeglut is built on macOS (Cocoa or OSMesa), on Linux for the
# OSMesa backend (FREEGLUT_OSMESA=1) and for the opt-in windowed X11/GLX build
# (FREEGLUT_VENDOR_LINUX=1), and for the Emscripten/wasm web target
# (WEB=1, all platforms -- see FREEGLUT_BUILD/FREEGLUT_STATIC_LIB up top);
# the default native Linux path uses system freeglut and needs no archive
# prereq. This condition must cover exactly the arms that put
# $(FREEGLUT_STATIC_LIB) on the link line via FREEGLUT_LIB, or the archive is
# linked without ever being built.
ifeq ($(FREEGLUT_VENDOR),1)
ifneq ($(filter Darwin,$(UNAME_S))$(filter 1,$(FREEGLUT_OSMESA))$(filter 1,$(WEB))$(filter 1,$(FREEGLUT_VENDOR_LINUX)),)
ifneq ($(USE_GL_STUBS),1)
$(SAMPLE_BIN) $(RENDER3D_DEMO_BIN) $(REPL_DEMO_BIN) $(REPL_LIVE_DEMO_BIN) $(EDITOR_DEMO_BIN) \
$(MEMPROF_DEMO_BIN) $(CPUPROF_DEMO_BIN) $(VARIABLE_PANEL_DEMO_BIN) \
$(COLOR_PICKER_DEMO_BIN) $(ASSIGN_PLOT_DEMO_BIN) $(CODE_PANEL_TEXT_BENCH_BIN) \
$(addprefix $(BINDIR)/,$(TEST_BINS) $(BENCH_BINS) $(GL_TEST_BINS)): $(FREEGLUT_STATIC_LIB)
endif
endif
endif

RUN_STATE_OWNERSHIP_CHECKS = \
	REPL_SRCS='$(REPL_SRCS)' \
	RENDER3D_SRCS='$(RENDER3D_SRCS)' \
	UI_SRCS='$(UI_SRCS)' \
	STATE_NEUTRAL_SRCS='$(STATE_NEUTRAL_SRCS)' \
	REPL_LIVE_DEMO_DEP_SRCS='$(REPL_LIVE_DEMO_DEP_SRCS)' \
	MEMPROF_DEMO_DEP_SRCS='$(MEMPROF_DEMO_DEP_SRCS)' \
	CPUPROF_DEMO_DEP_SRCS='$(CPUPROF_DEMO_DEP_SRCS)' \
	VARIABLE_PANEL_DEMO_DEP_SRCS='$(VARIABLE_PANEL_DEMO_DEP_SRCS)' \
	COLOR_PICKER_DEMO_DEP_SRCS='$(COLOR_PICKER_DEMO_DEP_SRCS)' \
	ASSIGN_PLOT_DEMO_DEP_SRCS='$(ASSIGN_PLOT_DEMO_DEP_SRCS)' \
	bash scripts/check/run-state-ownership.sh

# Layering boundary enforcement ------------------------------------------
check-gl-boundaries: ## Verify GL/GLUT calls are isolated to allowed files.
	@echo "    Checking GL/GLU drawing calls isolation..."
	@! grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*[[:space:]]*\(' $(REPL_SRCS) | grep -v '^src/repl/executor\.c:' | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GL/GLU calls found outside src/repl/executor.c$(NC)" && exit 1)
	@echo "    Checking GL/GLU calls in gl_repl.h..."
	@! grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*[[:space:]]*\(' gl_repl.h src/app/glr_defaults.h | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GL/GLU calls found in gl_repl.h$(NC)" && exit 1)
	@echo "    Checking GLUT input/feedback calls isolation..."
	@! grep -nE '\bglut[A-Z][A-Za-z0-9]*[[:space:]]*\(' $(REPL_SRCS) | grep -vE '^src/repl/executor\.c:' | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GLUT calls found outside src/repl/executor.c$(NC)" && exit 1)
	@echo "    GL/GLUT boundaries $(GREEN)OK$(NC)"

check-layer-coupling: ## Verify UI and render3d layers don't include each other's headers.
	@echo "    Checking UI/render3d layer coupling..."
	@! grep -nE '#include\s+"render3d/' $(UI_SRCS) $(UI_HDRS) || (echo "    $(RED)ERROR: UI files must not include render3d headers$(NC)" && exit 1)
	@! grep -nE '#include\s+"ui/' $(RENDER3D_SRCS) $(RENDER3D_HDRS) || (echo "    $(RED)ERROR: render3d files must not include UI headers$(NC)" && exit 1)
	@echo "    Layer coupling $(GREEN)OK$(NC)"


check-controller-boundaries: ## Verify controller owns the render3d/UI wiring boundary.
	@$(RUN_STATE_OWNERSHIP_CHECKS) check-controller-boundaries

check-render3d-no-repl-state-mut: ## Verify render3d code does not mutate REPL state directly.
	@$(RUN_STATE_OWNERSHIP_CHECKS) check-render3d-no-repl-state-mut

check-pure-render3d-no-repl-state: ## Verify render3d files do not reach into REPL state/replay APIs.
	@$(RUN_STATE_OWNERSHIP_CHECKS) check-pure-render3d-no-repl-state

check-state-boundaries: ## Verify REPL state facade usage stays in owned modules.
	@$(RUN_STATE_OWNERSHIP_CHECKS) check-state-boundaries

check-views-no-owners: ## Verify render3d/UI files do not include src/repl/state_owners.h.
	@$(RUN_STATE_OWNERSHIP_CHECKS) check-views-no-owners

check-ui-no-repl-state-mut: ## Verify UI files do not mutate REPL state directly.
	@$(RUN_STATE_OWNERSHIP_CHECKS) check-ui-no-repl-state-mut

check-no-write-through-view: ## Verify no writes happen through pointer fields on view structs.
	@bash scripts/check/check-no-write-through-view.sh scripts/allowlists/write-through-view.txt $(UI_SRCS) $(RENDER3D_SRCS)

check-runtime-state-value-fields: ## Verify ReplRuntimeState owns values, not pointer aliases.
	@bash scripts/check/check-runtime-state-value-fields.sh src/repl/state.h

check-views-flat-types: ## Verify view/state snapshot structs avoid mutable pointer fields.
	@bash scripts/check/check-views-flat.sh scripts/baselines/views-flat-violations.txt

check-views-by-value-snapshot: ## Ratchet pointer-return snapshot accessors down over time.
	@bash scripts/check/check-views-by-value-snapshot.sh scripts/baselines/by-value-snapshot-pointer-returns.txt

check-ui-renderer-takes-view: ## Verify audited UI renderers use canonical snapshot signatures.
	@bash scripts/check/check-ui-renderer-signatures.sh scripts/allowlists/ui-renderers-signature.txt

check-renderer-no-direct-mutators: ## Verify audited renderers do not mutate state directly.
	@bash scripts/check/check-renderer-purity.sh scripts/allowlists/renderer-purity.txt

check-output-actualization: ## Verify Ui*Output fields are consumed by controller actualization.
	@bash scripts/check/check-output-actualization.sh

check-state-c-shrinking: ## Ratchet src/repl/state.c line count down over time.
	@bash scripts/check/check-state-c-shrinking.sh scripts/baselines/state-c-lines.txt src/repl/state.c

check-repl-no-direct-editor: ## Forbid editor coupling in repl_*.{c,h} (hard zero).
	@bash scripts/check/check-repl-no-direct-editor.sh

check-editor-no-app: ## Ratchet: forbid new app/glr_* coupling in src/editor/ (see audit #8).
	@bash scripts/check/check-editor-no-app.sh

check-repl-no-app: ## Ratchet: forbid new app/glr_* coupling in src/repl/.
	@bash scripts/check/check-repl-no-app.sh

check-repl-no-mut-reads: ## Ratchet: cap `_mut()` calls in src/repl/ outside owner files (audit #7/#14).
	@bash scripts/check/check-repl-no-mut-reads.sh

check-render3d-no-upper-layers: ## Hard guard: src/render3d/ must not include from app/editor/ui/subsystems.
	@bash scripts/check/check-render3d-no-upper-layers.sh

check-ui-core-no-upper-layers: ## Hard guard: src/ui/core/ must not include from app/editor/repl/render3d/subsystems/ui-app.
	@bash scripts/check/check-ui-core-no-upper-layers.sh

check-repl-demo-no-editor: ## Forbid editor implementation in the standalone demo.
	@bash scripts/check/check-repl-demo-no-editor.sh

check-repl-live-demo-no-editor: ## Forbid editor implementation in the standalone live REPL demo.
	@bash scripts/check/check-repl-demo-no-editor.sh repl_live_demo REPL_LIVE_DEMO_DEP_SRCS

check-memprof-demo-isolation: ## Forbid app/repl/editor coupling in the memprof demo link set.
	@bash scripts/check/check-subsystem-demo-isolation.sh MEMPROF_DEMO_DEP_SRCS tools/memprof_demo memprof_demo

check-cpuprof-demo-isolation: ## Forbid app/repl/editor coupling in the cpuprof demo link set.
	@bash scripts/check/check-subsystem-demo-isolation.sh CPUPROF_DEMO_DEP_SRCS tools/cpuprof_demo cpuprof_demo

check-cpuprof-standalone: ## Verify the generic CPU-profile timer compiles with no section catalog (fallback path).
	@bash scripts/check/check-cpuprof-standalone.sh

check-audio-nothread: ## Verify glr_audio.c compiles single-threaded (GLR_AUDIO_NO_THREAD=1, the Emscripten path).
	@bash scripts/check/check-audio-nothread.sh

check-variable-panel-demo-isolation: ## Forbid app/repl/editor coupling in the variable-panel demo link set.
	@bash scripts/check/check-subsystem-demo-isolation.sh VARIABLE_PANEL_DEMO_DEP_SRCS tools/variable_panel_demo variable_panel_demo

check-color-picker-demo-isolation: ## Forbid app/repl/editor coupling in the color-picker demo link set.
	@bash scripts/check/check-subsystem-demo-isolation.sh COLOR_PICKER_DEMO_DEP_SRCS tools/color_picker_demo color_picker_demo

check-assign-plot-demo-isolation: ## Forbid app/repl/editor coupling in the assignment-plot demo link set.
	@bash scripts/check/check-subsystem-demo-isolation.sh ASSIGN_PLOT_DEMO_DEP_SRCS tools/assign_plot_demo assign_plot_demo

check-source-document-port-owners: ## source_document_* symbols only defined in approved host adapters.
	@bash scripts/check/check-source-document-port-owners.sh

check-no-facade-include-in-views: ## Verify view/render files avoid repl_state facade headers.
	@bash scripts/check/check-no-facade-include-in-views.sh scripts/allowlists/facade-includes-in-views.txt

check-domain-owner-encapsulation: ## Enforce per-domain mutator encapsulation rules as domains migrate.
	@bash scripts/check/check-domain-encapsulation.sh scripts/allowlists/domain-owner-encapsulation.txt

check-ui-no-repl-state-read: ## Verify UI renderers consume the UiRenderSnapshot, not live repl_state_*().
	@echo "Checking UI render entry points consume UiRenderSnapshot..."
	@bad=$$(grep -nE 'repl_state_[A-Za-z0-9_]+\s*\(' $(UI_SRCS) 2>/dev/null \
		|| true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: ui_*.c files outside the input-bridge allowlist read live repl_state_*():$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@bash scripts/check/check-ui-renderer-signatures.sh scripts/allowlists/ui-renderers-signature.txt
	@echo "ui-no-repl-state-read $(GREEN)OK$(NC)"

check-state-ownership: ## Run state-ownership contract checks (new + tightened existing checks).
	@$(RUN_STATE_OWNERSHIP_CHECKS)

check-prof-sections-instrumented: ## Hard guard: every prof_sections.h catalog row has a prof_begin() site (no zombie profiler rows).
	@bash scripts/check/check-prof-sections-instrumented.sh

check-public-api-usage: ## Scan public API declarations for unused functions (informational).
	@bash scripts/check/check-unused-apis.sh

check-duplicate-api-decls: ## Scan module public headers for duplicate function declarations; fails if any are found.
	@bash scripts/check/check-duplicate-api-decls.sh

audit-editor-ownership: ## Report editor/REPL/UI ownership drift (informational; see done/editor-owns-text-completion.md).
	@bash scripts/audit_editor_ownership.sh

check-editor-ownership-budget: ## Ratchet the editor/UI transitional-coupling budget down only.
	@bash scripts/check/check-editor-ownership-budget.sh scripts/baselines/editor-ownership-budget.txt

check-no-store-text-api: ## Verify repl_command_store_*_with_line[s] API stays gone.
	@bash scripts/check/check-no-store-text-api.sh

check-repl-no-direct-buffer-read: ## Verify repl_*.c readers go through EditorBufferView, not editor_buffer_line().
	@bash scripts/check/check-repl-no-direct-buffer-read.sh scripts/allowlists/repl-no-direct-buffer-read.txt

check-glr-ctrl-not-editor-mirror: ## Verify imrepl_ctrl does not grow per-field editor wrappers.
	@bash scripts/check/check-glr-ctrl-not-editor-mirror.sh

check-ui-returns-hits-only: ## Verify ui_*.c input helpers do not call REPL/editor mutators (ratchet down only).
	@bash scripts/check/check-ui-returns-hits-only.sh scripts/baselines/ui-returns-hits-only.txt

check-ui-text-panel-pure: ## Verify src/ui/core/text_panel.* stays REPL/editor-free.
	@bash scripts/check/check-ui-text-panel-pure.sh

check-editor-repl-surface: ## Ratchet direct repl_* call surface in src/editor/input.c and commit.c.
	@bash scripts/check/check-editor-repl-surface.sh scripts/baselines/editor-repl-surface.txt

check-edit-ops-pure: ## Verify src/editor/edit_ops.* stays REPL-free.
	@bash scripts/check/check-edit-ops-pure.sh

check-no-raw-undo-clear: ## Production code must use editor_undo_note_wholesale_replacement(), not raw editor_undo_clear().
	@bash scripts/check/check-no-raw-undo-clear.sh

check-ui-panels-no-mutators: ## Hard guard: src/ui/app/panels.c references no input-dispatch mutators.
	@bash scripts/check/check-ui-panels-no-mutators.sh

check-replay-ui-isolation: ## Hard guard: replay_ui_*.c is feature-UI - no editor / REPL mutators or parser/compile/apply calls.
	@bash scripts/check/check-replay-ui-isolation.sh

check-color-picker-ui-isolation: ## Strict guard: src/ui/subsystems/color_picker.c is pure renderer/hit-test over ColorPickerView - no mutators, no live state reads, no parser/compile/apply.
	@bash scripts/check/check-color-picker-ui-isolation.sh

check-variable-panel-forwarders: ## Ratchet variable_panel forwarder API uses (editor_state_variable_drag*, ui_state_variable_panel*, repl_var_drag_*).
	@bash scripts/check/check-variable-panel-forwarders.sh scripts/baselines/variable-panel-forwarders.txt

check-replay-forwarders: ## Ratchet repl_state_replay* forwarder API uses (replay peer is the owner).
	@bash scripts/check/check-replay-forwarders.sh scripts/baselines/replay-forwarders.txt

check-no-repl-commit: ## Verify repl_commit.{c,h} are absent (commit dispatch lives in src/editor/commit.c).
	@bash scripts/check/check-no-repl-commit.sh

check-no-repl-editor-input-shim: ## Verify src/editor/input.c does not delegate to repl_*_func entry points.
	@bash scripts/check/check-no-repl-editor-input-shim.sh

check-no-set-status-in-repl-parser: ## Ratchet set_status calls inside src/repl/parser.c (parser diagnostics flow via ctx->err_buf).
	@bash scripts/check/check-no-set-status-in-repl-parser.sh scripts/baselines/repl-parser-set-status.txt

check-no-set-status-in-compile-apply: ## Verify src/repl/compile.c and src/repl/apply.c never call set_status.
	@bash scripts/check/check-no-set-status-in-compile-apply.sh

check-no-load-line-to-input-in-pipeline: ## Verify REPL pipeline TUs do not call editor-side editor_load_line_to_input.
	@bash scripts/check/check-no-load-line-to-input-in-pipeline.sh

check-repl-state-no-glr-state: ## Verify REPL pipeline TUs do not include src/app/glr_state.h or reference GlrState symbols.
	@bash scripts/check/check-repl-state-no-glr-state.sh

check-app-boot-band: ## Verify the frame-time controller band (src/app/*) does not include boot headers (src/app/boot/*).
	@bash scripts/check/check-app-boot-band.sh

check-depth-capture-after-finish: ## Verify gl_repl.c captures the vertex-label depth snapshot after glFinish and before the swap.
	@bash scripts/check/check-depth-capture-after-finish.sh

check-glr-state-no-repl-mutators: ## Verify src/app/glr_state.c does not call back into REPL state mutators.
	@bash scripts/check/check-glr-state-no-repl-mutators.sh

check-repl-scenes-cfg-clear-paired: ## Verify every g_user_scenes[X].used=0 in src/repl/scenes.c pairs with scene_cfg_clear.
	@bash scripts/check/check-repl-scenes-cfg-clear-paired.sh

check-repl-export-no-ui-layout: ## Verify export/import TUs do not call ui_layout_* / ui_state_*.
	@bash scripts/check/check-repl-export-no-ui-layout.sh

check-repl-export-via-bridge: ## Verify export/import TUs pull app/render3d state only via controller-installed bridges (no scene_*/glr_* calls or render3d/app includes).
	@bash scripts/check/check-repl-export-via-bridge.sh

check-ui-no-export-resolver: ## Verify src/ui reads the snapshot-frozen reshape projection, never calls repl_export_reshape_projection_lines() live.
	@bash scripts/check/check-ui-no-export-resolver.sh

check-no-feed-line-in-pipeline: ## Verify REPL pipeline TUs do not call editor_feed_line().
	@bash scripts/check/check-no-feed-line-in-pipeline.sh

check-repl-no-direct-tutorial-runner: ## Verify REPL pipeline TUs request tutorial teardown through ReplHostEffects.
	@bash scripts/check/check-repl-no-direct-tutorial-runner.sh

check-module-prefixes: ## Verify stale pre-cleanup symbol prefixes have not reappeared under src/.
	@bash scripts/check/check-module-prefixes.sh

check-repl-demo-stubs-shrinking: ## Ratchet on tools/repl_demo/stubs.c - must not grow past 0 stubs.
	@bash scripts/check/check-repl-demo-stubs-shrinking.sh

check-include-style: ## Hard guard: project-local headers must use "X.h", not <X.h>.
	@bash scripts/check/check-include-style.sh

check-completions: ## Hard guard: scripts/completions/ offers exactly the options --help documents.
	@bash scripts/check/check-completions.sh

check-web-glut-get: ## Hard guard: web-reachable glutGet() enums are ones Emscripten's JS GLUT implements (it abort()s on the rest).
	@python3 scripts/check/check-web-glut-get.py

check-log-prefix-single-source: ## Hard guard: gl-repl's log prefix/tag is spelled only in src/app/glr_log_prefix.h.
	@bash scripts/check/check-log-prefix-single-source.sh

check-doc-links: ## Validate local Markdown file links and line anchors.
	@python3 scripts/check/check-doc-links.py

fix-doc-links: ## Attempt to repair Markdown file/line links, then verify.
	@bash scripts/check/fix-doc-links.sh

check-user-guide-keymap: ## Validate USER_GUIDE shortcut claims against keymap.h.
	@python3 scripts/check/check-user-guide-keymap.py

check-user-guide-examples: ## Validate docs' example references and USER_GUIDE's catalog table.
	@python3 scripts/check/check-user-guide-examples.py

check-user-guide-commands: ## Validate every REPL command is listed in USER_GUIDE.md (or explicitly exempt).
	@python3 scripts/check/check-user-guide-commands.py

check-keymap-no-dup: ## Hard guard: no two keymap.h bindings share a (key, mods) - a double-map.
	@bash scripts/keymap.sh check

keymap-list: ## Print current key bindings + the free Ctrl / Ctrl+Shift / F-key slots.
	@bash scripts/keymap.sh list

config-list: gl-repl ## Print config labels and their stable @cfg slugs.
	@set -o pipefail; \
	sort_config() { \
		IFS= read -r header || return 1; \
		printf '%s\n' "$$header"; \
		LC_ALL=C sort -t "$$(printf '\t')" -k1,1; \
	}; \
	if command -v column >/dev/null 2>&1; then \
		./gl-repl --list-config | sort_config | column -t -s "$$(printf '\t')"; \
	else \
		./gl-repl --list-config | sort_config; \
	fi

check-config-slugs: gl-repl ## Check scene @cfg headers against the config table.
	@python3 scripts/check/check-config-slugs.py ./gl-repl examples/scenes tests/scenes

check-palette: ## Hard guard: covered scenes + README table stay on the active accent palette (accent_palette.h).
	@python3 scripts/check/check-palette.py

palette-list: ## Print the active accent palette anchors (floats + hex) and the role map.
	@python3 scripts/check/check-palette.py --list

check-examples-catalog: ## Validate the file-backed built-in example catalog.
	@python3 scripts/gen_examples.py --check --catalog $(EXAMPLES_CATALOG)

check-tours-catalog: ## Validate the file-backed guided-tour catalog.
	@python3 scripts/gen_tours.py --check --catalog $(TOURS_CATALOG)

check-command-descriptions: ## Validate complete GL command/capability popup descriptions.
	@python3 scripts/gen_command_descriptions.py --check \
		--catalog $(COMMAND_DESCRIPTIONS_SOURCE)

check-stroke-fonts: ## Validate and reproduce the vendored high-resolution stroke fonts.
	@python3 scripts/check/check-stroke-fonts.py

check-formatted: ## Verify that example scenes under examples/scenes are formatted correctly.
	@python3 scripts/format_scenes.py --check || ( \
		echo "$(RED)ERROR: Some example scenes are not formatted correctly.$(NC)"; \
		echo "To format them automatically, run: $(CYAN)./scripts/format_scenes.py --write$(NC)"; \
		exit 1; \
	)

check-c99: $(GENERATED_EXAMPLES_INC) $(GENERATED_COMMAND_DESCRIPTIONS_INC) $(GENERATED_TOURS_INC) ## C99 build guard: gl-repl + bench + demo sources must syntax-check under gcc -std=c99 (non-pedantic; tests excluded; in the standard gate).
	@C99_SRCS='$(SRCS)' bash scripts/check/check-c99.sh

check-tier-c-function-size: ## Size ratchet: parse_command and flatten_range must not grow past their baselines.
	@bash scripts/check/check-tier-c-function-size.sh scripts/baselines/tier-c-function-size.txt

check-no-test-default-output: ## Hard guard: tests may not call repl_save_default_output() (writes ./output.c in repo root).
	@bash scripts/check/check-no-test-default-output.sh

find-trailing-whitespace: ## Report all trailing whitespace in tracked source files (whole repo).
	@git ls-files '*.c' '*.h' '*.md' Makefile | xargs grep -rn ' $$' || echo "no trailing whitespace found"

check-trailing-whitespace: ## Verify commits since origin/main contain no trailing whitespace.
	@set -e; \
	base=$${CHECK_BASE:-origin/main}; \
	if git rev-parse --verify "$$base" >/dev/null 2>&1; then \
		merge_base=$$(git merge-base "$$base" HEAD); \
		git --no-pager diff --check "$$merge_base" -- . ':(exclude)third_party/' ':(exclude)packaging/web/patches/'; \
	else \
		git --no-pager diff --cached --check -- . ':(exclude)third_party/' ':(exclude)packaging/web/patches/'; \
		git --no-pager diff --check -- . ':(exclude)third_party/' ':(exclude)packaging/web/patches/'; \
	fi; \
	echo "trailing-whitespace OK"

unicode-count: ## Report Unicode in project C, Markdown, and scene sources and its normalization policy.
	@python3 scripts/count-unicode.py

fix-unicode: ## Replace configured Unicode in project C, Markdown, and scene sources.
	@python3 scripts/count-unicode.py --fix

check-unicode: ## Hard guard: apply configured Unicode replacements in project C, Markdown, and scene sources.
	@python3 scripts/count-unicode.py --check --c-files --glr-files

CHECK_TARGETS = \
	check-trailing-whitespace \
	check-depth-capture-after-finish \
	check-unicode \
	check-doc-links \
	check-user-guide-keymap \
	check-user-guide-examples \
	check-user-guide-commands \
	check-examples-catalog \
	check-tours-catalog \
	check-command-descriptions \
	check-stroke-fonts \
	check-formatted \
	check-gl-boundaries \
	check-layer-coupling \
	check-state-ownership \
	check-ui-text-panel-pure \
	check-public-api-usage \
	check-duplicate-api-decls

check: ## Run all checks.
	@set -e; \
	for target in $(CHECK_TARGETS); do \
		desc=$$(awk -v target="$$target" 'BEGIN {FS = ":.*## "} $$1 == target { print $$2; exit }' $(firstword $(MAKEFILE_LIST))); \
		printf "$(CYAN)\n==> %s$(NC)\n" "$$desc"; \
		$(MAKE) --no-print-directory $$target || exit $$?; \
	done

# `make test` is the portable, headless contract: one check gate followed by
# the ordinary suite against the no-op GL stubs. Real-context tests remain
# opt-in through `make gl-tests`.
test: ## Run the full headless test gate (checks plus GL stubs).
	+$(MAKE) --no-print-directory test-stubs

test-detailed: ## Run the stubbed suite with verbose example export/compile logging.
	+$(MAKE) --no-print-directory internal-test-suite \
		USE_GL_STUBS=1 BUILD=$(BUILD) TEST_VERBOSE=1

internal-test-suite: $(addprefix $(BINDIR)/,$(TEST_BINS)) $(COMPILE_REPORT_TEST_SUMMARY)
	@REPL_EXPORT_VERBOSE=$(if $(filter 1,$(TEST_VERBOSE)),1,0) \
	REPL_EXPORT_CC="$(CC)" \
	REPL_EXPORT_COMPILE_CFLAGS='$(BUILD_CFLAGS) $(CFLAGS)' \
	TEST_JOBS="$(TEST_JOBS)" \
	bash scripts/run-tests.sh $(TEST_RUNNER_CASES)

# GNU make treats tokens after its own `--` as more goals. TEST_ARGS is the
# pass-through for single-test runners, for example:
# `make run-test-repl-core-examples TEST_ARGS='--dump-index 2'`.
$(RUN_TEST_TARGETS): run-%:
	+$(MAKE) --no-print-directory internal-test-case \
		USE_GL_STUBS=1 TEST_CASE=$(subst -,_,$*)

$(RUN_TEST_FILE_TARGETS): run-%:
	+$(MAKE) --no-print-directory internal-test-case \
		USE_GL_STUBS=1 TEST_CASE=$*

internal-test-case: $$(BINDIR)/$$(TEST_CASE)
	@REPL_EXPORT_CC="$(CC)" \
	REPL_EXPORT_COMPILE_CFLAGS='$(BUILD_CFLAGS) $(CFLAGS)' \
	$($(TEST_CASE)_RUN) $(TEST_ARGS)
	@bash scripts/compile-report.sh summary "$(COMPILE_REPORT_DIR)"

# The scene corpora under tests/scenes/ are opt-in: every scene costs an
# export plus a cc invocation, and unlike examples/scenes (which ships to
# users and always runs) they exist to collect corner cases and are meant to
# grow freely. REPL_SCENE_CORPUS is read by tests/support/scene_corpus.h;
# only these two binaries consult it, so only they need rebuilding+running.
SCENE_CORPUS_TESTS = test_repl_core_examples test_camera_header_parity

COMPILE_REPORT_SCENES_SUMMARY := $(COMPILE_REPORT_DIR)/scenes-summary
$(COMPILE_REPORT_SCENES_SUMMARY): $(addprefix $(BINDIR)/,$(SCENE_CORPUS_TESTS))
	@bash scripts/compile-report.sh summary "$(COMPILE_REPORT_DIR)"
	@touch $@

test-scenes: ## Run the opt-in tests/scenes corpora (stress + general) through export+compile.
	+$(MAKE) --no-print-directory internal-test-scenes USE_GL_STUBS=1 BUILD=$(BUILD)

internal-test-scenes: $(addprefix $(BINDIR)/,$(SCENE_CORPUS_TESTS)) $(COMPILE_REPORT_SCENES_SUMMARY)
	@REPL_SCENE_CORPUS=1 \
	REPL_EXPORT_VERBOSE=$(if $(filter 1,$(TEST_VERBOSE)),1,0) \
	REPL_EXPORT_CC="$(CC)" \
	REPL_EXPORT_COMPILE_CFLAGS='$(BUILD_CFLAGS) $(CFLAGS)' \
	TEST_JOBS="$(TEST_JOBS)" \
	bash scripts/run-tests.sh \
		$(foreach test,$(SCENE_CORPUS_TESTS),'$(test):::$($(test)_RUN)')

rebuild-golden: ## Rebuild all golden examples from test_repl_core_examples.
	+$(MAKE) --no-print-directory internal-rebuild-golden USE_GL_STUBS=1

internal-rebuild-golden: $(BINDIR)/test_repl_core_examples
	@$(BINDIR)/test_repl_core_examples --update-golden

test-stubs: check ## Build and run tests using local GL/GLU/GLUT stubs, without GL libs.
	+$(MAKE) --no-print-directory internal-test-suite \
		USE_GL_STUBS=1 BUILD=$(BUILD) \
		NO_SAN=$(if $(filter undefined,$(origin NO_SAN)),1,$(NO_SAN))

test-no-checks: ## Build and run tests using local GL stubs, without running pre-checks.
	+$(MAKE) --no-print-directory internal-test-suite \
		USE_GL_STUBS=1 BUILD=$(BUILD) \
		NO_SAN=$(if $(filter undefined,$(origin NO_SAN)),1,$(NO_SAN))

test-only: test-no-checks ## Alias for test-no-checks.

test-asan-ubsan: ## Build and run the stubbed test suite under AddressSanitizer + UBSan (forces sanitizers on regardless of environment).
	+$(MAKE) --no-print-directory internal-test-suite \
		USE_GL_STUBS=1 BUILD=debug SAN=address NO_SAN=0

test-msan: ## Build and run stubbed tests with MemorySanitizer.
ifeq ($(UNAME_S),Darwin)
	@printf "WARNING: MemorySanitizer is not supported on macOS (Darwin). Skipping test-msan.\n" >&2
else
	GLR_AUDIO_NO_DEVICE=1 $(MAKE) internal-test-suite \
		USE_GL_STUBS=1 BUILD=debug SAN=memory CC=$(MSAN_CC)
endif

# The wasm twin of `make test-stubs`: the same test binaries, the same no-op GL
# stubs, the same parallel runner -- compiled by emcc and run under node. What
# it adds over the native gate is the __EMSCRIPTEN__ side of every #ifdef in
# the tree (menu_bar, edit_overlays, geometry_guides, glr_audio, glr_clipboard,
# glr_web_io, memprof, help_text ...), plus wasm's own 32-bit pointers and
# stricter alignment, none of which a native x86/arm64 run can reach.
#
# What it does NOT cover: gl4es -> WebGL2. This lane links the GL stubs, so no
# GL call goes anywhere. A regression in the draw path is invisible here
# exactly as it is in `make bench-web` -- that needs a browser.
#
# Unlike `make web` / `make bench-web` this needs no scripts/web-deps.sh and no
# third_party/web checkout, because there is no gl4es in the link.
#
# Deliberately absent from the BUILD=debug goal list up top: this runs release
# wasm, like `make bench-web` and like the shipping web build. `make test`
# already owns the sanitizer gate, and it runs the same C.
test-web: require-emcc ## Build and run the test suite as wasm under node (CPU pipeline + __EMSCRIPTEN__ branches; needs emcc + node).
	@command -v node >/dev/null 2>&1 || { \
		echo "ERROR: node not found on PATH -- test-web runs the wasm tests under node."; \
		exit 1; \
	}
	+$(MAKE) --no-print-directory internal-test-suite-web WEB=1 USE_GL_STUBS=1

internal-test-suite-web: $(addprefix $(BINDIR)/,$(WEB_TEST_BINS)) $(COMPILE_REPORT_WEB_TEST_SUMMARY)
	@GLR_AUDIO_NO_DEVICE=1 \
	REPL_EXPORT_VERBOSE=$(if $(filter 1,$(TEST_VERBOSE)),1,0) \
	REPL_EXPORT_CC="cc" \
	TEST_JOBS="$(TEST_JOBS)" \
	bash scripts/run-tests.sh $(WEB_TEST_RUNNER_CASES)

test-full: ## Run the full build, test, sanitizer, benchmark, and real-GL gate.
	@set -e; for target in $(HEADLESS_DEMO_TARGETS); do \
		$(MAKE) --no-print-directory $$target USE_GL_STUBS=1; \
	done
	+$(MAKE) --no-print-directory test-stubs NO_SAN=0
	+$(MAKE) --no-print-directory test-scenes
	+$(MAKE) --no-print-directory test-msan
	+$(MAKE) --no-print-directory gl-repl
	+$(MAKE) --no-print-directory gl-tests
	+$(MAKE) --no-print-directory bench
	+$(MAKE) --no-print-directory clean
	+$(MAKE) --no-print-directory glut

install-hooks: ## Point this clone's git hooks at the tracked .githooks/ directory.
	@git config core.hooksPath .githooks
	@echo "git core.hooksPath -> .githooks (pre-push: check-trailing-whitespace + test-stubs + git lfs pre-push)"

install-completions: ## Add the bundled zsh completions to ~/.zshrc (idempotent; override ZSHRC to choose another file).
	@touch "$(ZSHRC)"
	@if grep -Fqx '# >>> gl-repl completions >>>' "$(ZSHRC)"; then \
		echo "gl-repl completions already installed in $(ZSHRC)"; \
	else \
		printf '%s\n' \
			'' \
			'# >>> gl-repl completions >>>' \
			'if (( ! $${+functions[compdef]} )); then' \
			'  autoload -Uz compinit && compinit' \
			'fi' \
			'source "$(ZSH_COMPLETIONS_DIR)/_gl-repl"' \
			'source "$(ZSH_COMPLETIONS_DIR)/_docs-assets.sh"' \
			'# <<< gl-repl completions <<<' >> "$(ZSHRC)"; \
		echo "installed gl-repl completions in $(ZSHRC)"; \
	fi

# Benchmark targets ------------------------------------------------------
# Built and invoked separately from `make test` because timing is sensitive
# to system load and we don't want a stray slow run failing CI. Use
# BENCH_ARGS to pass through flags, e.g. `make bench BENCH_ARGS="--iters 20"`.
BENCH_ARGS ?=

# Standalone bitmap-font benchmark.  It intentionally has its own two small
# binaries instead of joining BENCH_BINS: one links Apple GLUT and the other
# links a separately configured freeglut, and both need a live macOS window.
# Override both variables together to benchmark another checkout, for example:
#   make bench-glut-bitmap \
#     GLUT_BITMAP_FREEGLUT_SRC=$$HOME/src/freeglut-fork \
#     GLUT_BITMAP_FREEGLUT_BUILD=build/glut-bitmap-bench/freeglut-fork
GLUT_BITMAP_BENCH_DIR := build/glut-bitmap-bench
GLUT_BITMAP_BENCH_SRC := bench/bench_glut_bitmap.c
GLUT_BITMAP_APPLE_BIN := $(GLUT_BITMAP_BENCH_DIR)/apple-glut
GLUT_BITMAP_FREEGLUT_BIN := $(GLUT_BITMAP_BENCH_DIR)/freeglut
GLUT_BITMAP_FREEGLUT_SRC ?= $(FREEGLUT_SRC)
GLUT_BITMAP_FREEGLUT_BUILD ?= $(GLUT_BITMAP_BENCH_DIR)/freeglut-build
GLUT_BITMAP_FREEGLUT_LIB := $(GLUT_BITMAP_FREEGLUT_BUILD)/lib/libglut.a
GLUT_BITMAP_BENCH_ARGS ?=
GLUT_BITMAP_BENCH_ARCH ?= $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
glut-bitmap-freeglut-lib:
	@mkdir -p $(GLUT_BITMAP_FREEGLUT_BUILD)
	cmake -S $(GLUT_BITMAP_FREEGLUT_SRC) -B $(GLUT_BITMAP_FREEGLUT_BUILD) \
	  -DFREEGLUT_COCOA=ON -DFREEGLUT_BUILD_STATIC_LIBS=ON \
	  -DFREEGLUT_BUILD_SHARED_LIBS=OFF -DFREEGLUT_BUILD_DEMOS=OFF \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_OSX_ARCHITECTURES=$(GLUT_BITMAP_BENCH_ARCH)
	+cmake --build $(GLUT_BITMAP_FREEGLUT_BUILD) --target freeglut_static

$(GLUT_BITMAP_APPLE_BIN): $(GLUT_BITMAP_BENCH_SRC)
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE \
	  -arch $(GLUT_BITMAP_BENCH_ARCH) \
	  -DGL_SILENCE_DEPRECATION -DGLUT_BITMAP_BENCH_APPLE \
	  $< -framework OpenGL -framework GLUT -o $@

$(GLUT_BITMAP_FREEGLUT_BIN): $(GLUT_BITMAP_BENCH_SRC) glut-bitmap-freeglut-lib
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE \
	  -arch $(GLUT_BITMAP_BENCH_ARCH) \
	  -DGL_SILENCE_DEPRECATION -DFREEGLUT_STATIC \
	  -I$(GLUT_BITMAP_FREEGLUT_SRC)/include $< \
	  $(GLUT_BITMAP_FREEGLUT_LIB) -lm -framework IOKit -framework Cocoa \
	  -framework OpenGL -framework CoreVideo -o $@

bench-glut-bitmap-build: $(GLUT_BITMAP_APPLE_BIN) $(GLUT_BITMAP_FREEGLUT_BIN) ## Build the isolated Apple GLUT/freeglut bitmap-font benchmarks.

bench-glut-bitmap-apple: $(GLUT_BITMAP_APPLE_BIN) ## Benchmark successive glutBitmapCharacter calls using Apple GLUT.
	$(GLUT_BITMAP_APPLE_BIN) $(GLUT_BITMAP_BENCH_ARGS)

bench-glut-bitmap-freeglut: $(GLUT_BITMAP_FREEGLUT_BIN) ## Benchmark freeglut character-loop and glutBitmapString paths.
	$(GLUT_BITMAP_FREEGLUT_BIN) $(GLUT_BITMAP_BENCH_ARGS)

bench-glut-bitmap: bench-glut-bitmap-build ## Compare Apple GLUT with freeglut bitmap rendering (macOS, opens two short-lived windows).
	@echo "==> Apple GLUT"
	@$(GLUT_BITMAP_APPLE_BIN) $(GLUT_BITMAP_BENCH_ARGS)
	@echo
	@echo "==> freeglut ($(GLUT_BITMAP_FREEGLUT_SRC))"
	@$(GLUT_BITMAP_FREEGLUT_BIN) $(GLUT_BITMAP_BENCH_ARGS)
else
bench-glut-bitmap-build bench-glut-bitmap-apple bench-glut-bitmap-freeglut bench-glut-bitmap:
	@echo "ERROR: the Apple GLUT comparison is available only on macOS." >&2
	@exit 1
endif

# Code-panel text-submission benchmark (Linux/Mesa). Prices a bitmap draw call
# against a glyph, which is what syntax highlighting trades: highlighting keeps
# the glyph count and multiplies the span count.
#
# It links the *real* src/ui/core/text_panel.c and calls ui_text_panel_render(),
# so the benchmark cannot drift from the code it measures. That link set is the
# one check-ui-text-panel-pure keeps REPL-free, and matches EDITOR_DEMO_DEP_SRCS
# minus the editor data model. Objects come from the ordinary $(OBJDIR) rules,
# so the TUs are built with the same flags as gl-repl itself.
#
# Uses whichever freeglut the local gl-repl build links, so
# FREEGLUT_VENDOR_LINUX=1 measures the vendored static X11/GLX build (and is
# required on a box with no system freeglut).
CODE_PANEL_TEXT_BENCH_DEP_SRCS = src/ui/core/text_layout.c \
                                 src/ui/core/text_panel.c \
                                 src/ui/core/text_search.c \
                                 src/ui/core/theme.c \
                                 src/support/cpuprof.c \
                                 src/support/histogram.c \
                                 src/support/runstats.c
CODE_PANEL_TEXT_BENCH_OBJS = $(OBJDIR)/bench/bench_code_panel_text.o \
                             $(addprefix $(OBJDIR)/,$(CODE_PANEL_TEXT_BENCH_DEP_SRCS:.c=.o))
CODE_PANEL_TEXT_BENCH_BIN := $(BINDIR)/bench_code_panel_text
CODE_PANEL_TEXT_BENCH_ARGS ?=

ifneq ($(filter Linux Darwin,$(UNAME_S)),)
$(CODE_PANEL_TEXT_BENCH_BIN): $(CODE_PANEL_TEXT_BENCH_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) $(CODE_PANEL_TEXT_BENCH_OBJS) $(GL_LDFLAGS) -o $@

bench-code-panel-text: $(CODE_PANEL_TEXT_BENCH_BIN) ## Benchmark code-panel text submission vs span fragmentation (Linux/macOS; opens a short-lived window).
	$(CODE_PANEL_TEXT_BENCH_BIN) $(CODE_PANEL_TEXT_BENCH_ARGS) $(BENCH_ARGS)
else
bench-code-panel-text:
	@echo "ERROR: bench-code-panel-text targets Linux/macOS (needs freeglut)." >&2
	@exit 1
endif

# Stencil-routed code-panel text benchmark (Linux/macOS). Tests whether routing
# glyph color through the stencil buffer - one masked glyph pass that tags
# pixels, then one gated fill per distinct color - beats the current per-span
# glColor. Trades N per-span color changes for C full-quad fills plus overdraw,
# so it is a measurement, not a refactor: it draws the glyphs a way the current
# renderer has no path for, and therefore cannot link ui_text_panel_render().
# Its "direct" case is the control that keeps the model honest against
# bench_code_panel_text.c, whose corpus and harness it shares.
CODE_PANEL_STENCIL_BENCH_SRC := bench/bench_code_panel_stencil.c
CODE_PANEL_STENCIL_BENCH_BIN := build/code-panel-stencil-bench/bench
CODE_PANEL_STENCIL_BENCH_ARGS ?=

ifeq ($(UNAME_S),Darwin)
$(CODE_PANEL_STENCIL_BENCH_BIN): $(CODE_PANEL_STENCIL_BENCH_SRC) $(FREEGLUT_STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE -DGL_SILENCE_DEPRECATION -DFREEGLUT_STATIC \
	  -I$(FREEGLUT_INCLUDE_DIR) $< \
	  $(FREEGLUT_STATIC_LIB) -lm \
	  -framework IOKit -framework Cocoa -framework OpenGL -framework CoreVideo -o $@

bench-code-panel-stencil: $(CODE_PANEL_STENCIL_BENCH_BIN) ## Benchmark stencil-routed vs per-span-glColor code-panel text (Linux/macOS; opens a short-lived window).
	$(CODE_PANEL_STENCIL_BENCH_BIN) $(CODE_PANEL_STENCIL_BENCH_ARGS) $(BENCH_ARGS)
else ifeq ($(UNAME_S),Linux)
ifeq ($(FREEGLUT_VENDOR_LINUX),1)
$(CODE_PANEL_STENCIL_BENCH_BIN): $(CODE_PANEL_STENCIL_BENCH_SRC) $(FREEGLUT_STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE -DFREEGLUT_STATIC \
	  -I$(FREEGLUT_INCLUDE_DIR) $< \
	  $(FREEGLUT_STATIC_LIB) -lGL -lGLU -lX11 -lXi -lXrandr -lXxf86vm \
	  -lm -lpthread -ldl -o $@
else
$(CODE_PANEL_STENCIL_BENCH_BIN): $(CODE_PANEL_STENCIL_BENCH_SRC)
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE \
	  $< -lglut -lGL -lGLU -lm -o $@
endif

bench-code-panel-stencil: $(CODE_PANEL_STENCIL_BENCH_BIN) ## Benchmark stencil-routed vs per-span-glColor code-panel text (Linux/macOS; opens a short-lived window).
	$(CODE_PANEL_STENCIL_BENCH_BIN) $(CODE_PANEL_STENCIL_BENCH_ARGS) $(BENCH_ARGS)
else
bench-code-panel-stencil:
	@echo "ERROR: bench-code-panel-stencil targets Linux/macOS (needs freeglut)." >&2
	@exit 1
endif

# Vertex-number overlay cost benchmark (Linux/macOS). Prices the three things
# the vertex-label pass does that could each explain the ~14 ms/frame the
# profile panel charges it on NVIDIA: the full-viewport depth readback behind
# the occlusion cull, the per-glyph label submission, and the per-vertex
# glGetFloatv(GL_MODELVIEW_MATRIX) in the walk callback.
#
# Standalone rather than linked against edit_overlays.c: the finding is about
# when the readback is issued (mid-frame, with a vsync-throttled swap queue
# outstanding) rather than about what the overlay computes, so it needs a
# harness that can issue the same read both ways. See the source header.
#
# Run it twice, plain and with __GL_SYNC_TO_VBLANK=0, and compare.
VERTEX_LABEL_BENCH_SRC  := bench/bench_vertex_label_readback.c
VERTEX_LABEL_BENCH_BIN  := build/vertex-label-bench/bench
VERTEX_LABEL_BENCH_ARGS ?=

ifeq ($(UNAME_S),Darwin)
$(VERTEX_LABEL_BENCH_BIN): $(VERTEX_LABEL_BENCH_SRC) $(FREEGLUT_STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE -DGL_SILENCE_DEPRECATION -DFREEGLUT_STATIC \
	  -I$(FREEGLUT_INCLUDE_DIR) $< \
	  $(FREEGLUT_STATIC_LIB) -lm \
	  -framework IOKit -framework Cocoa -framework OpenGL -framework CoreVideo -o $@

bench-vertex-labels: $(VERTEX_LABEL_BENCH_BIN) ## Benchmark the vertex-number overlay's depth readback, glyph draw, and matrix reads (Linux/macOS; opens a short-lived window).
	$(VERTEX_LABEL_BENCH_BIN) $(VERTEX_LABEL_BENCH_ARGS) $(BENCH_ARGS)
else ifeq ($(UNAME_S),Linux)
$(VERTEX_LABEL_BENCH_BIN): $(VERTEX_LABEL_BENCH_SRC)
	@mkdir -p $(dir $@)
	$(CC) -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE \
	  $< -lglut -lGL -lGLU -lm -o $@

bench-vertex-labels: $(VERTEX_LABEL_BENCH_BIN) ## Benchmark the vertex-number overlay's depth readback, glyph draw, and matrix reads (Linux/macOS; opens a short-lived window).
	$(VERTEX_LABEL_BENCH_BIN) $(VERTEX_LABEL_BENCH_ARGS) $(BENCH_ARGS)
else
bench-vertex-labels:
	@echo "ERROR: bench-vertex-labels targets Linux/macOS (needs freeglut)." >&2
	@exit 1
endif

capacity-matrix: ## Print state-scaling matrix: per-tunable bytes-per-unit, current totals, and undo/redo ring footprint.
	@$(CC) $(COMMON_CFLAGS) tools/capacity_matrix.c -o build/capacity_matrix
	@./build/capacity_matrix

bench: $(BENCH_TARGET_NAMES) $(COMPILE_REPORT_BENCH_SUMMARY) ## Build and run the REPL runtime benchmarks.
	@for b in $(BENCH_BINS); do \
		echo "==> $$b $(BENCH_ARGS)"; \
		$(BINDIR)/$$b $(BENCH_ARGS) || exit $$?; \
	done

bench-csv: $(BENCH_TARGET_NAMES) ## Run benchmarks with --csv output (machine readable).
	@for b in $(BENCH_BINS); do \
		$(BINDIR)/$$b --csv $(BENCH_ARGS) || exit $$?; \
	done

# The same benchmarks compiled to wasm and run under node, because wasm is
# not a constant multiple of native: measured against this machine's native
# release build, per-op cost ran 1.2x on replay_long but 2.2x on
# normalize_large_doc, so the native run reorders which paths look expensive
# on the web. Numbers are only comparable web-to-web -- see the caveats in
# bench/bench_repl.c's header comment before diffing them against `make bench`.
#
# This measures the C pipeline only. It does not, and cannot, see the
# gl4es -> WebGL2 -> browser-GL cost that the app pays for real draw calls:
# node has no GPU, and fade_batches (the one sub-benchmark that emits GL)
# skips itself here. A regression that lives in the draw path will not show
# up in these numbers.
bench-web: require-emcc ## Build and run the REPL runtime benchmarks as wasm under node (web-side CPU cost only; needs emcc + node).
	@command -v node >/dev/null 2>&1 || { \
		echo "ERROR: node not found on PATH -- bench-web runs the wasm build headless under node."; \
		exit 1; \
	}
	scripts/web-deps.sh
	@for b in $(BENCH_BINS); do \
		$(MAKE) --no-print-directory WEB=1 $(WEB_BINDIR)/$$b || exit $$?; \
	done
	@for b in $(BENCH_BINS); do \
		echo "==> $$b (wasm/node) $(BENCH_ARGS)"; \
		node $(WEB_BINDIR)/$$b $(BENCH_ARGS) || exit $$?; \
	done

bench-web-csv: require-emcc ## Run the wasm benchmarks with --csv output (machine readable).
	@command -v node >/dev/null 2>&1 || { \
		echo "ERROR: node not found on PATH -- bench-web runs the wasm build headless under node."; \
		exit 1; \
	}
	@scripts/web-deps.sh >/dev/null
	@for b in $(BENCH_BINS); do \
		$(MAKE) --no-print-directory WEB=1 $(WEB_BINDIR)/$$b >/dev/null || exit $$?; \
	done
	@for b in $(BENCH_BINS); do \
		node $(WEB_BINDIR)/$$b --csv $(BENCH_ARGS) || exit $$?; \
	done

# Browser-only companion to bench-web. Unlike the node benchmarks above, this
# exercises gl4es -> WebGL2 and guards against the "fast because blank" failure
# mode. The two builds separate short-lived immediate renderlists from the
# compiled display-list cache and its append/copy/delete lifetime.
GL4ES_POLYGON_LINE_BENCH_SRC = packaging/web/bench/gl4es_polygon_line.c
GL4ES_POLYGON_LINE_BENCH_BINS = \
	$(WEB_BINDIR)/gl4es-polygon-line-immediate.html \
	$(WEB_BINDIR)/gl4es-polygon-line-display-list.html

bench-web-gl4es: require-emcc ## Build browser gl4es polygon-line immediate/display-list benchmarks.
	scripts/web-deps.sh
	$(MAKE) WEB=1 $(GL4ES_POLYGON_LINE_BENCH_BINS)
	@echo "Serve with: python3 scripts/web-serve.py $(WEB_BINDIR)"
	@echo "Immediate:   http://localhost:8000/gl4es-polygon-line-immediate.html"
	@echo "Display list: http://localhost:8000/gl4es-polygon-line-display-list.html"

ifeq ($(WEB),1)
$(WEB_BINDIR)/gl4es-polygon-line-immediate.html: $(GL4ES_POLYGON_LINE_BENCH_SRC) \
		packaging/web/gl4es_bootstrap.c $(WEB_GL_ARCHIVES)
	@mkdir -p $(dir $@)
	$(CC) $(GL_HEADER_CFLAGS) -DGL4ES_BENCH_DISPLAY_LIST=0 \
		$(GL4ES_POLYGON_LINE_BENCH_SRC) packaging/web/gl4es_bootstrap.c \
		$(WEB_GL_ARCHIVES) $(WEB_RUNTIME_LDFLAGS) -o $@

$(WEB_BINDIR)/gl4es-polygon-line-display-list.html: $(GL4ES_POLYGON_LINE_BENCH_SRC) \
		packaging/web/gl4es_bootstrap.c $(WEB_GL_ARCHIVES)
	@mkdir -p $(dir $@)
	$(CC) $(GL_HEADER_CFLAGS) -DGL4ES_BENCH_DISPLAY_LIST=1 \
		$(GL4ES_POLYGON_LINE_BENCH_SRC) packaging/web/gl4es_bootstrap.c \
		$(WEB_GL_ARCHIVES) $(WEB_RUNTIME_LDFLAGS) -o $@
endif

# count lines: $(SRCS) $(HDRS)
lines: $(SRCS) $(HDRS) ## Count SLOC (code/comment/blank) across source and header files.
	@if ! command -v cloc >/dev/null 2>&1; then \
		echo "cloc not found. Install it with:"; \
		echo "  macOS:  brew install cloc"; \
		echo "  Linux:  sudo apt install cloc"; \
		exit 1; \
	fi
	@cloc $(SRCS) $(HDRS) --by-file | awk '\
	BEGIN { \
		mods[1] = "src/support"; \
		mods[2] = "src/app"; \
		mods[3] = "src/editor"; \
		mods[5] = "src/render3d"; \
		mods[4] = "src/repl"; \
		mods[6] = "src/ui"; \
		mods[7] = "src/subsystems"; \
		mods[8] = "(root files)"; \
		for (i = 1; i <= 8; i++) { \
			files[mods[i]] = 0; \
			blank[mods[i]] = 0; \
			comment[mods[i]] = 0; \
			code[mods[i]] = 0; \
		} \
	} \
	{ \
		print $$0; \
	} \
	NF == 4 && $$1 != "SUM:" && $$2 ~ /^[0-9]+$$/ && $$3 ~ /^[0-9]+$$/ && $$4 ~ /^[0-9]+$$/ { \
		fn = $$1; \
		m = ""; \
		if (fn ~ /^src\/support\//) m = "src/support"; \
		else if (fn ~ /^src\/app\//) m = "src/app"; \
		else if (fn ~ /^src\/editor\//) m = "src/editor"; \
		else if (fn ~ /^src\/render3d\//) m = "src/render3d"; \
		else if (fn ~ /^src\/repl\//) m = "src/repl"; \
		else if (fn ~ /^src\/ui\//) m = "src/ui"; \
		else if (fn ~ /^src\/subsystems\//) m = "src/subsystems"; \
		else m = "(root files)"; \
		files[m]++; \
		blank[m] += $$2; \
		comment[m] += $$3; \
		code[m] += $$4; \
	} \
	END { \
		print ""; \
		print "==============================================================================="; \
		print "                           METRICS SUMMARY BY MODULE"; \
		print "==============================================================================="; \
		printf "%-25s %10s %10s %10s %10s\n", "Module", "Files", "Blank", "Comment", "Code"; \
		printf "%-25s %10s %10s %10s %10s\n", "-------------------------", "----------", "----------", "----------", "----------"; \
		for (i = 1; i <= 8; i++) { \
			m = mods[i]; \
			printf "%-25s %10d %10d %10d %10d\n", m, files[m], blank[m], comment[m], code[m]; \
		} \
	}'

# count lines: test sources + shared test helpers
TEST_SLOC_SRCS = $(wildcard tests/*.c tests/*.h tests/support/*.c tests/support/*.h)
lines-test: $(TEST_SLOC_SRCS) ## Count SLOC (code/comment/blank) across test sources.
	@if ! command -v cloc >/dev/null 2>&1; then \
		echo "cloc not found. Install it with:"; \
		echo "  macOS:  brew install cloc"; \
		echo "  Linux:  sudo apt install cloc"; \
		exit 1; \
	fi
	@cloc $(TEST_SLOC_SRCS) --by-file

debug: ## Build everything with debug ASan+UBSan flags.
	$(MAKE) all BUILD=debug

debug-msan: ## Build everything with debug MemorySanitizer flags.
ifeq ($(UNAME_S),Darwin)
	@printf "WARNING: MemorySanitizer is not supported on macOS (Darwin). Skipping debug-msan.\n" >&2
else
	$(MAKE) all BUILD=debug SAN=memory CC=$(MSAN_CC)
endif

coverage: ## Clean, rebuild tests with coverage, run suite, generate HTML report.
	$(MAKE) clean
	$(MAKE) internal-test-suite BUILD=coverage TEST_JOBS=1 USE_GL_STUBS=1
	mkdir -p build/coverage-gl-stubs
	lcov --capture \
		--directory build/coverage-gl-stubs \
		--output-file build/coverage-gl-stubs/lcov.info \
		--ignore-errors mismatch,empty \
		--exclude '*/test_*.c' \
		--exclude '*/miniaudio.h' \
		--rc branch_coverage=1
	genhtml build/coverage-gl-stubs/lcov.info \
		--output-directory build/coverage-gl-stubs/html \
		--branch-coverage \
		--title "REPL coverage" \
		--ignore-errors inconsistent
	@echo "Coverage report: build/coverage-gl-stubs/html/index.html"

SANITIZER_CHECKERS ?= core,deadcode,unix,cplusplus,osx
# Files to exclude from static analysis (e.g., third-party library includes)
ANALYZE_EXCLUDE ?= src/app/glr_audio.c
ANALYZE_SRCS = $(filter-out $(ANALYZE_EXCLUDE),$(SRCS))

analyze: ## Run static analyzer (clang on macOS, gcc on Linux).
ifeq ($(UNAME_S),Darwin)
	@echo "Running clang static analyzer (macOS)..."
	@for src in $(ANALYZE_SRCS); do \
		echo "Analyzing $$src..."; \
		$(CC) $(OBJ_CFLAGS) --analyze -Xanalyzer -analyzer-output=text -Xclang -analyzer-checker=$(SANITIZER_CHECKERS) $$src || true; \
	done
else
	@echo "Running gcc static analyzer (Linux)..."
	@for src in $(ANALYZE_SRCS); do \
		echo "Analyzing $$src..."; \
		$(CC) $(OBJ_CFLAGS) -fanalyzer -S -o /dev/null $$src || true; \
	done
endif

clean: ## Remove built binaries and object files.
	rm -rf $(ROOT_BIN_LINKS) gl-repl.dSYM gl-repl-unchained.dSYM render3d_demo.dSYM render3d_hot_demo.dSYM repl_demo.dSYM repl_live_demo.dSYM editor_demo.dSYM memprof_demo.dSYM variable_panel_demo.dSYM color_picker_demo.dSYM assign_plot_demo.dSYM cpuprof_demo.dSYM \
		$(TEST_BINS) $(addsuffix .dSYM,$(TEST_BINS)) \
		$(BENCH_BINS) $(addsuffix .dSYM,$(BENCH_BINS)) \
		build/coverage/lcov.info build/coverage/html \
		build \
		gl-repl.app packaging/macos/gl-repl.icns packaging/macos/gl-repl.iconset \
		dist \
		callgraph*.mmd callgraph*.dot callgraph*.html callgrind.out*

distclean: clean freeglut-clean ## Remove all build outputs, including the vendored freeglut build.

glut: ## Rebuild using the Apple GLUT framework instead of freeglut.
	$(MAKE) all \
		BUILD="$(BUILD)" \
		CFLAGS="$(CFLAGS) -DUSE_GLUT" \
		GL_LDFLAGS="$(GLUT_GL_LDFLAGS)" \
		FREEGLUT_VENDOR=0

# glprobe ---------------------------------------------------------------------
# Link the GL_FEEDBACK geometry probe (tools/glprobe/) into a standalone
# fixed-function sample -- one of the loose .c files at the repo root, or any
# other single-TU GLUT program. The sample opts in by including "glprobe.h" and
# calling glprobe_diagnose(); this target only supplies the flags.
#
#   make glprobe SAMPLE=flame-torch.c                     # native window
#   make glprobe SAMPLE=flame-torch.c FREEGLUT_OSMESA=1   # headless, capturable
#
# The headless form additionally requires the sample to include
# tools/glprobe/glprobe_glut.h instead of its own #ifdef __APPLE__ GLUT block;
# see tools/glprobe/README.md. Binary lands at build/glprobe/<basename>.
GLPROBE_SRCS = tools/glprobe/glprobe.c src/support/mesh_ply.c
GLPROBE_DIR  = build/glprobe$(if $(filter 1,$(FREEGLUT_OSMESA)),-osmesa,)
GLPROBE_BIN  = $(GLPROBE_DIR)/$(basename $(notdir $(SAMPLE)))

# make extract SAMPLE=<file.c> -- one command, geometry out.
#
# Builds the sample and the probe library headless, runs one frame with no
# window, and drops <name>.ply and <name>.glr in the CURRENT directory. Both
# come from a SINGLE capture, so they describe the same frame; running the app
# twice would not, for anything animated.
#
# Headless is not a convenience here, it is what makes the target a target: a
# windowed run never exits on its own, so it could not be a build step.
#
# Optional: BATCH=<n> extracts one object, MAX_TRIS=<n> caps the count, FRAME=<n>
# picks a later frame (useful when frame 1 catches an animation mid-setup).
.PHONY: extract
extract: ## Extract geometry from a standalone GL sample: make extract SAMPLE=<file.c> -> <name>.ply + <name>.glr
	@test -n "$(SAMPLE)" || { \
		echo "$(RED)usage: make extract SAMPLE=<sample.c> [BATCH=n] [MAX_TRIS=n] [FRAME=n]$(NC)"; \
		exit 1; }
	@test -f "$(SAMPLE)" || { echo "$(RED)no such sample: $(SAMPLE)$(NC)"; exit 1; }
	@$(MAKE) --no-print-directory glprobe glprobe-preload \
		SAMPLE="$(SAMPLE)" FREEGLUT_OSMESA=1 >/dev/null
	@GLPROBE_EXTRACT="$(CURDIR)/$(basename $(notdir $(SAMPLE)))" \
	 $(if $(BATCH),GLPROBE_BATCH=$(BATCH),) \
	 $(if $(MAX_TRIS),GLPROBE_MAX_TRIS=$(MAX_TRIS),) \
	 $(if $(FRAME),GLPROBE_FRAME=$(FRAME),) \
	 DYLD_INSERT_LIBRARIES="$(CURDIR)/build/glprobe-osmesa/libglprobe_preload.$(GLPROBE_PRELOAD_EXT)" \
	 LD_PRELOAD="$(CURDIR)/build/glprobe-osmesa/libglprobe_preload.$(GLPROBE_PRELOAD_EXT)" \
	 FREEGLUT_CAPTURE_FRAMES=$(if $(FRAME),$(FRAME),1) \
	 $(CURDIR)/build/glprobe-osmesa/$(basename $(notdir $(SAMPLE))) 2>&1 \
	 | grep -v '^freeglut' || true
	@rm -f freeglut-*.ppm

# The same probe as an injectable library, for samples whose source must not be
# touched. Interposes glutDisplayFunc to get a frame hook; see the header of
# tools/glprobe/glprobe_preload.c.
#
#   make glprobe-preload
#   DYLD_INSERT_LIBRARIES=build/glprobe/libglprobe_preload.dylib GLPROBE=1 ./sample
#
# INTERPOSITION NEEDS A DYNAMIC GLUT. The default macOS link is the Apple GLUT
# framework and Linux uses -lglut, both fine. FREEGLUT_OSMESA=1 normally links
# the vendored freeglut as a STATIC archive, which leaves no glutDisplayFunc
# symbol to interpose -- so the headless variant builds and links a SHARED
# freeglut instead (separate CMake dir, gitignored like the others).
#
# Both glprobe targets share these flags. The `glprobe` sample build has no need
# of a dynamic GLUT itself, but a sample linked the other way cannot be probed
# by the preload library, and a debugging convenience target that quietly
# produces an unprobeable binary is worse than one extra CMake directory.
GLPROBE_PRELOAD_EXT  = $(if $(filter Darwin,$(UNAME_S)),dylib,so)
GLPROBE_PRELOAD_LIB  = $(GLPROBE_DIR)/libglprobe_preload.$(GLPROBE_PRELOAD_EXT)
GLPROBE_PRELOAD_SRCS = tools/glprobe/glprobe_preload.c \
                       tools/glprobe/glprobe_extract.c $(GLPROBE_SRCS)

# Headless only: redirect a sample's <GLUT/glut.h> (the Apple-framework branch
# of its own #ifdef) to the glprobe shim, so an untouched Apple-style sample
# builds against OSMesa. Must precede the SDK on the include path, hence first.
# Never set for a native build, where the real framework is the right answer.
GLPROBE_COMPAT_CFLAGS = $(if $(filter 1,$(FREEGLUT_OSMESA)),-Itools/glprobe/compat,)

ifeq ($(FREEGLUT_OSMESA),1)
  FREEGLUT_SHARED_BUILD := $(FREEGLUT_SRC)/build-osmesa-shared
  FREEGLUT_SHARED_LIB   := $(FREEGLUT_SHARED_BUILD)/lib/libglut_osmesa.$(GLPROBE_PRELOAD_EXT)
  GLPROBE_GLUT_LDFLAGS = -L$(FREEGLUT_SHARED_BUILD)/lib -lglut_osmesa \
	-Wl,-rpath,$(abspath $(FREEGLUT_SHARED_BUILD)/lib) \
	-L$(MESA_PREFIX)/lib -lOSMesa -lGL -L$(MESA_GLU_PREFIX)/lib -lGLU \
	-Wl,-rpath,$(MESA_PREFIX)/lib -Wl,-rpath,$(MESA_GLU_PREFIX)/lib -lm
  GLPROBE_GLUT_DEPS = $(FREEGLUT_SHARED_LIB)
else
  GLPROBE_GLUT_LDFLAGS = $(GLUT_GL_LDFLAGS)
  GLPROBE_GLUT_DEPS =
endif

$(FREEGLUT_SHARED_LIB): $(FREEGLUT_SRC)/VENDORED.txt
	PKG_CONFIG_PATH="$(FREEGLUT_PKG_CONFIG_PATH):$$PKG_CONFIG_PATH" \
	cmake -S $(FREEGLUT_SRC) -B $(FREEGLUT_SHARED_BUILD) \
	  $(FREEGLUT_CMAKE_BACKEND) -DFREEGLUT_BUILD_STATIC_LIBS=OFF \
	  -DFREEGLUT_BUILD_SHARED_LIBS=ON -DFREEGLUT_BUILD_DEMOS=OFF \
	  -DCMAKE_BUILD_TYPE=Release
	cmake --build $(FREEGLUT_SHARED_BUILD) --target freeglut

.PHONY: glprobe
glprobe: $(GLPROBE_GLUT_DEPS) ## Build a standalone GL sample with the GL_FEEDBACK geometry probe (SAMPLE=<file.c>).
	@test -n "$(SAMPLE)" || { \
		echo "$(RED)usage: make glprobe SAMPLE=<sample.c> [FREEGLUT_OSMESA=1]$(NC)"; \
		exit 1; }
	@test -f "$(SAMPLE)" || { echo "$(RED)no such sample: $(SAMPLE)$(NC)"; exit 1; }
	@mkdir -p $(GLPROBE_DIR)
	$(CC) -std=c99 -g -O1 -Wno-deprecated-declarations \
		$(GLPROBE_COMPAT_CFLAGS) $(GL_HEADER_CFLAGS) \
		-Itools/glprobe -Isrc \
		-o $(GLPROBE_BIN) $(SAMPLE) $(GLPROBE_SRCS) $(GLPROBE_GLUT_LDFLAGS)
	@echo "$(GREEN)built $(GLPROBE_BIN)$(NC)"

.PHONY: glprobe-preload
glprobe-preload: $(GLPROBE_GLUT_DEPS) ## Build the glprobe probe as an LD_PRELOAD/DYLD_INSERT_LIBRARIES library (no sample source changes).
	@mkdir -p $(GLPROBE_DIR)
	$(CC) -std=c99 -g -O1 -Wno-deprecated-declarations -shared -fPIC \
		$(GL_HEADER_CFLAGS) -Itools/glprobe -Isrc \
		-o $(GLPROBE_PRELOAD_LIB) $(GLPROBE_PRELOAD_SRCS) \
		$(GLPROBE_GLUT_LDFLAGS)
	@echo "$(GREEN)built $(GLPROBE_PRELOAD_LIB)$(NC)"

# Call graph generation targets -----------------------------------------------

callgraph-static: ## Generate static call graph using cflow -> Mermaid diagram.
	@if ! command -v cflow &> /dev/null; then \
		echo "ERROR: cflow not found. Install with: brew install cflow"; exit 1; \
	fi
	@echo "Generating static call graph from $(SRCS)..."
	@cflow $(SRCS) 2>/dev/null | python3 scripts/cflow_to_mermaid.py > callgraph-static.mmd
	@echo "Call graph saved to callgraph-static.mmd"
	@echo "Visualize at: https://mermaid.live or with: npx @mermaid-js/mermaid-cli"

callgraph-static-entry: ## Generate call graph from specific entry point (ENTRY=function_name).
	@if ! command -v cflow &> /dev/null; then \
		echo "ERROR: cflow not found. Install with: brew install cflow"; exit 1; \
	fi
	@if [ -z "$(ENTRY)" ]; then \
		echo "ERROR: specify entry point with ENTRY=function_name"; \
		echo "  Example: make callgraph-static-entry ENTRY=imrepl_ctrl_display_frame"; exit 1; \
	fi
	@echo "Generating static call graph from $(ENTRY)..."
	@cflow -m $(ENTRY) $(SRCS) 2>/dev/null | python3 scripts/cflow_to_mermaid.py > callgraph-$(ENTRY).mmd
	@echo "Call graph saved to callgraph-$(ENTRY).mmd"

callgraph-profile: gl-repl ## Generate profile-based call graph using Valgrind callgrind.
	@if ! command -v valgrind &> /dev/null; then \
		echo "ERROR: valgrind not found. Install with: brew install valgrind"; exit 1; \
	fi
	@if ! command -v callgrind_annotate &> /dev/null; then \
		echo "ERROR: callgrind_annotate not found (part of valgrind)"; exit 1; \
	fi
	@if [ -z "$(PROG)" ]; then \
		echo "Running gl-repl with no args for default 5 seconds..."; \
		PROG="./gl-repl"; \
		timeout 5 valgrind --tool=callgrind --callgrind-out-file=callgrind.out $$PROG 2>/dev/null || true; \
	else \
		echo "Running: $$PROG"; \
		valgrind --tool=callgrind --callgrind-out-file=callgrind.out $$PROG 2>/dev/null; \
	fi
	@if [ -f callgrind.out ]; then \
		callgrind_annotate callgrind.out 2>/dev/null | python3 scripts/callgrind_to_mermaid.py > callgraph-profile.mmd; \
		echo "Profile-based call graph saved to callgraph-profile.mmd"; \
	else \
		echo "ERROR: callgrind.out not generated"; exit 1; \
	fi

callgraph-graphviz: ## Generate Graphviz DOT format (better for large graphs). Use: make callgraph-graphviz [ENTRY=function_name]
	@if ! command -v cflow &> /dev/null; then \
		echo "ERROR: cflow not found. Install with: brew install cflow"; exit 1; \
	fi
	@if [ -n "$(ENTRY)" ]; then \
		echo "Generating Graphviz call graph from $(ENTRY)..."; \
		cflow -m $(ENTRY) $(SRCS) 2>/dev/null | python3 scripts/cflow_to_graphviz.py --no-stdlib > callgraph-$(ENTRY).dot; \
		echo "Graphviz DOT saved to callgraph-$(ENTRY).dot"; \
	else \
		echo "Generating Graphviz call graph from all functions..."; \
		cflow $(SRCS) 2>/dev/null | python3 scripts/cflow_to_graphviz.py --no-stdlib > callgraph-full.dot; \
		echo "Graphviz DOT saved to callgraph-full.dot"; \
	fi
	@echo "Render with: dot -Tsvg callgraph-*.dot -o callgraph.svg"
	@echo "Or for better layout: neato -Tsvg callgraph-*.dot -o callgraph.svg"
	@echo "Or: sfdp -Tsvg callgraph-*.dot -o callgraph.svg (scalable force-directed)"

callgraph-html: ## Generate interactive Cytoscape.js HTML (no size limits, searchable, filterable).
	@if ! command -v cflow &> /dev/null; then \
		echo "ERROR: cflow not found. Install with: brew install cflow"; exit 1; \
	fi
	@if [ -n "$(ENTRY)" ]; then \
		echo "Generating interactive HTML from $(ENTRY)..."; \
		cflow -m $(ENTRY) $(SRCS) 2>/dev/null | python3 scripts/cflow_to_cytoscape_html.py --no-stdlib --no-gl > callgraph-$(ENTRY).html; \
		echo "Interactive graph saved to callgraph-$(ENTRY).html"; \
	else \
		echo "Generating interactive HTML from all functions..."; \
		cflow $(SRCS) 2>/dev/null | python3 scripts/cflow_to_cytoscape_html.py --no-stdlib --no-gl > callgraph-full.html; \
		echo "Interactive graph saved to callgraph-full.html"; \
	fi
	@echo "Open in browser: open callgraph-*.html"

callgraph-files: ## Generate file-level Mermaid dependency graph (optional ENTRY=function_name, CALLGRAPH_FILES_GROUP_CONFIG=path).
	@if ! command -v cflow &> /dev/null; then \
		echo "ERROR: cflow not found. Install with: brew install cflow"; exit 1; \
	fi
	@if [ -n "$(ENTRY)" ]; then \
		echo "Generating file-level Mermaid graph from $(ENTRY)..."; \
		cflow -m $(ENTRY) $(SRCS) 2>/dev/null | python3 scripts/cflow_to_file_mermaid.py --no-stdlib --no-gl --group-config "$(if $(CALLGRAPH_FILES_GROUP_CONFIG),$(CALLGRAPH_FILES_GROUP_CONFIG),scripts/callgraph_file_groups.json)" > callgraph-files-$(ENTRY).mmd; \
		echo "File-level graph saved to callgraph-files-$(ENTRY).mmd"; \
	else \
		echo "Generating file-level Mermaid graph from all reachable functions..."; \
		cflow $(SRCS) 2>/dev/null | python3 scripts/cflow_to_file_mermaid.py --no-stdlib --no-gl --group-config "$(if $(CALLGRAPH_FILES_GROUP_CONFIG),$(CALLGRAPH_FILES_GROUP_CONFIG),scripts/callgraph_file_groups.json)" > callgraph-files.mmd; \
		echo "File-level graph saved to callgraph-files.mmd"; \
	fi
	@echo "Visualize at: https://mermaid.live or with: npx @mermaid-js/mermaid-cli"

help: ## Show the common targets (run make help-details for the full list).
	@printf "Immediate-mode REPL - common Make targets\n\n"
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / {d[$$1]=$$2} \
		END {split("gl-repl clean test test-full fetch-music help help-details",o," "); \
		for (i=1;i<=7;i++) printf "  %-16s %s\n", o[i], d[o[i]]}' $(MAKEFILE_LIST)
	@printf "\nRun 'make help-details' for all targets, build modes, and runtime/env notes.\n"

help-details: ## Show available targets and build-mode notes.
	@printf "Immediate-mode REPL Make targets\n\n"
	@printf "Build modes:\n"
	@printf "  common flags:  %s\n" "$(COMMON_CFLAGS)" | fold -s -w 100 | sed '1!s/^/                 /'
	@printf "  default:       \$$(common_flags) %s \n" "$(filter-out $(COMMON_CFLAGS),$(RELEASE_CFLAGS))"
	@printf "  quick:         \$$(common_flags) %s \n" "$(filter-out $(COMMON_CFLAGS),$(QUICK_CFLAGS))"
	@printf "  debug:         \$$(common_flags) %s \n" "$(filter-out $(COMMON_CFLAGS),$(DEBUG_CFLAGS))"
	@printf "  coverage:      \$$(common_flags) %s \n\n" "$(filter-out $(COMMON_CFLAGS),$(COVERAGE_CFLAGS))"
	@printf "GL stubs:        make test (or test-stubs); ordinary individual tests use stubs automatically.\n"
	@printf "Web build:       make web (or scripts/build-web.sh for a cold start with no\n"
	@printf "                 emsdk sourced yet), then make web-serve. See packaging/web/README.md.\n"
	@printf "Runtime env:\n"
	@printf "  - GLR_NO_POINT_PARAMETER=1 ./gl-repl forces the no-glPointParameterfv\n"
	@printf "    path (camera-distance glPointSize fallback). Support is otherwise\n"
	@printf "    auto-detected from the GL context at startup; there is no build\n"
	@printf "    flag. See docs/ARCHITECTURE.md > Core Subsystem Features & Integrations > Runtime GL Capability Detection.\n"
	@printf "  - GLR_NO_GPU_PROF=1 ./gl-repl disables the GPU timer queries behind\n"
	@printf "    the profile panel's GPU column (the column reads \"--\"). Otherwise\n"
	@printf "    auto-detected at startup: GL_ARB_timer_query / GL 3.3 timestamps\n"
	@printf "    preferred (additive), GL_EXT_timer_query elapsed brackets as the\n"
	@printf "    fallback (Apple GL 2.1); no build flag. Same doc section.\n"
	@printf "  - GLR_AUDIO_HITCH_MS=N ./gl-repl sets the audio-worker hitch\n"
	@printf "    threshold (default 50ms; 0 disables). --no-audio skips audio\n"
	@printf "    init to isolate startup stalls; startup prints an [init +Ns]\n"
	@printf "    trace per phase.\n"
	@printf "  - GLR_PROF_DUMP=N ./gl-repl prints the profile panel's rows to\n"
	@printf "    stderr every N frames (running averages, ms, indented by\n"
	@printf "    section depth) - the panel in a form a shell loop can diff, so\n"
	@printf "    a feature can be A/B'd without a window or a screenshot.\n"
	@printf "    GLR_PROF_DUMP_MIN_MS sets the row cutoff (default 0.02; 0 prints\n"
	@printf "    every section). Read the rows as attribution, not work: a section\n"
	@printf "    holding a synchronous GL readback absorbs the driver's pipeline\n"
	@printf "    drain, which on a render-ahead driver is most of a refresh\n"
	@printf "    interval. If a row lands near the vsync period, re-run with\n"
	@printf "    __GL_SYNC_TO_VBLANK=0 before believing it; make bench-vertex-labels\n"
	@printf "    reproduces both sides. See docs/ADVANCED_USAGE.md > Diagnostics.\n"
	@printf "  - GLR_DETAILED_PROF=1 ./gl-repl (or --detailed-prof) promotes\n"
	@printf "    the optional fine-grained init-trace phases (glutInit split,\n"
	@printf "    audio playlist sub-steps, first-two-frames triple); default\n"
	@printf "    off. See docs/ARCHITECTURE.md > Core Subsystem Features & Integrations > Startup & Audio-Worker Diagnostics.\n"
	@printf "Build options:\n"
	@printf "  - UI_THEME_DEFAULT=N picks the compile-time UI color scheme\n"
	@printf "    (0 green default, 1 warm, 2 cyan, 3 amber, 4 violet, 5 mono),\n"
	@printf "    e.g. make gl-repl CFLAGS=-DUI_THEME_DEFAULT=1. Defined in\n"
	@printf "    config.h, range-checked in src/ui/core/theme.h. See\n"
	@printf "    docs/ARCHITECTURE.md > UI Color Theming.\n"
	@printf "  - SAN=memory selects MemorySanitizer for debug builds (separate build/debug-msan dir).\n"
	@printf "    make debug-msan builds the full target set with SAN=memory CC=$(MSAN_CC).\n"
	@printf "    make test-msan runs the stubbed test suite with SAN=memory CC=$(MSAN_CC).\n"
	@printf "  - NO_SAN=1 (or NOSAN=1/ASAN=0) disables debug-build sanitizers.\n"
	@printf "  - FREEGLUT_VENDOR_LINUX=1 (Linux, windowed) links the vendored static\n"
	@printf "    freeglut (X11/GLX) instead of the distro's -lglut, e.g.\n"
	@printf "    make gl-repl FREEGLUT_VENDOR_LINUX=1. Needed for frame capture:\n"
	@printf "    system freeglut has no capture hooks, so SIGUSR1 screenshots,\n"
	@printf "    FREEGLUT_CAPTURE_FRAMES/_STREAM and the docs-assets/record-gif/\n"
	@printf "    record-video scripts silently produce nothing without it. Needs\n"
	@printf "    cmake + X11/GL dev headers; separate build/*-fgvendor objdir, so it\n"
	@printf "    coexists with the default build. macOS always vendors and the Linux\n"
	@printf "    OSMesa build (FREEGLUT_OSMESA=1) vendors unconditionally; neither\n"
	@printf "    needs this flag. See docs/ADVANCED_USAGE.md > Windowed capture on Linux.\n"
	@printf "  - FREEGLUT_LIB_PATH=<path/to/libglut.a> links an external static\n"
	@printf "    freeglut instead of the vendored one - for trying a different\n"
	@printf "    fork/branch without re-vendoring. Pair it with\n"
	@printf "    FREEGLUT_INCLUDE_DIR=<path/to/include> whenever that build's\n"
	@printf "    <GL/freeglut.h> differs from the vendored header (upstream\n"
	@printf "    freeglut, which lacks the capture declarations, is exactly that),\n"
	@printf "    e.g. make gl-repl FREEGLUT_LIB_PATH=~/src/freeglut/build/lib/libglut.a\n"
	@printf "    FREEGLUT_INCLUDE_DIR=~/src/freeglut/include. Nothing is built from\n"
	@printf "    third_party/freeglut; separate build/*-fgext objdir. On Linux it\n"
	@printf "    implies FREEGLUT_VENDOR_LINUX=1 (archive by path, no -lglut).\n"
	@printf "    See docs/ADVANCED_USAGE.md > External freeglut.\n"
	@printf "  - GLR_AUDIO_NO_THREAD=1 (e.g. make gl-repl CFLAGS=-DGLR_AUDIO_NO_THREAD=1)\n"
	@printf "    drops the audio background worker thread: the playlist lifecycle ops\n"
	@printf "    (file open/uninit, state save) run synchronously, drained from\n"
	@printf "    glr_audio_tick() on the caller. Auto-enabled on Emscripten (no\n"
	@printf "    -pthread); set =0 to force the thread on. The toggle is contained\n"
	@printf "    entirely in src/app/glr_audio.c.\n"
	@printf "Build output:    concise timed lines per compiled/linked file plus the longest build steps.\n"
	@printf "                 V=1 (or VERBOSE=1) restores each compiler/linker command and its output.\n"
	@printf "User CFLAGS are appended to the selected build mode.\n\n"
	@printf "Tests:           make test runs the headless stub suite; set TEST_JOBS=N to limit jobs.\n\n"
	@printf "Individual tests can be built with make test-eval, or built and run with\n"
	@printf "                 make run-test-eval. Pass arguments with TEST_ARGS, e.g.\n"
	@printf "                 make run-test-repl-core-examples TEST_ARGS='--show-mismatch'.\n\n"
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / && $$1 !~ /^check-/ {printf "  %-24s %s\n", $$1, $$2}' $(MAKEFILE_LIST) | sort

# Keep procedural targets phony without maintaining a second copy of every
# check-* rule. The Makefile already uses this source-driven approach for help.
CHECK_PHONY_TARGETS := $(sort $(shell awk -F: '/^check-[[:alnum:]_.-]+:/ {print $$1}' $(firstword $(MAKEFILE_LIST))))

.PHONY: $(BUILD_TARGETS) $(PACKAGE_TARGETS) $(TEST_TARGETS) \
	$(BENCH_TARGETS) $(MAINTENANCE_TARGETS) $(CHECK_PHONY_TARGETS)

-include $(DEPS)
