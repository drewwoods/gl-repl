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

ifeq ($(USE_GL_STUBS),1)
GL_HEADER_CFLAGS = \
	-I$(GL_STUB_INCLUDE) \
	-DGL_STUBS
else
GL_HEADER_CFLAGS = \
	-I/usr/include \
	-I/opt/homebrew/include \
	-I$(HOME)/src/freeglut-fork/include
endif

# Language standard: C99, project-wide, no exceptions. Everything
# (sample, tests, demos, bench, CI) compiles -std=c99 so the project
# runs on old machines / old GCC. Non-pedantic by default — GNU
# extensions GCC accepts in -std=c99 are fine; the goal is "old gcc
# compiles it", not pure ISO C99. The shipped/real binaries (sample,
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
	-DGL_SILENCE_DEPRECATION \
	$(GL_HEADER_CFLAGS) \
	-I$(PROJECT_ROOT) \
	-I$(SRC_DIR) \
	-I$(LOCAL_INCLUDE)

RELEASE_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O2

DEBUG_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O0 \
	-fsanitize=address -fno-omit-frame-pointer \
	-fsanitize=undefined -fno-sanitize-recover=undefined

COVERAGE_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O0 \
	--coverage -fprofile-arcs -ftest-coverage

# Test targets build & run under the debug sanitizer build
# (AddressSanitizer + UBSan) by default; sample, bench and the demos
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
# macOS: system frameworks + homebrew / local freeglut fork.
GLUT_GL_LDFLAGS = \
	-L/opt/homebrew/lib -lm -lpthread \
	-framework IOKit -framework Cocoa -framework OpenGL -framework GLUT \
	-framework CoreAudio -framework CoreFoundation -framework AudioToolbox

GL_LDFLAGS = \
	-L/opt/homebrew/lib \
	-L$(HOME)/src/freeglut-fork/build/lib \
	-Wl,-rpath,$(HOME)/src/freeglut-fork/build/lib \
	-lglut -lm -lpthread \
	-framework IOKit -framework Cocoa -framework OpenGL \
	-framework CoreAudio -framework CoreFoundation -framework AudioToolbox

GL_STUB_LDFLAGS = \
	-lm -lpthread \
	-framework CoreAudio -framework CoreFoundation -framework AudioToolbox
else
# Linux: system freeglut + GL/GLU. miniaudio dlopen()s pulseaudio/alsa
# at runtime, so we only need -ldl (plus the existing -lpthread -lm).
GLUT_GL_LDFLAGS = \
	-lglut -lGL -lGLU -lm -lpthread -ldl

GL_LDFLAGS = \
	-lglut -lGL -lGLU -lm -lpthread -ldl

GL_STUB_LDFLAGS = \
	-lm -lpthread -ldl
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
	check-repl-no-direct-buffer-read \
	check-repl-no-direct-editor \
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
	lines \
	sample \
	test \
	test-detailed \
	test-full \
	test-stubs \
	FORCE

all: sample

# Used to force rebuild if you list as a prerequisite, e.g. `test_eval: FORCE $(test_eval_OBJS)`.
FORCE:

SRCS = \
	src/app/glr_audio.c \
	src/repl/format.c \
	prof.c \
	sample.c \
	src/app/glr_actions.c \
	src/app/glr_camera.c \
	src/app/glr_camera_export.c \
	src/app/glr_completion.c \
	src/app/glr_config.c \
	src/app/glr_ctrl.c \
	src/app/glr_debug.c \
	src/app/glr_source_document.c \
	src/app/glr_state.c \
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
	src/editor/services.c \
	src/editor/state.c \
	src/editor/undo.c \
	src/repl/apply.c \
	src/repl/autonormal.c \
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
	src/repl/load.c \
	src/repl/parser.c \
	src/repl/replay_annotations.c \
	src/repl/scenes.c \
	src/repl/source_scope.c \
	src/repl/state.c \
	src/repl/tutorials.c \
	src/scene/axes.c \
	src/scene/backdrop.c \
	src/scene/postprocess_filter.c \
	src/scene/grid.c \
	src/scene/guides/geometry_guides.c \
	src/scene/guides/transform_guides.c \
	src/scene/lights.c \
	src/scene/overlays.c \
	src/scene/render.c \
	src/scene/scene_transition.c \
	src/ui/autocomplete_panel.c \
	src/ui/color_picker.c \
	src/ui/layout.c \
	src/ui/menu_bar.c \
	src/ui/panels.c \
	src/ui/profile_panel.c \
	src/ui/repl_code_panel.c \
	src/ui/replay_hud.c \
	src/ui/scene_tabs.c \
	src/ui/state.c \
	src/ui/tabbed_overlay.c \
	src/ui/text_layout.c \
	src/ui/text_panel.c \
	src/ui/text_search.c \
	src/ui/variable_panel.c \
	src/widgets/color_picker_state.c \
	src/widgets/replay.c \
	src/widgets/replay_state.c \
	src/widgets/tutorial.c \
	src/widgets/tutorial_state.c \
	src/widgets/variable_panel_drag.c \
	src/widgets/variable_panel_state.c \
	tests/gl-stubs/gl_stub_counts.c
HDRS = \
	src/app/glr_audio.h \
	src/repl/format.h \
	prof.h \
	sample.h \
	source_document.h \
	transform_utils.h \
	src/app/glr_actions.h \
	src/app/glr_camera.h \
	src/app/glr_completion.h \
	src/app/glr_config.h \
	src/app/glr_ctrl.h \
	src/app/glr_debug.h \
	src/app/glr_defaults.h \
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
	src/editor/services.h \
	src/editor/state.h \
	src/editor/undo.h \
	src/repl/apply.h \
	src/repl/command_spec.h \
	src/repl/command_store.h \
	src/repl/compile.h \
	src/repl/core.h \
	src/repl/core_internal.h \
	src/repl/eval.h \
	src/repl/example_loader.h \
	src/repl/examples.h \
	src/repl/help_text.h \
	src/repl/parser.h \
	src/repl/pipeline.h \
	src/repl/replay_annotations.h \
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
	src/ui/autocomplete_panel.h \
	src/ui/color_picker.h \
	src/ui/layout.h \
	src/ui/menu_bar.h \
	src/ui/panels.h \
	src/ui/profile_panel.h \
	src/ui/repl_code_panel.h \
	src/ui/replay_hud.h \
	src/ui/scene_tabs.h \
	src/ui/state.h \
	src/ui/state_types.h \
	src/ui/tabbed_overlay.h \
	src/ui/text_layout.h \
	src/ui/text_panel.h \
	src/ui/text_search.h \
	src/ui/theme.h \
	src/ui/variable_panel.h \
	src/widgets/color_picker_state.h \
	src/widgets/replay.h \
	src/widgets/replay_state.h \
	src/widgets/tutorial.h \
	src/widgets/tutorial_state.h \
	src/widgets/variable_panel_drag.h \
	src/widgets/variable_panel_state.h
CORE_TEST_SRCS = \
	src/app/glr_audio.c \
	src/repl/format.c \
	prof.c \
	src/app/glr_actions.c \
	src/app/glr_camera.c \
	src/app/glr_camera_export.c \
	src/app/glr_completion.c \
	src/app/glr_config.c \
	src/app/glr_ctrl.c \
	src/app/glr_debug.c \
	src/app/glr_source_document.c \
	src/app/glr_state.c \
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
	src/editor/services.c \
	src/editor/state.c \
	src/editor/undo.c \
	src/repl/apply.c \
	src/repl/autonormal.c \
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
	src/repl/load.c \
	src/repl/parser.c \
	src/repl/replay_annotations.c \
	src/repl/scenes.c \
	src/repl/source_scope.c \
	src/repl/state.c \
	src/repl/tutorials.c \
	src/scene/axes.c \
	src/scene/backdrop.c \
	src/scene/postprocess_filter.c \
	src/scene/grid.c \
	src/scene/guides/geometry_guides.c \
	src/scene/guides/transform_guides.c \
	src/scene/lights.c \
	src/scene/overlays.c \
	src/scene/render.c \
	src/scene/scene_transition.c \
	src/ui/autocomplete_panel.c \
	src/ui/color_picker.c \
	src/ui/layout.c \
	src/ui/menu_bar.c \
	src/ui/panels.c \
	src/ui/profile_panel.c \
	src/ui/repl_code_panel.c \
	src/ui/replay_hud.c \
	src/ui/scene_tabs.c \
	src/ui/state.c \
	src/ui/tabbed_overlay.c \
	src/ui/text_layout.c \
	src/ui/text_panel.c \
	src/ui/text_search.c \
	src/ui/variable_panel.c \
	src/widgets/color_picker_state.c \
	src/widgets/replay.c \
	src/widgets/replay_state.c \
	src/widgets/tutorial.c \
	src/widgets/tutorial_state.c \
	src/widgets/variable_panel_drag.c \
	src/widgets/variable_panel_state.c \
	tests/gl-stubs/gl_stub_counts.c

REPL_SRCS = $(filter src/repl/%.c,$(SRCS))
SCENE_SRCS = $(filter src/scene/%.c,$(SRCS))
UI_SRCS = $(filter src/ui/%.c,$(SRCS))
SCENE_HDRS = $(filter src/scene/%.h,$(HDRS))
UI_HDRS = $(filter src/ui/%.h,$(HDRS))
STATE_NEUTRAL_SRCS = src/repl/format.c prof.c tests/gl-stubs/gl_stub_counts.c

# Object lists used to build the standalone scene_demo without dragging in
# any REPL editor/controller code. Scene + prof — the scene module no
# longer touches repl_eval (replay-baseline restore is dispatched through a
# function pointer the controller installs; geometry-guide arg parsing is
# done in the controller before snapshot is built).
SCENE_DEMO_DEP_SRCS = $(SCENE_SRCS) prof.c

# Object list for the standalone repl_demo (the inverse of scene_demo:
# proves the REPL pipeline links without editor input dispatch
# (src/editor/input.c), the controller (src/app/glr_ctrl.c + glr_ctrl_router_*),
# the UI (src/ui/*, src/ui/replay_hud.c), or — as of Phase 6 of
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
                     prof.c \
                     src/widgets/replay.c \
                     src/widgets/replay_state.c \
                     src/widgets/tutorial_state.c \
					 src/repl/apply.c \
                     src/repl/autonormal.c \
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
                     src/repl/load.c \
                     src/repl/parser.c \
                     src/repl/replay_annotations.c \
                     src/repl/scenes.c \
                     src/repl/source_scope.c \
                     src/repl/state.c \
                     tools/repl_demo/source_document.c \
                     tests/gl-stubs/gl_stub_counts.c
# src/app/glr_config.c removed in step 4 of the decouple plan: pipeline TUs
# (src/repl/export.c, src/repl/scenes.c, src/repl/example_loader.c) no longer call
# glr_config_*; the controller-installed ReplExportConfigBridge is the
# only path that touches cfg state, and that lives in src/app/glr_actions.c
# (not in the demo link set).

# Object list for the standalone editor_demo. Proves the editor module
# set links without the REPL pipeline, the controller, or the scene
# renderer. Per the "Scope decision" in plans/active/editor-demo.md,
# UI chrome (menu bar, help overlay, color picker, tutorial) is
# editor-inherent and links directly; src/repl/* and src/app/* are
# replaced by tools/editor_demo/repl_shim.c. src/editor/services.c is
# *excluded* — the shim's editor_services_default() takes its place,
# so the demo gets demo-local bindings for compile / apply / parse.
EDITOR_DEMO_DEP_SRCS = src/editor/clipboard.c \
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
                       src/editor/undo.c \
                       src/ui/text_layout.c \
                       src/ui/text_panel.c \
                       src/ui/text_search.c \
                       prof.c \
                       tests/gl-stubs/gl_stub_counts.c

OBJDIR = build/$(BUILD)$(if $(filter 1,$(USE_GL_STUBS)),-gl-stubs,)
BINDIR = $(OBJDIR)
OBJ_CFLAGS = $(BUILD_CFLAGS) $(CFLAGS)
DEPFLAGS = -MMD -MP

SAMPLE_OBJS = $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
CORE_TEST_OBJS = $(addprefix $(OBJDIR)/,$(CORE_TEST_SRCS:.c=.o))

TEST_BINS = \
	test_eval \
	test_format \
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
	test_tutorial_runner

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
endif

CORE_TEST_BINS = $(filter-out test_eval test_format test_repl_code_panel_layout test_ui_theme test_scene_palette test_audio,$(TEST_BINS))

# Benchmark binaries follow the same linking pattern as core test binaries
# (they reuse CORE_TEST_OBJS so they work in both real-GL and stubs builds),
# but they are intentionally NOT in TEST_BINS so `make test` does not run
# them — benchmarks are timing-sensitive and should be invoked explicitly.
BENCH_BINS = bench_repl

ROOT_BIN_LINKS = sample scene_demo repl_demo editor_demo

.PHONY: $(ROOT_BIN_LINKS) $(TEST_BINS) $(BENCH_BINS)

SAMPLE_BIN = $(BINDIR)/sample
SCENE_DEMO_BIN = $(BINDIR)/scene_demo
REPL_DEMO_BIN = $(BINDIR)/repl_demo
EDITOR_DEMO_BIN = $(BINDIR)/editor_demo

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

test_repl_code_panel_layout_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_code_panel_layout.o $(OBJDIR)/src/ui/text_layout.o
test_repl_code_panel_layout_LDLIBS =
test_repl_code_panel_layout_RUN ?= $(BINDIR)/test_repl_code_panel_layout

# Header-only: ui/theme.h pulls in no project objects.
test_ui_theme_OBJS = $(OBJDIR)/$(TEST_DIR)/test_ui_theme.o
test_ui_theme_LDLIBS =
test_ui_theme_RUN ?= $(BINDIR)/test_ui_theme

# Header-only: scene/palette.h pulls in no project objects.
test_scene_palette_OBJS = $(OBJDIR)/$(TEST_DIR)/test_scene_palette.o
test_scene_palette_LDLIBS =
test_scene_palette_RUN ?= $(BINDIR)/test_scene_palette

test_audio_OBJS = $(OBJDIR)/$(TEST_DIR)/test_audio.o $(OBJDIR)/src/app/glr_audio.o
test_audio_LDLIBS = $(GL_LDFLAGS)
test_audio_RUN ?= $(BINDIR)/test_audio

# For tests using the "include-as-unit" pattern (e.g., `#include "file.c"` to test
# internal static functions), we must filter out the original object file from
# the CORE_TEST_OBJS link list to prevent duplicate symbol errors.
test_glr_ctrl_OBJS = $(OBJDIR)/$(TEST_DIR)/test_glr_ctrl.o $(filter-out $(OBJDIR)/src/app/glr_ctrl.o,$(CORE_TEST_OBJS))

test_repl_executor_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_executor.o $(filter-out $(OBJDIR)/src/repl/executor.o,$(CORE_TEST_OBJS))

test_repl_replay_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_replay.o $(filter-out $(OBJDIR)/src/widgets/replay.o,$(CORE_TEST_OBJS))

# test_replay_walk includes app/glr_ctrl.c as a translation unit to reach
# the static cursor_guide_snapshot_with_flat_args helper, so the object
# must be filtered out the same way test_glr_ctrl does.
test_replay_walk_OBJS = $(OBJDIR)/$(TEST_DIR)/test_replay_walk.o $(filter-out $(OBJDIR)/src/app/glr_ctrl.o,$(CORE_TEST_OBJS))

TEST_OBJS = $(foreach test,$(TEST_BINS),$($(test)_OBJS))
TEST_RUNNER_CASES = $(foreach test,$(TEST_BINS),'$(test):::$($(test)_RUN)')
BENCH_OBJS = $(foreach bin,$(BENCH_BINS),$($(bin)_OBJS))

TEST_JOBS ?=

ALL_OBJS = $(sort $(SAMPLE_OBJS) $(TEST_OBJS) $(BENCH_OBJS))

DEPS = $(ALL_OBJS:.o=.d)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(SAMPLE_BIN): $(SAMPLE_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(SAMPLE_OBJS) $(GL_LDFLAGS)

sample: FORCE $(SAMPLE_BIN) ## Build the main REPL sample using release flags by default.
	ln -sfn $(SAMPLE_BIN) $@

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

# Standalone editor demo. Inverse of repl_demo: proves the editor
# module set (src/editor/*, plus the UI / widgets / prof modules it
# legitimately depends on downward) links without src/repl/*,
# src/app/*, or src/scene/*. tools/editor_demo/repl_shim.c provides
# editor_services_default() with no-op bindings plus direct stubs
# for the repl_* / glr_* symbols the editor still calls by name.
# Adding a new direct call site in the editor that grows the shim
# is a regression; the check-editor-repl-surface gate (Phase 7b)
# catches it.
EDITOR_DEMO_OBJS = $(OBJDIR)/tools/editor_demo/editor_demo.o \
                   $(OBJDIR)/tools/editor_demo/repl_shim.o \
                   $(OBJDIR)/tools/editor_demo/menu.o \
                   $(addprefix $(OBJDIR)/,$(EDITOR_DEMO_DEP_SRCS:.c=.o))

$(EDITOR_DEMO_BIN): $(EDITOR_DEMO_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $(EDITOR_DEMO_OBJS) $(GL_LDFLAGS)

editor_demo: FORCE $(EDITOR_DEMO_BIN) ## Build the standalone editor demo.
	ln -sfn $(EDITOR_DEMO_BIN) $@

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
# Link is GL-only (gl_2d.h is header-inline; no project objects needed).
GL_TEST_BINS = test_ui_gl_state

$(BINDIR)/test_ui_gl_state: $(OBJDIR)/$(TEST_DIR)/test_ui_gl_state.o
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) -o $@ $^ $(GL_LDFLAGS)

gl-tests: $(addprefix $(BINDIR)/,$(GL_TEST_BINS)) ## Run real-GL UI state tests (needs a display; excluded from `make test`).
	@for b in $(addprefix $(BINDIR)/,$(GL_TEST_BINS)); do \
	  printf '$(CYAN)==> %s$(NC)\n' "$$b"; "$$b" || exit $$?; \
	done

.PHONY: gl-tests $(GL_TEST_BINS)

# Layering boundary enforcement ------------------------------------------
check-gl-boundaries: ## Verify GL/GLUT calls are isolated to allowed files.
	@echo "    Checking GL/GLU drawing calls isolation..."
	@! grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*[[:space:]]*\(' $(REPL_SRCS) | grep -v '^src/repl/executor\.c:' | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GL/GLU calls found outside src/repl/executor.c$(NC)" && exit 1)
	@echo "    Checking GL/GLU calls in sample.h..."
	@! grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*[[:space:]]*\(' sample.h src/app/glr_defaults.h | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GL/GLU calls found in sample.h$(NC)" && exit 1)
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
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"scene/' $(REPL_SRCS) src/app/glr_ctrl.c \
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
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"ui/' $(REPL_SRCS) src/app/glr_ctrl.c \
		| grep -vE '^src/app/(glr_ctrl|glr_actions)\.c$$|^src/repl/export\.c$$|^repl_editor\.c$$' || true); \
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

check-repl-demo-no-editor: ## Forbid editor implementation in the standalone demo (Phase 7).
	@bash scripts/check-repl-demo-no-editor.sh

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
		check-repl-demo-stubs-shrinking \
		check-repl-no-direct-editor \
		check-repl-demo-no-editor \
		check-editor-repl-surface \
		check-edit-ops-pure \
		check-source-document-port-owners \
		check-c99 \
		check-module-prefixes \
		check-no-test-default-output; do \
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

check-ui-text-panel-pure: ## Verify src/ui/text_panel.* stays REPL/editor-free.
	@bash scripts/check-ui-text-panel-pure.sh

check-editor-repl-surface: ## Ratchet direct repl_* call surface in src/editor/input.c and commit.c.
	@bash scripts/check-editor-repl-surface.sh scripts/baselines/editor-repl-surface.txt

check-edit-ops-pure: ## Verify src/editor/edit_ops.* stays REPL-free (Phase 8 generic-primitives invariant).
	@bash scripts/check-edit-ops-pure.sh

check-ui-panels-no-mutators: ## Hard guard: src/ui/panels.c references no input-dispatch mutators (Phase J2.2).
	@bash scripts/check-ui-panels-no-mutators.sh

check-replay-ui-isolation: ## Hard guard: replay_ui_*.c is feature-UI — no editor / REPL mutators or parser/compile/apply calls.
	@bash scripts/check-replay-ui-isolation.sh

check-color-picker-ui-isolation: ## Strict guard: src/ui/color_picker.c is pure renderer/hit-test over ColorPickerView — no mutators, no live state reads, no parser/compile/apply.
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

check-module-prefixes: ## Verify stale pre-cleanup symbol prefixes have not reappeared under src/.
	@bash scripts/check-module-prefixes.sh

check-repl-demo-stubs-shrinking: ## Ratchet on tools/repl_demo/stubs.c — must not grow past 0 stubs.
	@bash scripts/check-repl-demo-stubs-shrinking.sh

check-c99: ## C99 build guard: sample + bench + demo sources must syntax-check under gcc -std=c99 (non-pedantic; tests excluded; in the standard gate).
	@C99_SRCS='$(SRCS)' bash scripts/check-c99.sh

check-no-test-default-output: ## Hard guard: tests may not call repl_save_default_output() (writes ./output.c in repo root).
	@bash scripts/check-no-test-default-output.sh

CHECK_TARGETS = \
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

test-stubs: check-gl-boundaries check-layer-coupling check-state-ownership ## Build and run tests using local GL/GLU/GLUT stubs, without GL libs.
	$(MAKE) test USE_GL_STUBS=1

test-full: ## Full gate: stub tests + checks + build sample, bench, repl_demo, scene_demo.
	$(MAKE) --no-print-directory repl_demo USE_GL_STUBS=1
	$(MAKE) --no-print-directory check
	$(MAKE) --no-print-directory test-stubs
	$(MAKE) --no-print-directory sample
	$(MAKE) --no-print-directory bench
	$(MAKE) --no-print-directory scene_demo

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
	rm -rf $(ROOT_BIN_LINKS) sample.dSYM scene_demo.dSYM repl_demo.dSYM editor_demo.dSYM \
		$(TEST_BINS) $(addsuffix .dSYM,$(TEST_BINS)) \
		$(BENCH_BINS) $(addsuffix .dSYM,$(BENCH_BINS)) \
		build/coverage/lcov.info build/coverage/html \
		build \
		callgraph*.mmd callgraph*.dot callgraph*.html callgrind.out*

glut: ## Rebuild using the Apple GLUT framework instead of freeglut.
	$(MAKE) clean
	$(MAKE) all \
		BUILD="$(BUILD)" \
		CFLAGS="$(CFLAGS) -DUSE_GLUT" \
		GL_LDFLAGS="$(GLUT_GL_LDFLAGS)"

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

callgraph-profile: sample ## Generate profile-based call graph using Valgrind callgrind.
	@if ! command -v valgrind &> /dev/null; then \
		echo "ERROR: valgrind not found. Install with: brew install valgrind"; exit 1; \
	fi
	@if ! command -v callgrind_annotate &> /dev/null; then \
		echo "ERROR: callgrind_annotate not found (part of valgrind)"; exit 1; \
	fi
	@if [ -z "$(PROG)" ]; then \
		echo "Running sample with no args for default 5 seconds..."; \
		PROG="./sample"; \
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
		END {split("sample clean test-stubs test-full help help-details",o," "); \
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
	@printf "Runtime env:     GLR_NO_POINT_PARAMETER=1 ./sample forces the no-glPointParameterfv\n"
	@printf "                 path (camera-distance glPointSize fallback). Support is otherwise\n"
	@printf "                 auto-detected from the GL context at startup; there is no build\n"
	@printf "                 flag. See ARCHITECTURE.md > Runtime GL Capability Detection.\n"
	@printf "                 GLR_AUDIO_HITCH_MS=N ./sample sets the audio-worker hitch\n"
	@printf "                 threshold (default 50ms; 0 disables). --no-audio skips audio\n"
	@printf "                 init to isolate startup stalls; startup prints an [init +Ns]\n"
	@printf "                 trace per phase. See ARCHITECTURE.md > Startup & Audio-Worker\n"
	@printf "                 Diagnostics.\n"
	@printf "Build options:   UI_THEME_DEFAULT=N picks the compile-time UI color scheme\n"
	@printf "                 (0 green default, 1 warm, 2 cyan, 3 amber, 4 violet, 5 mono),\n"
	@printf "                 e.g. make sample CPPFLAGS=-DUI_THEME_DEFAULT=1. Defined in\n"
	@printf "                 config.h, range-checked in src/ui/theme.h. See\n"
	@printf "                 ARCHITECTURE.md > UI Color Theming.\n"
	@printf "User CFLAGS are appended to the selected build mode.\n\n"
	@printf "Tests:           make test runs test binaries in parallel; set TEST_JOBS=N to limit jobs.\n\n"
	@printf "Individual tests can still be built directly, e.g. make test_eval or make test_repl_core_io.\n\n"
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / && $$1 !~ /^check-/ {printf "  %-24s %s\n", $$1, $$2}' $(MAKEFILE_LIST) | sort

-include $(DEPS)
