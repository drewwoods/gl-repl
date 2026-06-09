# Recipes use bash features (`set -o pipefail`, `$'...'` ANSI-C
# quoting for colorized check output). Without this, GNU make runs
# recipes via /bin/sh — which is dash on Debian/Ubuntu — and
# `set -e -o pipefail` aborts with "Illegal option -o pipefail",
# breaking check-state-ownership / test-stubs on Linux. bash is
# present on every supported dev box (macOS /bin/bash, Linux
# /bin/bash); pin it so recipe behavior is identical everywhere.
SHELL := /bin/bash

CC = gcc
PROJECT_ROOT := $(abspath .)
LOCAL_INCLUDE := $(abspath include)
SRC_DIR := $(abspath src)
GL_STUB_INCLUDE := $(abspath tests/gl-stubs/include)
TEST_DIR := tests
BENCH_DIR := bench

# Parallel builds by default, but not too aggressively
ifeq ($(filter -j%,$(MAKEFLAGS)),)
MAKEFLAGS += -j3
endif

# Color codes for output. ESC holds a real escape byte (not the two-char
# "\033" text) so plain `echo` in recipes — macOS /bin/sh echo does not
# interpret backslash escapes — emits real color, not literal "\033[...".
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
# native macOS Cocoa backend and linked into the GL binaries. macOS-only —
# Linux keeps the system freeglut path. Re-vendor with scripts/vendor-freeglut.sh;
# the pinned commit is recorded in third_party/freeglut/VENDORED.txt.
FREEGLUT_SRC        := third_party/freeglut
# `make glut` (Apple GLUT framework fallback) passes FREEGLUT_VENDOR=0 to skip
# building/linking the vendored library.
FREEGLUT_VENDOR     ?= 1

# FREEGLUT_OSMESA=1 builds the vendored freeglut with its headless OSMesa
# (off-screen software) backend instead of the macOS Cocoa backend, and links
# the GL binaries against Mesa's libGL/libGLU + libOSMesa rather than Apple's
# OpenGL framework. This gives a windowless build that renders through swrast —
# usable for headless geometry/feedback tests (PLY export, the real-GL tests)
# with no display. The OSMesa backend lives in the vendored tree only after
# re-vendoring from a freeglut that carries it (see VENDORED.txt). Build dir and
# static-lib name are kept distinct from the Cocoa build so the two coexist.
FREEGLUT_OSMESA     ?= 0
ifeq ($(FREEGLUT_OSMESA),1)
FREEGLUT_BUILD          := $(FREEGLUT_SRC)/build-osmesa
FREEGLUT_STATIC_LIB     := $(FREEGLUT_BUILD)/lib/libglut_osmesa.a
FREEGLUT_CMAKE_BACKEND  := -DFREEGLUT_OSMESA=ON -DFREEGLUT_GLES=OFF
else
FREEGLUT_BUILD          := $(FREEGLUT_SRC)/build
FREEGLUT_STATIC_LIB     := $(FREEGLUT_BUILD)/lib/libglut.a
FREEGLUT_CMAKE_BACKEND  := -DFREEGLUT_COCOA=ON
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

# Language standard: C99, project-wide, no exceptions. Everything
# (gl-repl, tests, demos, bench, CI) compiles -std=c99 so the project
# runs on old machines / old GCC. Non-pedantic by default — GNU
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
COMMON_CFLAGS = \
	-Wall -ggdb -g3 \
	-Wno-deprecated-declarations -Wfloat-conversion \
	-std=c99 -D_GNU_SOURCE -Werror=implicit-function-declaration \
	-DGL_SILENCE_DEPRECATION -DFREEGLUT_STATIC \
	$(GL_HEADER_CFLAGS) \
	-I$(PROJECT_ROOT) \
	-I$(SRC_DIR) \
	-I$(LOCAL_INCLUDE)

RELEASE_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O2

ifeq ($(NOSAN),1)
NO_SAN := 1
endif

ifeq ($(NO_SAN),1)
DEBUG_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O0
else
DEBUG_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O0 \
	-fsanitize=address -fno-omit-frame-pointer \
	-fsanitize=undefined -fno-sanitize-recover=undefined
endif

COVERAGE_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O0 \
	--coverage -fprofile-arcs -ftest-coverage

# Test targets build & run under the debug sanitizer build
# (AddressSanitizer + UBSan) by default; gl-repl, bench and the demos
# stay release. An explicit `BUILD=...` on the command line or in the
# environment always wins, so `make coverage` (BUILD=coverage) and
# `make test BUILD=release` (fast unsanitized run) keep working.
ifeq ($(origin BUILD),command line)
# explicit BUILD — honor it
else ifeq ($(origin BUILD),environment)
# explicit BUILD — honor it
else ifneq ($(filter test test-detailed test-stubs test-full,$(MAKECMDGOALS)),)
BUILD := debug
else
BUILD := release
endif

ifeq ($(BUILD),debug)
BUILD_CFLAGS = $(DEBUG_CFLAGS)
else ifeq ($(BUILD),coverage)
BUILD_CFLAGS = $(COVERAGE_CFLAGS)
else
BUILD_CFLAGS = $(RELEASE_CFLAGS)
endif

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
FREEGLUT_HEADER_CFLAGS = -I$(FREEGLUT_SRC)/include -I$(MESA_PREFIX)/include -I$(MESA_GLU_PREFIX)/include
ifeq ($(FREEGLUT_VENDOR),1)
FREEGLUT_LIB := $(FREEGLUT_STATIC_LIB)
endif

OSMESA_GL_LDFLAGS = \
	$(FREEGLUT_LIB) \
	-L$(MESA_PREFIX)/lib -lGL -lOSMesa -L$(MESA_GLU_PREFIX)/lib -lGLU \
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
FREEGLUT_HEADER_CFLAGS = -I$(FREEGLUT_SRC)/include
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
FREEGLUT_HEADER_CFLAGS = -I$(FREEGLUT_SRC)/include
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
# Linux: system freeglut + GL/GLU. miniaudio dlopen()s pulseaudio/alsa
# at runtime, so we only need -ldl (plus the existing -lpthread -lm).
# No vendoring on Linux — system <GL/freeglut.h> is on the default include path.
FREEGLUT_HEADER_CFLAGS =
GLUT_GL_LDFLAGS = \
	-lglut -lGL -lGLU -lm -lpthread -ldl

GL_LDFLAGS = \
	-lglut -lGL -lGLU -lm -lpthread -ldl

GL_STUB_LDFLAGS = \
	-lm -lpthread -ldl
endif
endif

ifeq ($(USE_GL_STUBS),1)
GLUT_GL_LDFLAGS = $(GL_STUB_LDFLAGS)
GL_LDFLAGS = $(GL_STUB_LDFLAGS)
endif

ifeq ($(BUILD),coverage)
COVERAGE_LDFLAGS = --coverage
else
COVERAGE_LDFLAGS =
endif

.PHONY: \
	all \
	app \
	icon-regen \
	icon-cube \
	icon-cube-strong \
	audit-editor-ownership \
	bench \
	bench-csv \
	callgraph-files \
	callgraph-graphviz \
	callgraph-html \
	callgraph-profile \
	callgraph-static \
	callgraph-static-entry \
	check \
	check-c99 \
	check-color-picker-ui-isolation \
	check-controller-boundaries \
	check-domain-owner-encapsulation \
	check-duplicate-api-decls \
	check-edit-ops-pure \
	check-editor-ownership-budget \
	check-editor-repl-surface \
	check-gl-boundaries \
	check-glr-ctrl-not-editor-mirror \
	check-glr-state-no-repl-mutators \
	check-layer-coupling \
	check-module-prefixes \
	check-no-facade-include-in-views \
	check-no-feed-line-in-pipeline \
	check-no-load-line-to-input-in-pipeline \
	check-no-raw-undo-clear \
	check-no-repl-commit \
	check-no-repl-editor-input-shim \
	check-no-set-status-in-compile-apply \
	check-no-set-status-in-repl-parser \
	check-no-store-text-api \
	check-no-test-default-output \
	check-no-write-through-view \
	check-public-api-usage \
	check-public-state-no-writable-pointers \
	check-pure-scene-no-repl-state \
	check-repl-demo-no-editor \
	check-repl-demo-stubs-shrinking \
	check-repl-export-no-ui-layout \
	check-repl-export-via-bridge \
	check-repl-no-mut-reads \
	check-repl-no-direct-buffer-read \
	check-repl-no-direct-editor \
	check-repl-no-direct-tutorial-runner \
	check-repl-scenes-cfg-clear-paired \
	check-repl-state-no-glr-state \
	check-renderer-no-direct-mutators \
	check-replay-forwarders \
	check-replay-ui-isolation \
	check-runtime-state-value-fields \
	check-scene-no-repl-state-mut \
	check-source-document-port-owners \
	check-state-boundaries \
	check-state-c-shrinking \
	check-state-ownership \
	check-tier-c-function-size \
	check-trailing-whitespace \
	check-state-read-getters-return-values \
	check-ui-no-export-resolver \
	check-ui-no-repl-state-mut \
	check-ui-no-repl-state-read \
	check-ui-panels-no-mutators \
	check-ui-text-panel-pure \
	check-ui-renderer-takes-view \
	check-ui-returns-hits-only \
	check-variable-panel-forwarders \
	check-views-by-value-snapshot \
	check-views-flat-types \
	check-views-no-owners \
	clean \
	coverage \
	debug \
	glut \
	help \
	help-details \
	install-hooks \
	lines \
	lines-test \
	gl-repl \
	test \
	test-detailed \
	test-full \
	test-stubs \
	FORCE

all: gl-repl install-hooks

# Used to force rebuild if you list as a prerequisite, e.g. `test_eval: FORCE $(test_eval_OBJS)`.
FORCE:

SUPPORT_SRCS = \
	src/support/memprof.c \
	src/support/cpuprof.c \
	src/support/mesh_ply.c

APP_CONTROLLER_SRCS = \
	src/app/glr_audio.c \
	src/app/glr_paths.c \
	src/app/glr_mesh_export.c \
	src/app/glr_actions.c \
	src/app/glr_camera.c \
	src/app/glr_camera_export.c \
	src/app/glr_color_picker_bridge.c \
	src/app/glr_ctrl_view_transition.c \
	src/app/glr_ctrl_router.c \
	src/app/glr_completion.c \
	src/app/glr_compositor.c \
	src/app/glr_config.c \
	src/app/glr_ctrl.c \
	src/app/glr_debug.c \
	src/app/glr_prof.c \
	src/app/glr_source_document.c \
	src/app/glr_state.c

EDITOR_SRCS = \
	src/editor/clipboard.c \
	src/editor/commit.c \
	src/editor/completion.c \
	src/editor/edit_ops.c \
	src/editor/help_session.c \
	src/editor/inline_file_prompt.c \
	src/editor/inline_rename.c \
	src/editor/input.c \
	src/editor/reformat.c \
	src/editor/search.c \
	src/editor/state.c \
	src/editor/undo.c

REPL_SRCS = \
	src/repl/format.c \
	src/repl/apply.c \
	src/repl/autonormal.c \
	src/repl/cfg_baseline.c \
	src/repl/command_spec.c \
	src/repl/command_store.c \
	src/repl/compile.c \
	src/repl/core.c \
	src/repl/eval.c \
	src/repl/example_loader.c \
	src/repl/examples.c \
	src/repl/executor.c \
	src/repl/export.c \
	src/repl/flatten.c \
	src/repl/help_text.c \
	src/repl/import.c \
	src/repl/load.c \
	src/repl/parser.c \
	src/repl/scenes.c \
	src/repl/source_scope.c \
	src/repl/state.c \
	src/repl/text_helpers.c \
	src/repl/tutorials.c

SCENE_SRCS = \
	src/scene/axes.c \
	src/scene/backdrop.c \
	src/scene/postprocess_filter.c \
	src/scene/grid.c \
	src/scene/guides/geometry_guides.c \
	src/scene/guides/transform_guides.c \
	src/scene/lights.c \
	src/scene/overlays.c \
	src/scene/render.c \
	src/scene/scene_transition.c

UI_CORE_SRCS = \
	src/ui/core/tabbed_overlay.c \
	src/ui/core/text_layout.c \
	src/ui/core/text_panel.c \
	src/ui/core/text_search.c \
	src/ui/core/theme.c

UI_APP_SRCS = \
	src/ui/app/autocomplete_panel.c \
	src/ui/app/layout.c \
	src/ui/app/menu_bar.c \
	src/ui/app/numeric_swatch.c \
	src/ui/app/panels.c \
	src/ui/app/repl_code_panel.c \
	src/ui/app/scene_tabs.c \
	src/ui/app/state.c \
	src/ui/app/variable_panel_view.c \
	src/ui/app/color_swatch.c

UI_SUPPORT_SRCS = \
	src/ui/support/memprof.c \
	src/ui/support/cpuprof.c

UI_SUBSYSTEMS_SRCS = \
	src/ui/subsystems/color_picker.c \
	src/ui/subsystems/replay_hud.c \
	src/ui/subsystems/variable_panel.c

SUBSYSTEMS_SRCS = \
	src/subsystems/color_picker/color_picker_state.c \
	src/subsystems/replay/replay.c \
	src/subsystems/replay/replay_annotations.c \
	src/subsystems/replay/replay_fade.c \
	src/subsystems/replay/replay_input.c \
	src/subsystems/replay/replay_playback.c \
	src/subsystems/replay/replay_state.c \
	src/subsystems/tutorial/tutorial_runner.c \
	src/subsystems/tutorial/tutorial_animation.c \
	src/subsystems/tutorial/tutorial_match.c \
	src/subsystems/tutorial/tutorial_state.c \
	src/subsystems/variable_panel/variable_panel_drag.c \
	src/subsystems/variable_panel/variable_panel_state.c \
	src/subsystems/replay/replay_render.c \
	src/subsystems/edit_overlays/edit_overlays.c

GL_STUB_COUNTS_SRCS = \
	tests/gl-stubs/gl_stub_counts.c

SRCS = \
	$(SUPPORT_SRCS) \
	$(APP_CONTROLLER_SRCS) \
	$(EDITOR_SRCS) \
	$(REPL_SRCS) \
	$(SCENE_SRCS) \
	$(UI_CORE_SRCS) \
	$(UI_APP_SRCS) \
	$(UI_SUPPORT_SRCS) \
	$(UI_SUBSYSTEMS_SRCS) \
	$(SUBSYSTEMS_SRCS) \
	$(GL_STUB_COUNTS_SRCS) \
	gl_repl.c

HDRS = \
	src/app/glr_audio.h \
	src/repl/format.h \
	src/support/memprof.h \
	src/support/cpuprof.h \
	gl_repl.h \
	source_document.h \
	src/scene/guides/transform_utils.h \
	src/app/glr_actions.h \
	src/app/glr_camera.h \
	src/app/glr_completion.h \
	src/app/glr_compositor.h \
	src/app/glr_config.h \
	src/app/glr_ctrl.h \
	src/app/glr_debug.h \
	src/app/glr_defaults.h \
	src/app/glr_paths.h \
	src/app/glr_state.h \
	src/editor/clipboard.h \
	src/editor/commit.h \
	src/editor/completion.h \
	src/editor/help_session.h \
	src/editor/inline_file_prompt.h \
	src/editor/inline_rename.h \
	src/editor/input.h \
	src/editor/reformat.h \
	src/editor/search.h \
	src/editor/state.h \
	src/editor/undo.h \
	src/repl/apply.h \
	src/repl/cfg_baseline.h \
	src/repl/command_spec.h \
	src/repl/command_store.h \
	src/repl/compile.h \
	src/repl/catalog_tags.h \
	src/repl/core.h \
	src/repl/core_internal.h \
	src/repl/eval.h \
	src/repl/example_loader.h \
	src/repl/examples.h \
	src/repl/export.h \
	src/repl/help_text.h \
	src/repl/parser.h \
	src/repl/pipeline.h \
	src/repl/scenes.h \
	src/repl/source_scope.h \
	src/repl/state.h \
	src/repl/tutorials.h \
	src/repl/util.h \
	src/scene/axes.h \
	src/scene/backdrop.h \
	src/scene/grid.h \
	src/scene/guides/geometry_guides.h \
	src/scene/guides/guides_shared.h \
	src/scene/guides/transform_guides.h \
	src/scene/lights.h \
	src/scene/overlays.h \
	src/scene/render.h \
	src/scene/scene_transition.h \
	src/scene/render_types.h \
	src/ui/app/autocomplete_panel.h \
	src/ui/subsystems/color_picker.h \
	src/ui/app/layout.h \
	src/ui/support/memprof.h \
	src/ui/app/menu_bar.h \
	src/ui/app/numeric_swatch.h \
	src/ui/app/panels.h \
	src/ui/support/cpuprof.h \
	src/ui/app/repl_code_panel.h \
	src/ui/subsystems/replay_hud.h \
	src/ui/app/scene_tabs.h \
	src/ui/app/state.h \
	src/ui/app/state_types.h \
	src/ui/core/tabbed_overlay.h \
	src/ui/core/text_layout.h \
	src/ui/core/text_panel.h \
	src/ui/core/text_search.h \
	src/ui/core/theme.h \
	src/ui/subsystems/variable_panel.h \
	src/subsystems/color_picker/color_picker_state.h \
	src/subsystems/replay/replay_internal.h \
	src/subsystems/replay/replay.h \
	src/subsystems/replay/replay_annotations.h \
	src/subsystems/replay/replay_state.h \
	src/subsystems/tutorial/tutorial_internal.h \
	src/subsystems/tutorial/tutorial.h \
	src/subsystems/tutorial/tutorial_state.h \
	src/subsystems/variable_panel/variable_panel_drag.h \
	src/subsystems/variable_panel/variable_panel_state.h \
	src/subsystems/replay/replay_render.h \
	src/subsystems/edit_overlays/edit_overlays.h

UI_SRCS = $(UI_CORE_SRCS) $(UI_APP_SRCS)
SCENE_HDRS = $(filter src/scene/%.h,$(HDRS))
UI_HDRS = $(filter src/ui/core/%.h src/ui/app/%.h,$(HDRS))
STATE_NEUTRAL_SRCS = src/repl/format.c src/support/memprof.c src/support/cpuprof.c tests/gl-stubs/gl_stub_counts.c

# Object lists used to build the standalone scene_demo without dragging in
# any REPL editor/controller code. Scene + prof — the scene module no
# longer touches repl_eval (replay-baseline restore is dispatched through a
# function pointer the controller installs; geometry-guide arg parsing is
# done in the controller before snapshot is built).
SCENE_DEMO_DEP_SRCS = $(SCENE_SRCS) src/support/cpuprof.c \
                      tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone repl_demo (the inverse of scene_demo:
# proves the REPL pipeline links without editor input dispatch
# (src/editor/input.c), the controller (src/app/glr_ctrl.c + glr_ctrl_router_*),
# the UI (src/ui/*, src/ui/subsystems/replay_hud.c), or — as of Phase 6 of
# feature/source-document-port.md — the editor text store
# (src/editor/state.c). Per-line text comes from
# src/editor/state.c, a standalone static line store
# implementing the source_document_* contract. After every REPL-pipeline
# → editor/UI/peer/glr_config/glr_camera/glr_state edge was routed
# through a controller-installed sink/bridge or an opaque parameter,
# tools/repl_demo/stubs.c contains zero function bodies — the demo
# links the pipeline TUs below with no stub backfill. Adding a new
# src/repl/*.c TU that pulls in an app/editor symbol is a regression and
# should be resolved at the pipeline TU instead (the
# check-repl-demo-stubs-shrinking and check-repl-no-direct-editor
# guards catch it). The dependency ledger lives in
# feature/decouple-repl-from-gl-repl-alt.md and
# feature/source-document-port.md.
REPL_DEMO_DEP_SRCS = src/repl/format.c \
				     src/support/cpuprof.c \
                     src/subsystems/replay/replay.c \
                     src/subsystems/replay/replay_fade.c \
                     src/subsystems/replay/replay_input.c \
                     src/subsystems/replay/replay_playback.c \
                     src/subsystems/replay/replay_state.c \
                     src/subsystems/tutorial/tutorial_state.c \
					 src/repl/apply.c \
                     src/repl/autonormal.c \
                     src/repl/cfg_baseline.c \
                     src/repl/command_spec.c \
                     src/repl/command_store.c \
                     src/repl/compile.c \
                     src/repl/core.c \
                     src/repl/eval.c \
                     src/repl/example_loader.c \
                     src/repl/examples.c \
                     src/repl/executor.c \
                     src/repl/export.c \
                     src/repl/flatten.c \
                     src/repl/import.c \
                     src/repl/load.c \
                     src/repl/parser.c \
                     src/subsystems/replay/replay_annotations.c \
                     src/repl/scenes.c \
                     src/repl/source_scope.c \
                     src/repl/state.c \
					 src/repl/text_helpers.c \
                     tools/repl_demo/source_document.c \
                     tests/gl-stubs/gl_stub_counts.c
# src/app/glr_config.c removed in step 4 of the decouple plan: pipeline TUs
# (src/repl/export.c, src/repl/scenes.c, src/repl/example_loader.c) no longer call
# glr_config_*; the controller-installed ReplExportConfigBridge is the
# only path that touches cfg state, and that lives in src/app/glr_actions.c
# (not in the demo link set).

# Object list for the standalone editor_demo. Phase 8 refit
# (plans/done/editor-demo.md) split the editor module into a generic
# half (state.c data model + edit_ops.c primitives) and a REPL-flavored
# controller half (input.c, commit.c, clipboard.c, undo.c, reformat.c,
# search.c, completion.c, plus the inline overlays). The demo links
# only the generic half plus the REPL-free UI render layer
# (text_panel, text_layout, text_search, theme). It intentionally does
# not link src/ui/app or src/app; the demo is the proof that the generic
# editor model can be rendered through ui/core without UiRenderSnapshot,
# UiState, menu/app chrome, or the REPL code-panel adapter. The REPL-flavored
# controllers are not linked at all; the demo provides its own
# generic input dispatcher (tools/editor_demo/input.c) and File menu
# (tools/editor_demo/menu.c). Phase 5 of
# plans/in-review/edit-line-ownership.md deleted the former
# tools/editor_demo/repl_shim.c — the prior ~85-stub shim went away
# with the controller files (Phase 8.7 of editor-demo.md) and the
# residual edit-line forwarder went away with the storage flip
# (Phase 4 of edit-line-ownership.md).
EDITOR_DEMO_DEP_SRCS = src/editor/edit_ops.c \
                       src/editor/state.c \
                       src/ui/core/text_layout.c \
                       src/ui/core/text_panel.c \
                       src/ui/core/text_search.c \
                       src/ui/core/theme.c \
				      src/support/cpuprof.c \
                       tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone memprof_demo (isolation demo #4). Proves
# the memory-profiling subsystem links cleanly from {support, ui/support,
# ui/core} alone — no src/ui/app, src/app, src/repl, or src/editor. The
# panel renderer (src/ui/support/memprof.c) was narrowed off UiRenderSnapshot
# onto UiMemoryPanelView so it pulls in none of the editor/repl/app headers
# the snapshot transitively dragged in. check-memprof-demo-isolation.sh
# enforces the link set.
MEMPROF_DEMO_DEP_SRCS = src/support/memprof.c \
                        src/ui/support/memprof.c \
                        src/ui/core/theme.c \
                        tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone cpuprof_demo (isolation demo #7). Twin of
# memprof_demo: the CPU profile panel (src/ui/support/cpuprof.c) was narrowed
# off UiRenderSnapshot onto UiProfilePanelView (anchor baked by the
# controller), so it links from {support, ui/support, ui/core} alone.
# support/cpuprof.c is the already-pure wall-time sampler.
CPUPROF_DEMO_DEP_SRCS = src/support/cpuprof.c \
                        src/ui/support/cpuprof.c \
                        src/ui/core/theme.c \
                        tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone variable_panel_demo (isolation demo #5).
# Proves the variable-panel subsystem links from {subsystems, ui/subsystems,
# ui/core} alone. The renderer was narrowed off UiRenderSnapshot onto
# UiVariablePanelView, and the drag handlers read name+value through an
# installed VariablePanelValueSource instead of repl/eval — so neither
# src/repl, src/editor, nor src/ui/app is in the link set. The demo builds
# the view directly and installs its own in-memory value source.
# check-variable-panel-demo-isolation enforces the link set.
VARIABLE_PANEL_DEMO_DEP_SRCS = src/subsystems/variable_panel/variable_panel_state.c \
                               src/subsystems/variable_panel/variable_panel_drag.c \
                               src/ui/subsystems/variable_panel.c \
                               src/ui/core/theme.c \
                               tests/gl-stubs/gl_stub_counts.c

# Object list for the standalone color_picker_demo (isolation demo #6). Proves
# the color-picker subsystem links from {subsystems, ui/subsystems, ui/core}
# alone. The peer reads the document + writes color edits + answers geometry
# through an installed ColorPickerHostBridge instead of repl/editor/ui-app, and
# the inline-swatch renderer (the one UiTransformer user) moved to
# src/ui/app/color_swatch.c — so none of src/repl, src/editor, or src/ui/app is
# in the link set. check-color-picker-demo-isolation enforces it.
COLOR_PICKER_DEMO_DEP_SRCS = src/subsystems/color_picker/color_picker_state.c \
                             src/ui/subsystems/color_picker.c \
                             src/ui/core/theme.c \
                             tests/gl-stubs/gl_stub_counts.c

OBJDIR = build/$(BUILD)$(if $(filter 1,$(USE_GL_STUBS)),-gl-stubs,)$(if $(filter 1,$(FREEGLUT_OSMESA)),-osmesa,)
BINDIR = $(OBJDIR)
OBJ_CFLAGS = $(BUILD_CFLAGS) $(CFLAGS) -include config.h -include prof_sections.h
DEPFLAGS = -MMD -MP

SAMPLE_OBJS = $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
SUPPORT_OBJS          = $(addprefix $(OBJDIR)/,$(SUPPORT_SRCS:.c=.o))
APP_CONTROLLER_OBJS   = $(addprefix $(OBJDIR)/,$(APP_CONTROLLER_SRCS:.c=.o))
EDITOR_OBJS           = $(addprefix $(OBJDIR)/,$(EDITOR_SRCS:.c=.o))
REPL_OBJS             = $(addprefix $(OBJDIR)/,$(REPL_SRCS:.c=.o))
SCENE_OBJS            = $(addprefix $(OBJDIR)/,$(SCENE_SRCS:.c=.o))
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
	$(SCENE_OBJS) \
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
	test_repl_state \
	test_repl_code_panel_layout \
	test_ui_theme \
	test_scene_palette \
	test_repl_code_panel_document \
	test_repl_code_panel_syntax \
	test_scene_transition \
	test_ui_scene_tabs \
	test_ui_tabbed_overlay \
	test_scene_file_menu \
	test_repl_core_parse \
	test_repl_core_format \
	test_repl_core_commit \
	test_repl_core_io \
	test_repl_export_all_commands \
	test_repl_export_lights \
	test_repl_export_clearcolor \
	test_repl_tune \
	test_repl_core_examples \
	test_repl_core_search \
	test_repl_core_search_extra \
	test_editor_completion \
	test_editor_input_selection \
	test_ui_menu_bar \
	test_audio \
	test_repl_core_internal \
	test_repl_autocomplete \
	test_repl_command_store \
	test_repl_var_drag \
	test_scene_guides \
	test_scene_render \
	test_glr_ctrl \
	test_repl_editor \
	test_repl_core_extra \
	test_repl_autonormal \
	test_repl_replay \
	test_repl_compile \
	test_tutorial_match \
	test_tutorial_runner \
	test_glr_camera

ifeq ($(USE_GL_STUBS),1)
TEST_BINS += test_ui
TEST_BINS += test_ui_text_panel
TEST_BINS += test_glr_actions
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
endif

CORE_TEST_BINS = $(filter-out test_eval test_format test_mesh_ply test_memprof test_repl_code_panel_layout test_ui_theme test_scene_palette test_audio test_scene_guides test_scene_transition test_scene_render test_scene_file_menu test_editor_completion test_glr_camera test_ui_cpuprof test_ui_memprof test_ui_text_panel test_tutorial_match,$(TEST_BINS))

# Benchmark binaries follow the same linking pattern as core test binaries
# (they reuse CORE_TEST_OBJS so they work in both real-GL and stubs builds),
# but they are intentionally NOT in TEST_BINS so `make test` does not run
# them — benchmarks are timing-sensitive and should be invoked explicitly.
BENCH_BINS = bench_repl

ROOT_BIN_LINKS = gl-repl scene_demo repl_demo editor_demo memprof_demo variable_panel_demo color_picker_demo cpuprof_demo

.PHONY: sample $(ROOT_BIN_LINKS) $(TEST_BINS) $(BENCH_BINS)

SAMPLE_BIN = $(BINDIR)/gl-repl
SCENE_DEMO_BIN = $(BINDIR)/scene_demo
REPL_DEMO_BIN = $(BINDIR)/repl_demo
EDITOR_DEMO_BIN = $(BINDIR)/editor_demo
MEMPROF_DEMO_BIN = $(BINDIR)/memprof_demo
CPUPROF_DEMO_BIN = $(BINDIR)/cpuprof_demo
VARIABLE_PANEL_DEMO_BIN = $(BINDIR)/variable_panel_demo
COLOR_PICKER_DEMO_BIN = $(BINDIR)/color_picker_demo

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

test_repl_code_panel_layout_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_code_panel_layout.o $(OBJDIR)/src/ui/core/text_layout.o
test_repl_code_panel_layout_LDLIBS =
test_repl_code_panel_layout_RUN ?= $(BINDIR)/test_repl_code_panel_layout

# Now needs theme.o to resolve externs.
test_ui_theme_OBJS = $(OBJDIR)/$(TEST_DIR)/test_ui_theme.o $(OBJDIR)/src/ui/core/theme.o
test_ui_theme_LDLIBS =
test_ui_theme_RUN ?= $(BINDIR)/test_ui_theme

# Header-only: scene/palette.h pulls in no project objects.
test_scene_palette_OBJS = $(OBJDIR)/$(TEST_DIR)/test_scene_palette.o
test_scene_palette_LDLIBS =
test_scene_palette_RUN ?= $(BINDIR)/test_scene_palette

test_audio_OBJS = $(OBJDIR)/$(TEST_DIR)/test_audio.o $(OBJDIR)/src/app/glr_audio.o
test_audio_LDLIBS = $(GL_LDFLAGS)
test_audio_RUN ?= $(BINDIR)/test_audio

test_scene_guides_OBJS = $(OBJDIR)/$(TEST_DIR)/test_scene_guides.o \
	$(OBJDIR)/src/scene/guides/geometry_guides.o \
	$(OBJDIR)/src/scene/guides/transform_guides.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_scene_guides_LDLIBS = $(GL_LDFLAGS)
test_scene_guides_RUN ?= $(BINDIR)/test_scene_guides

test_scene_transition_OBJS = $(OBJDIR)/$(TEST_DIR)/test_scene_transition.o \
	$(OBJDIR)/src/scene/scene_transition.o
test_scene_transition_LDLIBS =
test_scene_transition_RUN ?= $(BINDIR)/test_scene_transition

test_scene_render_OBJS = $(OBJDIR)/$(TEST_DIR)/test_scene_render.o \
	$(SCENE_OBJS) \
	$(OBJDIR)/src/support/cpuprof.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_scene_render_LDLIBS = $(GL_LDFLAGS)
test_scene_render_RUN ?= $(BINDIR)/test_scene_render

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
	$(OBJDIR)/src/ui/support/cpuprof.o \
	$(OBJDIR)/src/ui/core/theme.o \
	$(OBJDIR)/tests/gl-stubs/gl_stub_counts.o
test_ui_cpuprof_LDLIBS = $(GL_LDFLAGS)
test_ui_cpuprof_RUN ?= $(BINDIR)/test_ui_cpuprof

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

test_repl_replay_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_replay.o $(filter-out $(OBJDIR)/src/subsystems/replay/replay.o,$(CORE_TEST_OBJS))

# test_replay_walk includes app/glr_ctrl.c as a translation unit to reach
# the static cursor_guide_snapshot_with_flat_args helper, so the object
# must be filtered out the same way test_glr_ctrl does.
test_replay_walk_OBJS = $(OBJDIR)/$(TEST_DIR)/test_replay_walk.o $(filter-out $(OBJDIR)/src/app/glr_ctrl.o,$(CORE_TEST_OBJS))

test_edit_overlays_OBJS = $(OBJDIR)/$(TEST_DIR)/test_edit_overlays.o $(filter-out $(OBJDIR)/src/subsystems/edit_overlays/edit_overlays.o,$(CORE_TEST_OBJS))

TEST_OBJS = $(foreach test,$(TEST_BINS),$($(test)_OBJS))
TEST_RUNNER_CASES = $(foreach test,$(TEST_BINS),'$(test):::$($(test)_RUN)')
BENCH_OBJS = $(foreach bin,$(BENCH_BINS),$($(bin)_OBJS))

TEST_JOBS ?=

ALL_OBJS = $(sort $(SAMPLE_OBJS) $(TEST_OBJS) $(BENCH_OBJS))

DEPS = $(ALL_OBJS:.o=.d)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) $(DEPFLAGS) -c -o $@ $<

# Vendored freeglut static library. Built once via CMake into $(FREEGLUT_BUILD),
# which lives under third_party/ so the top-level `make clean` (rm -rf ./build)
# leaves it intact. This rule has NO dependency on the freeglut sources, so
# after re-vendoring run `make freeglut-clean` to force a rebuild. The backend
# (Cocoa by default, OSMesa under FREEGLUT_OSMESA=1) and build dir/lib name are
# selected by FREEGLUT_CMAKE_BACKEND / FREEGLUT_BUILD up top. The OSMesa backend
# resolves libOSMesa via pkg-config; point it at Homebrew's mesa .pc files.
# On macOS, OSMesa's pkg-config (osmesa.pc) lives under Homebrew's mesa prefix;
# on Linux libosmesa6-dev puts it on the default pkg-config path, so this stays
# empty there.
FREEGLUT_PKG_CONFIG_PATH := $(if $(filter 1,$(FREEGLUT_OSMESA)),$(if $(filter Darwin,$(UNAME_S)),$(shell brew --prefix mesa 2>/dev/null)/lib/pkgconfig),)
$(FREEGLUT_STATIC_LIB):
	PKG_CONFIG_PATH="$(FREEGLUT_PKG_CONFIG_PATH):$$PKG_CONFIG_PATH" \
	cmake -S $(FREEGLUT_SRC) -B $(FREEGLUT_BUILD) \
	  $(FREEGLUT_CMAKE_BACKEND) -DFREEGLUT_BUILD_STATIC_LIBS=ON \
	  -DFREEGLUT_BUILD_SHARED_LIBS=OFF -DFREEGLUT_BUILD_DEMOS=OFF \
	  -DCMAKE_BUILD_TYPE=Release
	cmake --build $(FREEGLUT_BUILD) --target freeglut_static

freeglut-clean: ## Remove the vendored freeglut CMake build (forces a rebuild).
	rm -rf $(FREEGLUT_BUILD)
.PHONY: freeglut-clean

$(SAMPLE_BIN): $(SAMPLE_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(SAMPLE_OBJS) $(GL_LDFLAGS)

gl-repl: FORCE $(SAMPLE_BIN) ## Build the main gl-repl binary using release flags by default.
	ln -sfn $(SAMPLE_BIN) $@

sample: gl-repl ## Alias for the main gl-repl sample binary.

# macOS .app bundle so the Dock/Finder show the gl-repl cube icon instead of
# the launching terminal's icon. Pure packaging — no source changes, so the
# -std=c99 / Linux-portable build stays untouched. Needs rsvg-convert
# (brew install librsvg) and the Xcode-shipped iconutil.
MACOS_PKG = packaging/macos
APP_BUNDLE = gl-repl.app
APP_ICNS = $(MACOS_PKG)/gl-repl.icns
APP_ICONSET = $(MACOS_PKG)/gl-repl.iconset
# Icon source SVG. Alternatives in packaging/macos/: gl-repl.svg (flat cube),
# gl-repl-retro-A.svg (chrome synthwave), -B (gold), -C/-C2 (silver OpenGL),
# -D (cube+wordmark), gl-repl-soft-cube.svg (soft pastel perspective cube; used
# here), gl-repl-soft-cube-strong.svg (same, stronger perspective).
APP_ICON_SVG = $(MACOS_PKG)/gl-repl-soft-cube.svg

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
	touch $(APP_BUNDLE)
	@echo "Built $(APP_BUNDLE) — run: open $(APP_BUNDLE)"

# Standalone demo binary that drives the scene module with a teapot callback.
# Proves the scene/ subtree links cleanly without the editor/UI/controller code.
SCENE_DEMO_OBJS = $(OBJDIR)/tools/scene_demo/scene_demo.o \
                   $(addprefix $(OBJDIR)/,$(SCENE_DEMO_DEP_SRCS:.c=.o))

$(SCENE_DEMO_BIN): $(SCENE_DEMO_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(SCENE_DEMO_OBJS) $(GL_LDFLAGS)

scene_demo: FORCE $(SCENE_DEMO_BIN) ## Build the standalone scene demo.
	ln -sfn $(SCENE_DEMO_BIN) $@

# Standalone REPL pipeline demo. Inverse of scene_demo: proves the
# REPL pipeline links without editor input dispatch / controller / UI.
REPL_DEMO_OBJS = $(OBJDIR)/tools/repl_demo/repl_demo.o \
                 $(OBJDIR)/tools/repl_demo/stubs.o \
                 $(addprefix $(OBJDIR)/,$(REPL_DEMO_DEP_SRCS:.c=.o))

$(REPL_DEMO_BIN): $(REPL_DEMO_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(REPL_DEMO_OBJS) $(GL_LDFLAGS)

repl_demo: FORCE $(REPL_DEMO_BIN) ## Build the standalone REPL pipeline demo.
	ln -sfn $(REPL_DEMO_BIN) $@

# Standalone generic text editor demo. Inverse of repl_demo: proves
# that the editor data model (src/editor/state.c) and the generic
# text-editing primitives (src/editor/edit_ops.c) link cleanly into a
# non-REPL controller using src/ui/core only, with no src/ui/app or
# src/app. The demo's own dispatcher
# (tools/editor_demo/input.c) and File menu (tools/editor_demo/menu.c)
# stand in for the REPL-flavored controller files
# (src/editor/{input,commit,clipboard,undo,reformat,search,completion}.c
# and the inline overlays), which Phase 8.7 dropped from this link
# set entirely. Phase 5 of plans/in-review/edit-line-ownership.md
# deleted the former tools/editor_demo/repl_shim.c — after Phase 4
# moved edit-line storage to EditorState, the shim's
# repl_state_edit_line stubs had no remaining callers.
EDITOR_DEMO_OBJS = $(OBJDIR)/tools/editor_demo/editor_demo.o \
                   $(OBJDIR)/tools/editor_demo/menu.o \
                   $(OBJDIR)/tools/editor_demo/input.o \
                   $(addprefix $(OBJDIR)/,$(EDITOR_DEMO_DEP_SRCS:.c=.o))

$(EDITOR_DEMO_BIN): $(EDITOR_DEMO_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(EDITOR_DEMO_OBJS) $(GL_LDFLAGS)

editor_demo: FORCE $(EDITOR_DEMO_BIN) ## Build the standalone editor demo.
	ln -sfn $(EDITOR_DEMO_BIN) $@

# Standalone memory-profiling demo (isolation demo #4). Drives the
# memprof sampler + overlay panel from {support, ui/support, ui/core}
# with no editor/repl/app/ui-app code linked in.
MEMPROF_DEMO_OBJS = $(OBJDIR)/tools/memprof_demo/memprof_demo.o \
                    $(addprefix $(OBJDIR)/,$(MEMPROF_DEMO_DEP_SRCS:.c=.o))

$(MEMPROF_DEMO_BIN): $(MEMPROF_DEMO_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(MEMPROF_DEMO_OBJS) $(GL_LDFLAGS)

memprof_demo: FORCE $(MEMPROF_DEMO_BIN) ## Build the standalone memory-profiling demo.
	ln -sfn $(MEMPROF_DEMO_BIN) $@

# Standalone CPU-profiling demo (isolation demo #7). Twin of memprof_demo:
# a spinning teapot bracketed by prof sections + the live CPU profile panel,
# from {support, ui/support, ui/core} with no editor/repl/app/ui-app linked in.
CPUPROF_DEMO_OBJS = $(OBJDIR)/tools/cpuprof_demo/cpuprof_demo.o \
                    $(addprefix $(OBJDIR)/,$(CPUPROF_DEMO_DEP_SRCS:.c=.o))

$(CPUPROF_DEMO_BIN): $(CPUPROF_DEMO_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(CPUPROF_DEMO_OBJS) $(GL_LDFLAGS)

cpuprof_demo: FORCE $(CPUPROF_DEMO_BIN) ## Build the standalone CPU-profiling demo.
	ln -sfn $(CPUPROF_DEMO_BIN) $@

# Standalone variable-panel demo (isolation demo #5). Drives the variable
# slider panel + drag math from {subsystems, ui/subsystems, ui/core} with no
# editor/repl/app/ui-app code linked in.
VARIABLE_PANEL_DEMO_OBJS = $(OBJDIR)/tools/variable_panel_demo/variable_panel_demo.o \
                           $(addprefix $(OBJDIR)/,$(VARIABLE_PANEL_DEMO_DEP_SRCS:.c=.o))

$(VARIABLE_PANEL_DEMO_BIN): $(VARIABLE_PANEL_DEMO_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(VARIABLE_PANEL_DEMO_OBJS) $(GL_LDFLAGS)

variable_panel_demo: FORCE $(VARIABLE_PANEL_DEMO_BIN) ## Build the standalone variable-panel demo.
	ln -sfn $(VARIABLE_PANEL_DEMO_BIN) $@

# Standalone color-picker demo (isolation demo #6). Drives the floating color
# picker over a row of GLUT shapes from {subsystems, ui/subsystems, ui/core}
# with no editor/repl/app/ui-app code linked in.
COLOR_PICKER_DEMO_OBJS = $(OBJDIR)/tools/color_picker_demo/color_picker_demo.o \
                         $(addprefix $(OBJDIR)/,$(COLOR_PICKER_DEMO_DEP_SRCS:.c=.o))

$(COLOR_PICKER_DEMO_BIN): $(COLOR_PICKER_DEMO_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(COLOR_PICKER_DEMO_OBJS) $(GL_LDFLAGS)

color_picker_demo: FORCE $(COLOR_PICKER_DEMO_BIN) ## Build the standalone color-picker demo.
	ln -sfn $(COLOR_PICKER_DEMO_BIN) $@

.SECONDEXPANSION:

# GNU make normally expands prerequisites before it knows the concrete target.
# .SECONDEXPANSION adds a second pass after `$@` is known, so this one rule can
# turn `test_eval` into `$(test_eval_OBJS)`, `test_repl_core_io` into
# `$(test_repl_core_io_OBJS)`, etc. The doubled dollars delay that lookup until
# the second pass.
define built_binary
$(BINDIR)/$(1): $$($(1)_OBJS)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(OBJ_CFLAGS) -o $$@ $$^ $$($(1)_LDLIBS) $$(COVERAGE_LDFLAGS)

$(1): $$(BINDIR)/$(1)
endef

$(foreach test,$(TEST_BINS),$(eval $(call built_binary,$(test))))
$(foreach bin,$(BENCH_BINS),$(eval $(call built_binary,$(bin))))

# Real-GL tests: need an actual GL context (created via GLUT), which the
# no-op stub harness cannot model and headless CI cannot provide. These
# are intentionally NOT in TEST_BINS, so `make test` / `make test-stubs`
# never build or run them. Run locally with a display via `make gl-tests`.
GL_TEST_BINS = test_ui_gl_state test_scene_underwater_fill_gl

$(BINDIR)/test_ui_gl_state: $(OBJDIR)/$(TEST_DIR)/test_ui_gl_state.o
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $^ $(GL_LDFLAGS)

# Drives scene_grid_render(GRID_THEME_OCEAN) with cam_world_y < 0 and
# nv_fog_distance_supported = 1, then glReadPixels and checks corner
# pixels. Reproduces the post-fb976f0 underwater-fill regression on
# drivers that advertise GL_NV_fog_distance.
$(BINDIR)/test_scene_underwater_fill_gl: $(OBJDIR)/$(TEST_DIR)/test_scene_underwater_fill_gl.o $(OBJDIR)/src/scene/grid.o
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $^ $(GL_LDFLAGS)

gl-tests: $(addprefix $(BINDIR)/,$(GL_TEST_BINS)) ## Run real-GL UI state tests (needs a display; excluded from `make test`).
	@for b in $(addprefix $(BINDIR)/,$(GL_TEST_BINS)); do \
	  printf '$(CYAN)==> %s$(NC)\n' "$$b"; "$$b" || exit $$?; \
	done

.PHONY: gl-tests $(GL_TEST_BINS)

# The vendored static freeglut (macOS) is a build-time artifact, so every binary
# whose link line embeds its archive path through $(GL_LDFLAGS) must order-only
# depend on it — otherwise those links run before the archive exists and fail.
# Placed HERE (after every referenced target var is defined: SAMPLE_BIN + the
# demos ~earlier, TEST_BINS/BENCH_BINS, and GL_TEST_BINS just above) — a static
# target list expands at parse time, so an earlier placement would silently
# attach the prereq to nothing. Skipped under `make glut` (FREEGLUT_VENDOR=0)
# and on the default Linux path / GL stubs (FREEGLUT_LIB is empty there too).
# Vendored freeglut is built on macOS (Cocoa or OSMesa) and on Linux ONLY for
# the OSMesa backend (FREEGLUT_OSMESA=1); the default Linux path uses system
# freeglut and needs no archive prereq.
ifeq ($(FREEGLUT_VENDOR),1)
ifneq ($(filter Darwin,$(UNAME_S))$(filter 1,$(FREEGLUT_OSMESA)),)
ifneq ($(USE_GL_STUBS),1)
$(SAMPLE_BIN) $(SCENE_DEMO_BIN) $(REPL_DEMO_BIN) $(EDITOR_DEMO_BIN) \
$(MEMPROF_DEMO_BIN) $(CPUPROF_DEMO_BIN) $(VARIABLE_PANEL_DEMO_BIN) \
$(COLOR_PICKER_DEMO_BIN) \
$(addprefix $(BINDIR)/,$(TEST_BINS) $(BENCH_BINS) $(GL_TEST_BINS)): | $(FREEGLUT_STATIC_LIB)
endif
endif
endif

# Layering boundary enforcement ------------------------------------------
check-gl-boundaries: ## Verify GL/GLUT calls are isolated to allowed files.
	@echo "    Checking GL/GLU drawing calls isolation..."
	@! grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*[[:space:]]*\(' $(REPL_SRCS) | grep -v '^src/repl/executor\.c:' | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GL/GLU calls found outside src/repl/executor.c$(NC)" && exit 1)
	@echo "    Checking GL/GLU calls in gl_repl.h..."
	@! grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*[[:space:]]*\(' gl_repl.h src/app/glr_defaults.h | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GL/GLU calls found in gl_repl.h$(NC)" && exit 1)
	@echo "    Checking GLUT input/feedback calls isolation..."
	@! grep -nE '\bglut[A-Z][A-Za-z0-9]*[[:space:]]*\(' $(REPL_SRCS) | grep -vE '^src/repl/executor\.c:' | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GLUT calls found outside src/repl/executor.c$(NC)" && exit 1)
	@echo "    GL/GLUT boundaries $(GREEN)OK$(NC)"

check-layer-coupling: ## Verify UI and scene layers don't include each other's headers.
	@echo "    Checking UI/scene layer coupling..."
	@! grep -nE '#include\s+"scene/' $(UI_SRCS) $(UI_HDRS) || (echo "    $(RED)ERROR: UI files must not include scene headers$(NC)" && exit 1)
	@! grep -nE '#include\s+"ui/' $(SCENE_SRCS) $(SCENE_HDRS) || (echo "    $(RED)ERROR: scene files must not include UI headers$(NC)" && exit 1)
	@echo "    Layer coupling $(GREEN)OK$(NC)"


check-controller-boundaries: ## Verify controller owns the scene/UI wiring boundary.
	@echo "Checking controller boundaries..."
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"scene/' $(REPL_SRCS) src/app/glr_ctrl.c src/app/glr_ctrl_router.c \
		| grep -v '^src/app/glr_ctrl\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: scene headers included outside src/app/glr_ctrl.c:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@# Boundary allowlist for ui/ includes: src/app/glr_ctrl.c is the
	@# router/controller and explicitly orchestrates UI; src/app/glr_actions.c
	@# is the menu/shortcut dispatch; src/repl/export.c reads UI chrome
	@# state to serialize ; repl_editor.c stays in the list as a
	@# historical breadcrumb (the file is deleted but the regex
	@# tolerates absence). The previously-listed repl_(debug|config|
	@# camera_controls) entries are gone — those files were renamed
	@# into the glr_* namespace and are no longer matched by
	@# REPL_SRCS, so the check skips them entirely.
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"ui/' $(REPL_SRCS) src/app/glr_ctrl.c src/app/glr_ctrl_router.c \
		| grep -vE '^src/app/(glr_ctrl|glr_ctrl_router|glr_actions)\.c$$|^src/repl/export\.c$$|^repl_editor\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: new ui headers included outside approved exceptions:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "Controller boundaries $(GREEN)OK$(NC)"

check-scene-no-repl-state-mut: ## Verify scene code does not mutate REPL state directly.
	@echo "Checking scene renderers do not mutate REPL state..."
	@bad=$$(grep -nE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' $(SCENE_SRCS) || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: scene files mutate REPL state:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "Scene mutation boundary $(GREEN)OK$(NC)"

check-pure-scene-no-repl-state: ## Verify scene files do not reach into REPL state/replay APIs.
	@echo "Checking scene files do not reach into REPL state/replay APIs..."
	@bad=$$(grep -nE 'repl_(state|replay)_' $(SCENE_SRCS) || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: scene files reach into REPL state/replay APIs:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "Pure-scene boundary $(GREEN)OK$(NC)"

check-state-boundaries: ## Verify REPL state facade usage stays in owned modules.
	@echo "Checking state facade boundaries..."
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"repl/state\.h"' $(SCENE_SRCS) $(STATE_NEUTRAL_SRCS) 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: scene or state-neutral files include src/repl/state.h:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"repl/core_internal\.h"' \
		src/app/glr_ctrl.c $(SCENE_SRCS) $(UI_SRCS) $(STATE_NEUTRAL_SRCS) 2>/dev/null \
		| grep -vE '^ui_(color_picker|panels)\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: unapproved view/utility files include src/repl/core_internal.h:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@bad=$$(grep -lE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' $(UI_SRCS) 2>/dev/null \
		| grep -vE '^(ui_(color_picker|help_overlay|panels))\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: unapproved UI files mutate REPL state directly:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@bad=$$(grep -nE 'repl_(state|replay)_' $(SCENE_SRCS) 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: scene files reach into REPL state/replay APIs:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "State facade boundaries $(GREEN)OK$(NC)"

check-views-no-owners: ## Verify scene/UI files do not include src/repl/state_owners.h.
	@echo "Checking scene/UI view files do not include src/repl/state_owners.h..."
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"repl/state_owners\.h"' $(SCENE_SRCS) $(UI_SRCS) 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: scene/UI view files include src/repl/state_owners.h:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "View-file ownership boundary $(GREEN)OK$(NC)"

check-ui-no-repl-state-mut: ## Verify UI files do not mutate REPL state directly.
	@echo "Checking UI files do not mutate REPL state directly..."
	@bad=$$(grep -nE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' $(UI_SRCS) || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: UI files mutate REPL state:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "UI mutation boundary $(GREEN)OK$(NC)"

check-no-write-through-view: ## Verify no writes happen through pointer fields on view structs.
	@bash scripts/check-no-write-through-view.sh scripts/allowlists/write-through-view.txt $(UI_SRCS) $(SCENE_SRCS)

check-runtime-state-value-fields: ## Verify ReplRuntimeState owns values, not pointer aliases.
	@bash scripts/check-runtime-state-value-fields.sh src/repl/state.h

check-views-flat-types: ## Verify view/state snapshot structs avoid mutable pointer fields.
	@bash scripts/check-views-flat.sh scripts/baselines/views-flat-violations.txt

check-public-state-no-writable-pointers: check-views-flat-types ## Alias for the public state/view writable pointer check.

check-views-by-value-snapshot: ## Ratchet pointer-return snapshot accessors down over time.
	@bash scripts/check-views-by-value-snapshot.sh scripts/baselines/by-value-snapshot-pointer-returns.txt

check-state-read-getters-return-values: check-views-by-value-snapshot ## Verify read getters return values or read-only views.

check-ui-renderer-takes-view: ## Verify audited UI renderers use canonical snapshot signatures.
	@bash scripts/check-ui-renderer-signatures.sh scripts/allowlists/ui-renderers-signature.txt

check-renderer-no-direct-mutators: ## Verify audited renderers do not mutate state directly.
	@bash scripts/check-renderer-purity.sh scripts/allowlists/renderer-purity.txt

check-output-actualization: ## Verify Ui*Output fields are consumed by controller actualization.
	@bash scripts/check-output-actualization.sh

check-state-c-shrinking: ## Ratchet src/repl/state.c line count down over time.
	@bash scripts/check-state-c-shrinking.sh scripts/baselines/state-c-lines.txt src/repl/state.c

check-repl-no-direct-editor: ## Forbid editor coupling in repl_*.{c,h} (Phase 7 of feature/source-document-port.md — hard zero).
	@bash scripts/check-repl-no-direct-editor.sh

check-editor-no-app: ## Ratchet: forbid new app/glr_* coupling in src/editor/ (see audit #8).
	@bash scripts/check-editor-no-app.sh

check-repl-no-app: ## Ratchet: forbid new app/glr_* coupling in src/repl/.
	@bash scripts/check-repl-no-app.sh

check-repl-no-mut-reads: ## Ratchet: cap `_mut()` calls in src/repl/ outside owner files (audit #7/#14).
	@bash scripts/check-repl-no-mut-reads.sh

check-scene-no-upper-layers: ## Hard guard: src/scene/ must not include from app/editor/ui/subsystems.
	@bash scripts/check-scene-no-upper-layers.sh

check-ui-core-no-upper-layers: ## Hard guard: src/ui/core/ must not include from app/editor/repl/scene/subsystems/ui-app.
	@bash scripts/check-ui-core-no-upper-layers.sh

check-repl-demo-no-editor: ## Forbid editor implementation in the standalone demo (Phase 7).
	@bash scripts/check-repl-demo-no-editor.sh

check-memprof-demo-isolation: ## Forbid app/repl/editor coupling in the memprof demo link set.
	@bash scripts/check-subsystem-demo-isolation.sh MEMPROF_DEMO_DEP_SRCS tools/memprof_demo memprof_demo

check-cpuprof-demo-isolation: ## Forbid app/repl/editor coupling in the cpuprof demo link set.
	@bash scripts/check-subsystem-demo-isolation.sh CPUPROF_DEMO_DEP_SRCS tools/cpuprof_demo cpuprof_demo

check-cpuprof-standalone: ## Verify the generic CPU-profile timer compiles with no section catalog (fallback path).
	@bash scripts/check-cpuprof-standalone.sh

check-variable-panel-demo-isolation: ## Forbid app/repl/editor coupling in the variable-panel demo link set.
	@bash scripts/check-subsystem-demo-isolation.sh VARIABLE_PANEL_DEMO_DEP_SRCS tools/variable_panel_demo variable_panel_demo

check-color-picker-demo-isolation: ## Forbid app/repl/editor coupling in the color-picker demo link set.
	@bash scripts/check-subsystem-demo-isolation.sh COLOR_PICKER_DEMO_DEP_SRCS tools/color_picker_demo color_picker_demo

check-source-document-port-owners: ## source_document_* symbols only defined in approved host adapters (Phase 7).
	@bash scripts/check-source-document-port-owners.sh

check-no-facade-include-in-views: ## Verify view/render files avoid repl_state facade headers.
	@bash scripts/check-no-facade-include-in-views.sh scripts/allowlists/facade-includes-in-views.txt

check-domain-owner-encapsulation: ## Enforce per-domain mutator encapsulation rules as domains migrate.
	@bash scripts/check-domain-encapsulation.sh scripts/allowlists/domain-owner-encapsulation.txt

check-ui-no-repl-state-read: ## Verify UI renderers consume the UiRenderSnapshot, not live repl_state_*().
	@echo "Checking UI render entry points consume UiRenderSnapshot..."
	@bad=$$(grep -nE 'repl_state_[A-Za-z0-9_]+\s*\(' $(UI_SRCS) 2>/dev/null \
		|| true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: ui_*.c files outside the input-bridge allowlist read live repl_state_*():$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@bash scripts/check-ui-renderer-signatures.sh scripts/allowlists/ui-renderers-signature.txt
	@echo "ui-no-repl-state-read $(GREEN)OK$(NC)"

check-state-ownership: ## Run state-ownership contract checks (new + tightened existing checks).
	@set -e -o pipefail; \
	for target in \
		check-controller-boundaries \
		check-scene-no-repl-state-mut \
		check-pure-scene-no-repl-state \
		check-state-boundaries \
		check-views-no-owners \
		check-ui-no-repl-state-mut \
		check-no-write-through-view \
		check-runtime-state-value-fields \
		check-public-state-no-writable-pointers \
		check-state-read-getters-return-values \
		check-ui-renderer-takes-view \
		check-renderer-no-direct-mutators \
		check-output-actualization \
		check-state-c-shrinking \
		check-no-facade-include-in-views \
		check-domain-owner-encapsulation \
		check-ui-no-repl-state-read \
		check-editor-ownership-budget \
		check-no-store-text-api \
		check-repl-no-direct-buffer-read \
		check-glr-ctrl-not-editor-mirror \
		check-ui-returns-hits-only \
		check-ui-panels-no-mutators \
		check-replay-ui-isolation \
		check-color-picker-ui-isolation \
		check-variable-panel-forwarders \
		check-replay-forwarders \
		check-no-repl-commit \
		check-no-repl-editor-input-shim \
		check-no-set-status-in-repl-parser \
		check-no-set-status-in-compile-apply \
		check-no-load-line-to-input-in-pipeline \
		check-repl-state-no-glr-state \
		check-glr-state-no-repl-mutators \
		check-repl-scenes-cfg-clear-paired \
		check-repl-export-no-ui-layout \
		check-repl-export-via-bridge \
		check-ui-no-export-resolver \
		check-no-feed-line-in-pipeline \
		check-repl-no-direct-tutorial-runner \
		check-repl-demo-stubs-shrinking \
		check-repl-no-direct-editor \
		check-repl-demo-no-editor \
		check-memprof-demo-isolation \
		check-cpuprof-demo-isolation \
		check-cpuprof-standalone \
		check-variable-panel-demo-isolation \
		check-color-picker-demo-isolation \
		check-editor-repl-surface \
		check-edit-ops-pure \
		check-no-raw-undo-clear \
		check-source-document-port-owners \
		check-editor-no-app \
		check-repl-no-app \
		check-repl-no-mut-reads \
		check-scene-no-upper-layers \
		check-ui-core-no-upper-layers \
		check-module-prefixes \
		check-include-style \
		check-tier-c-function-size \
		check-no-test-default-output \
		check-keymap-no-dup; do \
		printf "  $(YELLOW)▶$(NC) $$target\n"; \
		$(MAKE) --no-print-directory $$target 2>&1 | sed 's/^/    /' | sed $$'s/ OK / \033[0;32mOK\033[0m /g; s/ OK$$/ \033[0;32mOK\033[0m/' || exit $$?; \
	done

check-public-api-usage: ## Scan public API declarations for unused functions (informational).
	@bash scripts/check-unused-apis.sh

check-duplicate-api-decls: ## Scan module public headers for duplicate function declarations; fails if any are found.
	@bash scripts/check-duplicate-api-decls.sh

audit-editor-ownership: ## Report editor/REPL/UI ownership drift (informational; see done/editor-owns-text-completion.md).
	@bash scripts/audit_editor_ownership.sh

check-editor-ownership-budget: ## Ratchet the editor/UI transitional-coupling budget down only.
	@bash scripts/check-editor-ownership-budget.sh scripts/baselines/editor-ownership-budget.txt

check-no-store-text-api: ## Verify repl_command_store_*_with_line[s] API stays gone.
	@bash scripts/check-no-store-text-api.sh

check-repl-no-direct-buffer-read: ## Verify repl_*.c readers go through EditorBufferView, not editor_buffer_line().
	@bash scripts/check-repl-no-direct-buffer-read.sh scripts/allowlists/repl-no-direct-buffer-read.txt

check-glr-ctrl-not-editor-mirror: ## Verify imrepl_ctrl does not grow per-field editor wrappers.
	@bash scripts/check-glr-ctrl-not-editor-mirror.sh

check-ui-returns-hits-only: ## Verify ui_*.c input helpers do not call REPL/editor mutators (ratchet down only).
	@bash scripts/check-ui-returns-hits-only.sh scripts/baselines/ui-returns-hits-only.txt

check-ui-text-panel-pure: ## Verify src/ui/core/text_panel.* stays REPL/editor-free.
	@bash scripts/check-ui-text-panel-pure.sh

check-editor-repl-surface: ## Ratchet direct repl_* call surface in src/editor/input.c and commit.c.
	@bash scripts/check-editor-repl-surface.sh scripts/baselines/editor-repl-surface.txt

check-edit-ops-pure: ## Verify src/editor/edit_ops.* stays REPL-free (Phase 8 generic-primitives invariant).
	@bash scripts/check-edit-ops-pure.sh

check-no-raw-undo-clear: ## Production code must use editor_undo_note_wholesale_replacement(), not raw editor_undo_clear().
	@bash scripts/check-no-raw-undo-clear.sh

check-ui-panels-no-mutators: ## Hard guard: src/ui/app/panels.c references no input-dispatch mutators (Phase J2.2).
	@bash scripts/check-ui-panels-no-mutators.sh

check-replay-ui-isolation: ## Hard guard: replay_ui_*.c is feature-UI — no editor / REPL mutators or parser/compile/apply calls.
	@bash scripts/check-replay-ui-isolation.sh

check-color-picker-ui-isolation: ## Strict guard: src/ui/subsystems/color_picker.c is pure renderer/hit-test over ColorPickerView — no mutators, no live state reads, no parser/compile/apply.
	@bash scripts/check-color-picker-ui-isolation.sh

check-variable-panel-forwarders: ## Ratchet legacy variable_panel forwarder API uses (editor_state_variable_drag*, ui_state_variable_panel*, repl_var_drag_*).
	@bash scripts/check-variable-panel-forwarders.sh scripts/baselines/variable-panel-forwarders.txt

check-replay-forwarders: ## Ratchet legacy repl_state_replay* forwarder API uses (replay peer is the owner).
	@bash scripts/check-replay-forwarders.sh scripts/baselines/replay-forwarders.txt

check-no-repl-commit: ## Verify repl_commit.{c,h} stays deleted (commit dispatch lives in src/editor/commit.c).
	@bash scripts/check-no-repl-commit.sh

check-no-repl-editor-input-shim: ## Verify src/editor/input.c does not delegate to legacy repl_*_func entry points.
	@bash scripts/check-no-repl-editor-input-shim.sh

check-no-set-status-in-repl-parser: ## Ratchet set_status calls inside src/repl/parser.c (parser diagnostics flow via ctx->err_buf).
	@bash scripts/check-no-set-status-in-repl-parser.sh scripts/baselines/repl-parser-set-status.txt

check-no-set-status-in-compile-apply: ## Verify src/repl/compile.c / src/repl/apply.c never call set_status (Phase C purity).
	@bash scripts/check-no-set-status-in-compile-apply.sh

check-no-load-line-to-input-in-pipeline: ## Verify REPL pipeline TUs do not call editor-side editor_load_line_to_input.
	@bash scripts/check-no-load-line-to-input-in-pipeline.sh

check-repl-state-no-glr-state: ## Verify REPL pipeline TUs do not include src/app/glr_state.h or reference GlrState symbols.
	@bash scripts/check-repl-state-no-glr-state.sh

check-glr-state-no-repl-mutators: ## Verify src/app/glr_state.c does not call back into REPL state mutators.
	@bash scripts/check-glr-state-no-repl-mutators.sh

check-repl-scenes-cfg-clear-paired: ## Verify every g_user_scenes[X].used=0 in src/repl/scenes.c pairs with scene_cfg_clear.
	@bash scripts/check-repl-scenes-cfg-clear-paired.sh

check-repl-export-no-ui-layout: ## Verify src/repl/export.c does not call ui_layout_* / ui_state_*.
	@bash scripts/check-repl-export-no-ui-layout.sh

check-repl-export-via-bridge: ## Verify src/repl/export.c pulls app/scene state only via controller-installed bridges (no scene_*/glr_* calls or scene/app includes).
	@bash scripts/check-repl-export-via-bridge.sh

check-ui-no-export-resolver: ## Verify src/ui reads the snapshot-frozen reshape projection, never calls repl_export_reshape_projection_lines() live.
	@bash scripts/check-ui-no-export-resolver.sh

check-no-feed-line-in-pipeline: ## Verify REPL pipeline TUs do not call editor_feed_line() (cleared by step 5b/7e).
	@bash scripts/check-no-feed-line-in-pipeline.sh

check-repl-no-direct-tutorial-runner: ## Verify REPL pipeline TUs request tutorial teardown through ReplHostEffects.
	@bash scripts/check-repl-no-direct-tutorial-runner.sh

check-module-prefixes: ## Verify stale pre-cleanup symbol prefixes have not reappeared under src/.
	@bash scripts/check-module-prefixes.sh

check-repl-demo-stubs-shrinking: ## Ratchet on tools/repl_demo/stubs.c — must not grow past 0 stubs.
	@bash scripts/check-repl-demo-stubs-shrinking.sh

check-include-style: ## Hard guard: project-local headers must use "X.h", not <X.h>.
	@bash scripts/check-include-style.sh

check-keymap-no-dup: ## Hard guard: no two keymap.h bindings share a (key, mods) — a double-map.
	@bash scripts/keymap.sh check

keymap-list: ## Print current key bindings + the free Ctrl / Ctrl+Shift / F-key slots.
	@bash scripts/keymap.sh list

check-c99: ## C99 build guard: gl-repl + bench + demo sources must syntax-check under gcc -std=c99 (non-pedantic; tests excluded; in the standard gate).
	@C99_SRCS='$(SRCS)' bash scripts/check-c99.sh

check-tier-c-function-size: ## Size ratchet: parse_command and flatten_range must not grow past their baselines.
	@bash scripts/check-tier-c-function-size.sh scripts/baselines/tier-c-function-size.txt

check-no-test-default-output: ## Hard guard: tests may not call repl_save_default_output() (writes ./output.c in repo root).
	@bash scripts/check-no-test-default-output.sh

find-trailing-whitespace: ## Report all trailing whitespace in tracked source files (whole repo).
	@git ls-files '*.c' '*.h' '*.md' Makefile | xargs grep -rn ' $$' || echo "no trailing whitespace found"

check-trailing-whitespace: ## Verify commits since origin/main contain no trailing whitespace.
	@set -e; \
	base=$${CHECK_BASE:-origin/main}; \
	if git rev-parse --verify "$$base" >/dev/null 2>&1; then \
		merge_base=$$(git merge-base "$$base" HEAD); \
		git --no-pager diff --check "$$merge_base" -- . ':(exclude)third_party/'; \
	else \
		git --no-pager diff --cached --check -- . ':(exclude)third_party/'; \
		git --no-pager diff --check -- . ':(exclude)third_party/'; \
	fi; \
	echo "trailing-whitespace OK"

CHECK_TARGETS = \
	check-trailing-whitespace \
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

test: $(TEST_BINS) ## Run the full automated test suite.
	@REPL_EXPORT_CC="$(CC)" \
	REPL_EXPORT_COMPILE_CFLAGS='$(BUILD_CFLAGS) $(CFLAGS)' \
	TEST_JOBS="$(TEST_JOBS)" \
	sh scripts/run-tests.sh $(TEST_RUNNER_CASES)

test-detailed: $(TEST_BINS) ## Run the full test suite with verbose example export/compile logging.
	@REPL_EXPORT_CC="$(CC)" \
	REPL_EXPORT_COMPILE_CFLAGS='$(BUILD_CFLAGS) $(CFLAGS)' \
	REPL_EXPORT_VERBOSE=1 \
	TEST_JOBS="$(TEST_JOBS)" \
	sh scripts/run-tests.sh $(TEST_RUNNER_CASES)

test-stubs: check-trailing-whitespace check-gl-boundaries check-layer-coupling check-state-ownership ## Build and run tests using local GL/GLU/GLUT stubs, without GL libs.
	$(MAKE) test USE_GL_STUBS=1

test-full: ## Full gate: stub tests + checks + build gl-repl, bench, repl_demo, scene_demo, editor_demo.
	$(MAKE) --no-print-directory repl_demo USE_GL_STUBS=1
	$(MAKE) --no-print-directory editor_demo USE_GL_STUBS=1
	$(MAKE) --no-print-directory memprof_demo USE_GL_STUBS=1
	$(MAKE) --no-print-directory cpuprof_demo USE_GL_STUBS=1
	$(MAKE) --no-print-directory variable_panel_demo USE_GL_STUBS=1
	$(MAKE) --no-print-directory color_picker_demo USE_GL_STUBS=1
	$(MAKE) --no-print-directory scene_demo USE_GL_STUBS=1
	$(MAKE) --no-print-directory check
	$(MAKE) --no-print-directory test-stubs
	$(MAKE) --no-print-directory gl-repl
	$(MAKE) --no-print-directory gl-tests
	$(MAKE) --no-print-directory bench

install-hooks: ## Point this clone's git hooks at the tracked .githooks/ directory.
	@git config core.hooksPath .githooks
	@echo "git core.hooksPath -> .githooks (pre-push: check-trailing-whitespace + test-stubs)"

# Benchmark targets ------------------------------------------------------
# Built and invoked separately from `make test` because timing is sensitive
# to system load and we don't want a stray slow run failing CI. Use
# BENCH_ARGS to pass through flags, e.g. `make bench BENCH_ARGS="--iters 20"`.
BENCH_ARGS ?=

capacity-matrix: ## Print state-scaling matrix: per-tunable bytes-per-unit, current totals, and undo/redo ring footprint.
	@$(CC) $(COMMON_CFLAGS) -o build/capacity_matrix tools/capacity_matrix.c
	@./build/capacity_matrix

bench: $(BENCH_BINS) ## Build and run the REPL runtime benchmarks.
	@for b in $(BENCH_BINS); do \
		echo "==> $$b $(BENCH_ARGS)"; \
		$(BINDIR)/$$b $(BENCH_ARGS) || exit $$?; \
	done

bench-csv: $(BENCH_BINS) ## Run benchmarks with --csv output (machine readable).
	@for b in $(BENCH_BINS); do \
		$(BINDIR)/$$b --csv $(BENCH_ARGS) || exit $$?; \
	done

# count lines: $(SRCS) $(HDRS)
lines: $(SRCS) $(HDRS) ## Count SLOC (code/comment/blank) across source and header files.
	@if ! command -v cloc >/dev/null 2>&1; then \
		echo "cloc not found. Install it with:"; \
		echo "  macOS:  brew install cloc"; \
		echo "  Linux:  sudo apt install cloc"; \
		exit 1; \
	fi
	@cloc $(SRCS) $(HDRS) --by-file

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

debug: ## Clean and rebuild everything with debug/ASan flags.
	$(MAKE) clean
	$(MAKE) all BUILD=debug

coverage: ## Clean, rebuild tests with coverage, run suite, generate HTML report.
	$(MAKE) clean
	$(MAKE) test BUILD=coverage TEST_JOBS=1 USE_GL_STUBS=1
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
	rm -rf $(ROOT_BIN_LINKS) gl-repl.dSYM scene_demo.dSYM repl_demo.dSYM editor_demo.dSYM memprof_demo.dSYM variable_panel_demo.dSYM color_picker_demo.dSYM cpuprof_demo.dSYM \
		$(TEST_BINS) $(addsuffix .dSYM,$(TEST_BINS)) \
		$(BENCH_BINS) $(addsuffix .dSYM,$(BENCH_BINS)) \
		build/coverage/lcov.info build/coverage/html \
		build \
		gl-repl.app packaging/macos/gl-repl.icns packaging/macos/gl-repl.iconset \
		callgraph*.mmd callgraph*.dot callgraph*.html callgrind.out*

glut: ## Rebuild using the Apple GLUT framework instead of freeglut.
	$(MAKE) all \
		BUILD="$(BUILD)" \
		CFLAGS="$(CFLAGS) -DUSE_GLUT" \
		GL_LDFLAGS="$(GLUT_GL_LDFLAGS)" \
		FREEGLUT_VENDOR=0

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
	@printf "Immediate-mode REPL — common Make targets\n\n"
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / {d[$$1]=$$2} \
		END {split("gl-repl clean test-stubs test-full help help-details",o," "); \
		for (i=1;i<=6;i++) printf "  %-16s %s\n", o[i], d[o[i]]}' $(MAKEFILE_LIST)
	@printf "\nRun 'make help-details' for all targets, build modes, and runtime/env notes.\n"

help-details: ## Show available targets and build-mode notes.
	@printf "Immediate-mode REPL Make targets\n\n"
	@printf "Build modes:\n"
	@printf "  common flags:  %s\n" "$(COMMON_CFLAGS)" | fold -s -w 100 | sed '1!s/^/                 /'
	@printf "  default:       \$$(common_flags) %s \n" "$(filter-out $(COMMON_CFLAGS),$(RELEASE_CFLAGS))"
	@printf "  debug:         \$$(common_flags) %s \n" "$(filter-out $(COMMON_CFLAGS),$(DEBUG_CFLAGS))"
	@printf "  coverage:      \$$(common_flags) %s \n\n" "$(filter-out $(COMMON_CFLAGS),$(COVERAGE_CFLAGS))"
	@printf "GL stubs:        make test-stubs, or add USE_GL_STUBS=1 to any target.\n"
	@printf "Runtime env:     GLR_NO_POINT_PARAMETER=1 ./gl-repl forces the no-glPointParameterfv\n"
	@printf "                 path (camera-distance glPointSize fallback). Support is otherwise\n"
	@printf "                 auto-detected from the GL context at startup; there is no build\n"
	@printf "                 flag. See ARCHITECTURE.md > Runtime GL Capability Detection.\n"
	@printf "                 GLR_AUDIO_HITCH_MS=N ./gl-repl sets the audio-worker hitch\n"
	@printf "                 threshold (default 50ms; 0 disables). --no-audio skips audio\n"
	@printf "                 init to isolate startup stalls; startup prints an [init +Ns]\n"
	@printf "                 trace per phase.\n"
	@printf "                 GLR_DETAILED_PROF=1 ./gl-repl (or --detailed-prof) promotes\n"
	@printf "                 the optional fine-grained init-trace phases (glutInit split,\n"
	@printf "                 audio playlist sub-steps, first-two-frames triple); default\n"
	@printf "                 off. See ARCHITECTURE.md > Startup & Audio-Worker Diagnostics.\n"
	@printf "Build options:   UI_THEME_DEFAULT=N picks the compile-time UI color scheme\n"
	@printf "                 (0 green default, 1 warm, 2 cyan, 3 amber, 4 violet, 5 mono),\n"
	@printf "                 e.g. make gl-repl CPPFLAGS=-DUI_THEME_DEFAULT=1. Defined in\n"
	@printf "                 config.h, range-checked in src/ui/core/theme.h. See\n"
	@printf "                 ARCHITECTURE.md > UI Color Theming.\n"
	@printf "                 NO_SAN=1 (or NOSAN=1) disables ASan/UBSan sanitizers in debug builds.\n"
	@printf "User CFLAGS are appended to the selected build mode.\n\n"
	@printf "Tests:           make test runs test binaries in parallel; set TEST_JOBS=N to limit jobs.\n\n"
	@printf "Individual tests can still be built directly, e.g. make test_eval or make test_repl_core_io.\n\n"
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / && $$1 !~ /^check-/ {printf "  %-24s %s\n", $$1, $$2}' $(MAKEFILE_LIST) | sort

-include $(DEPS)
