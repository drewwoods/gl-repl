CC = gcc
PROJECT_ROOT := $(abspath .)
LOCAL_INCLUDE := $(abspath include)
GL_STUB_INCLUDE := $(abspath tests/gl-stubs/include)
TEST_DIR := tests
BENCH_DIR := bench

UNAME_S := $(shell uname -s)

ifeq ($(USE_GL_STUBS),1)
GL_HEADER_CFLAGS = \
	-I$(GL_STUB_INCLUDE) \
	-DOPENGL_VIBE_USE_GL_STUBS
else
GL_HEADER_CFLAGS = \
	-I/usr/include \
	-I/opt/homebrew/include \
	-I$(HOME)/src/freeglut-fork/include
endif

COMMON_CFLAGS = \
	-Wall -ggdb -g3 \
	-Wno-deprecated-declarations -Wfloat-conversion \
	-std=c2x -DGL_SILENCE_DEPRECATION \
	$(GL_HEADER_CFLAGS) \
	-I$(PROJECT_ROOT) \
	-I$(LOCAL_INCLUDE)

RELEASE_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O2

DEBUG_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O0 \
	-fsanitize=address -fno-omit-frame-pointer

COVERAGE_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O0 \
	--coverage -fprofile-arcs -ftest-coverage

BUILD ?= release

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

ifeq ($(NO_POINT_PARAMETER),1)
CFLAGS += -DNO_POINT_PARAMETER
endif

ifeq ($(BUILD),coverage)
COVERAGE_LDFLAGS = --coverage
else
COVERAGE_LDFLAGS =
endif

.PHONY: all clean test check test-detailed test_detailed test-stubs lines debug coverage glut help bench bench-csv check-gl-boundaries check-layer-coupling check-controller-boundaries check-scene-no-repl-state-mut check-state-boundaries check-views-no-owners check-pure-scene-no-repl-state check-ui-no-repl-state-mut check-public-api-usage check-state-ownership check-no-write-through-view check-runtime-state-value-fields check-public-state-no-writable-pointers check-views-flat-types check-state-read-getters-return-values check-views-by-value-snapshot check-ui-renderer-takes-view check-renderer-no-direct-mutators check-output-actualization check-state-c-shrinking check-no-facade-include-in-views check-domain-owner-encapsulation check-cursor-px-encapsulated check-ui-no-repl-state-read check-editor-ownership-budget check-no-store-text-api check-repl-no-direct-buffer-read check-imrepl-not-editor-mirror check-ui-returns-hits-only audit-editor-ownership callgraph-static callgraph-static-entry callgraph-profile callgraph-graphviz callgraph-html callgraph-files FORCE

all: sample

# Used to force rebuild if you list as a prerequisite, e.g. `test_eval: FORCE $(test_eval_OBJS)`.
FORCE:

SRCS = sample.c imrepl_ctrl.c repl_core.c repl_debug.c repl_state.c editor_state.c editor_commit.c editor_services.c editor_input.c ui_state.c variable_panel.c repl_config.c repl_command_spec.c repl_parser.c repl_source_scope.c repl_command_store.c repl_commit.c repl_compile.c repl_apply.c repl_clipboard.c repl_undo.c repl_camera_controls.c repl_actions.c repl_layout.c repl_code_panel_layout.c repl_code_panel_document.c repl_flatten.c repl_executor.c repl_autocomplete.c ui_autocomplete_panel.c repl_autonormal.c repl_scenes.c repl_example_loader.c repl_replay.c repl_replay_annotations.c repl_search.c repl_export.c repl_editor.c repl_examples.c scene_render.c scene_geometry_guides.c scene_transform_guides.c scene_grid.c scene_axes.c scene_backdrop.c scene_lights.c scene_overlays.c ui_panels.c ui_menu_bar.c ui_color_picker.c ui_help_overlay.c ui_variable_panel.c ui_replay_hud.c repl_var_drag.c repl_inline_rename.c repl_eval.c cmd_format.c repl_audio.c ui_profile_panel.c prof.c tests/gl-stubs/gl_stub_counts.c
HDRS = sample.h repl_search.h imrepl_ctrl.h repl_state.h editor_state.h editor_commit.h editor_services.h editor_input.h ui_state.h variable_panel.h repl_config.h repl_core.h repl_core_internal.h repl_debug.h repl_command_spec.h repl_parser.h repl_source_scope.h repl_command_store.h repl_compile.h repl_apply.h repl_layout.h repl_pipeline.h repl_clipboard.h repl_undo.h repl_camera_controls.h repl_actions.h repl_code_panel_layout.h repl_code_panel_document.h repl_replay.h repl_replay_annotations.h repl_examples.h scene_render_types.h scene_guides_shared.h scene_geometry_guides.h scene_transform_guides.h scene_transform_utils.h scene_grid.h scene_axes.h scene_render.h scene_backdrop.h scene_lights.h scene_overlays.h ui_panels.h ui_menu_bar.h ui_color_picker.h ui_help_overlay.h ui_variable_panel.h ui_replay_hud.h repl_var_drag.h ui_autocomplete_panel.h repl_inline_rename.h repl_eval.h cmd_format.h repl_audio.h ui_profile_panel.h prof.h
CORE_TEST_SRCS = repl_core.c imrepl_ctrl.c repl_debug.c repl_state.c editor_state.c editor_commit.c editor_services.c editor_input.c ui_state.c variable_panel.c repl_config.c repl_command_spec.c repl_parser.c repl_source_scope.c repl_command_store.c repl_commit.c repl_compile.c repl_apply.c repl_clipboard.c repl_undo.c repl_camera_controls.c repl_actions.c repl_layout.c repl_code_panel_layout.c repl_code_panel_document.c repl_flatten.c repl_executor.c repl_autocomplete.c ui_autocomplete_panel.c repl_autonormal.c repl_scenes.c repl_example_loader.c repl_replay.c repl_replay_annotations.c repl_search.c repl_export.c repl_editor.c repl_examples.c scene_render.c scene_geometry_guides.c scene_transform_guides.c scene_grid.c scene_axes.c scene_backdrop.c scene_lights.c scene_overlays.c ui_panels.c ui_menu_bar.c ui_color_picker.c ui_help_overlay.c ui_variable_panel.c ui_replay_hud.c repl_var_drag.c repl_inline_rename.c repl_eval.c cmd_format.c repl_audio.c ui_profile_panel.c prof.c tests/gl-stubs/gl_stub_counts.c

REPL_SRCS = $(filter repl_%.c,$(SRCS))
SCENE_SRCS = $(filter scene_%.c,$(SRCS))
UI_SRCS = $(filter ui_%.c,$(SRCS))
SCENE_HDRS = $(filter scene_%.h,$(HDRS))
UI_HDRS = $(filter ui_%.h,$(HDRS))
STATE_NEUTRAL_SRCS = cmd_format.c prof.c tests/gl-stubs/gl_stub_counts.c

OBJDIR = build/$(BUILD)$(if $(filter 1,$(USE_GL_STUBS)),-gl-stubs,)
OBJ_CFLAGS = $(BUILD_CFLAGS) $(CFLAGS)
DEPFLAGS = -MMD -MP

SAMPLE_OBJS = $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
CORE_TEST_OBJS = $(addprefix $(OBJDIR)/,$(CORE_TEST_SRCS:.c=.o))

TEST_BINS = \
	test_eval \
	test_format \
	test_repl_state \
	test_repl_code_panel_layout \
	test_repl_code_panel_document \
	test_repl_core_parse \
	test_repl_core_format \
	test_repl_core_commit \
	test_repl_core_io \
	test_repl_core_examples \
	test_repl_core_search \
	test_repl_core_search_extra \
	test_repl_audio \
	test_repl_core_internal \
	test_repl_autocomplete \
	test_repl_command_store \
	test_repl_var_drag \
	test_scene_guides \
	test_scene_render \
	test_imrepl_ctrl \
	test_repl_editor \
	test_repl_core_extra \
	test_repl_autonormal \
	test_repl_replay \
	test_repl_compile

ifeq ($(USE_GL_STUBS),1)
TEST_BINS += test_ui
TEST_BINS += test_repl_actions
TEST_BINS += test_repl_executor
endif

CORE_TEST_BINS = $(filter-out test_eval test_format test_repl_code_panel_layout test_repl_audio,$(TEST_BINS))

# Benchmark binaries follow the same linking pattern as core test binaries
# (they reuse CORE_TEST_OBJS so they work in both real-GL and stubs builds),
# but they are intentionally NOT in TEST_BINS so `make test` does not run
# them — benchmarks are timing-sensitive and should be invoked explicitly.
BENCH_BINS = bench_repl

define core_test_binary
$(1)_OBJS = $$(OBJDIR)/$$(TEST_DIR)/$(1).o $$(CORE_TEST_OBJS)
$(1)_LDLIBS = $$(GL_LDFLAGS)
$(1)_RUN ?= ./$(1)
endef

define bench_binary
$(1)_OBJS = $$(OBJDIR)/$$(BENCH_DIR)/$(1).o $$(CORE_TEST_OBJS)
$(1)_LDLIBS = $$(GL_LDFLAGS)
$(1)_RUN ?= ./$(1)
endef

$(foreach test,$(CORE_TEST_BINS),$(eval $(call core_test_binary,$(test))))
$(foreach bin,$(BENCH_BINS),$(eval $(call bench_binary,$(bin))))

test_eval_OBJS = $(OBJDIR)/$(TEST_DIR)/test_eval.o $(OBJDIR)/repl_eval.o
test_eval_LDLIBS = -lm -lpthread
test_eval_RUN = ./test_eval --run-tests

test_format_OBJS = $(OBJDIR)/$(TEST_DIR)/test_format.o $(OBJDIR)/cmd_format.o
test_format_LDLIBS = -lm
test_format_RUN ?= ./test_format

test_repl_code_panel_layout_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_code_panel_layout.o $(OBJDIR)/repl_code_panel_layout.o
test_repl_code_panel_layout_LDLIBS =
test_repl_code_panel_layout_RUN ?= ./test_repl_code_panel_layout

test_repl_audio_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_audio.o $(OBJDIR)/repl_audio.o
test_repl_audio_LDLIBS = $(GL_LDFLAGS)
test_repl_audio_RUN ?= ./test_repl_audio

# For tests using the "include-as-unit" pattern (e.g., `#include "file.c"` to test
# internal static functions), we must filter out the original object file from
# the CORE_TEST_OBJS link list to prevent duplicate symbol errors.
test_imrepl_ctrl_OBJS = $(OBJDIR)/$(TEST_DIR)/test_imrepl_ctrl.o $(filter-out $(OBJDIR)/imrepl_ctrl.o,$(CORE_TEST_OBJS))

test_repl_executor_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_executor.o $(filter-out $(OBJDIR)/repl_executor.o,$(CORE_TEST_OBJS))

test_repl_replay_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_replay.o $(filter-out $(OBJDIR)/repl_replay.o,$(CORE_TEST_OBJS))

TEST_OBJS = $(foreach test,$(TEST_BINS),$($(test)_OBJS))
TEST_RUNNER_CASES = $(foreach test,$(TEST_BINS),'$(test):::$($(test)_RUN)')
BENCH_OBJS = $(foreach bin,$(BENCH_BINS),$($(bin)_OBJS))

TEST_JOBS ?=

ALL_OBJS = $(sort $(SAMPLE_OBJS) $(TEST_OBJS) $(BENCH_OBJS))

DEPS = $(ALL_OBJS:.o=.d)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(OBJ_CFLAGS) $(DEPFLAGS) -c -o $@ $<

sample: $(SAMPLE_OBJS) ## Build the main REPL sample using release flags by default.
	$(CC) $(OBJ_CFLAGS) -o $@ $(SAMPLE_OBJS) $(GL_LDFLAGS)

.SECONDEXPANSION:

# GNU make normally expands prerequisites before it knows the concrete target.
# .SECONDEXPANSION adds a second pass after `$@` is known, so this one rule can
# turn `test_eval` into `$(test_eval_OBJS)`, `test_repl_core_io` into
# `$(test_repl_core_io_OBJS)`, etc. The doubled dollars delay that lookup until
# the second pass.
$(TEST_BINS) $(BENCH_BINS): %: $$($$@_OBJS)
	$(CC) $(OBJ_CFLAGS) -o $@ $^ $($@_LDLIBS) $(COVERAGE_LDFLAGS)

# Layering boundary enforcement ------------------------------------------
check-gl-boundaries: ## Verify GL/GLUT calls are isolated to allowed files.
	@echo "Checking GL/GLU drawing calls isolation..."
	@! grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*[[:space:]]*\(' $(REPL_SRCS) | grep -v '^repl_executor\.c:' | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "ERROR: GL/GLU calls found outside repl_executor.c" && exit 1)
	@echo "Checking GL/GLU calls in sample.h..."
	@! grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*[[:space:]]*\(' sample.h | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "ERROR: GL/GLU calls found in sample.h" && exit 1)
	@echo "Checking GLUT input/feedback calls isolation..."
	@! grep -nE '\bglut[A-Z][A-Za-z0-9]*[[:space:]]*\(' $(REPL_SRCS) | grep -vE '^repl_(editor|executor)\.c:' | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "ERROR: GLUT calls found outside repl_editor.c and repl_executor.c" && exit 1)
	@echo "GL/GLUT boundaries OK"

check-layer-coupling: ## Verify UI and scene layers don't include each other's headers.
	@echo "Checking UI/scene layer coupling..."
	@! grep -nE '#include\s+"scene_' $(UI_SRCS) $(UI_HDRS) || (echo "ERROR: UI files must not include scene headers" && exit 1)
	@! grep -nE '#include\s+"ui_' $(SCENE_SRCS) $(SCENE_HDRS) || (echo "ERROR: scene files must not include UI headers" && exit 1)
	@echo "Layer coupling OK"


check-controller-boundaries: ## Verify controller owns the scene/UI wiring boundary.
	@echo "Checking controller boundaries..."
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"scene_' $(REPL_SRCS) imrepl_ctrl.c \
		| grep -v '^imrepl_ctrl\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: scene headers included outside imrepl_ctrl.c:"; \
		echo "$$bad"; exit 1; \
	fi
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"ui_' $(REPL_SRCS) imrepl_ctrl.c \
		| grep -vE '^(imrepl_ctrl|repl_(actions|editor|export))\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: new ui headers included outside approved exceptions:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "Controller boundaries OK"

check-scene-no-repl-state-mut: ## Verify scene code does not mutate REPL state directly.
	@echo "Checking scene renderers do not mutate REPL state..."
	@bad=$$(grep -nE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' $(SCENE_SRCS) || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: scene files mutate REPL state:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "Scene mutation boundary OK"

check-pure-scene-no-repl-state: ## Verify scene files do not reach into REPL state/replay APIs.
	@echo "Checking scene files do not reach into REPL state/replay APIs..."
	@bad=$$(grep -nE 'repl_(state|replay)_' $(SCENE_SRCS) || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: scene files reach into REPL state/replay APIs:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "Pure-scene boundary OK"

check-state-boundaries: ## Verify REPL state facade usage stays in owned modules.
	@echo "Checking state facade boundaries..."
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"repl_state\.h"' $(SCENE_SRCS) $(STATE_NEUTRAL_SRCS) 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: scene or state-neutral files include repl_state.h:"; \
		echo "$$bad"; exit 1; \
	fi
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"repl_core_internal\.h"' \
		imrepl_ctrl.c $(SCENE_SRCS) $(UI_SRCS) $(STATE_NEUTRAL_SRCS) 2>/dev/null \
		| grep -vE '^ui_(color_picker|panels)\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: unapproved view/utility files include repl_core_internal.h:"; \
		echo "$$bad"; exit 1; \
	fi
	@bad=$$(grep -lE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' $(UI_SRCS) 2>/dev/null \
		| grep -vE '^(ui_(color_picker|help_overlay|panels))\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: unapproved UI files mutate REPL state directly:"; \
		echo "$$bad"; exit 1; \
	fi
	@bad=$$(grep -nE 'repl_(state|replay)_' $(SCENE_SRCS) 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: scene files reach into REPL state/replay APIs:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "State facade boundaries OK"

check-views-no-owners: ## Verify scene/UI files do not include repl_state_owners.h.
	@echo "Checking scene/UI view files do not include repl_state_owners.h..."
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"repl_state_owners\.h"' $(SCENE_SRCS) $(UI_SRCS) 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: scene/UI view files include repl_state_owners.h:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "View-file ownership boundary OK"

check-ui-no-repl-state-mut: ## Verify UI files do not mutate REPL state directly.
	@echo "Checking UI files do not mutate REPL state directly..."
	@bad=$$(grep -nE 'repl_state_[A-Za-z0-9_]*_mut[[:space:]]*\(' $(UI_SRCS) || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: UI files mutate REPL state:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "UI mutation boundary OK"

check-no-write-through-view: ## Verify no writes happen through pointer fields on view structs.
	@bash scripts/check-no-write-through-view.sh scripts/allowlists/write-through-view.txt $(UI_SRCS) $(SCENE_SRCS)

check-runtime-state-value-fields: ## Verify ReplRuntimeState owns values, not pointer aliases.
	@bash scripts/check-runtime-state-value-fields.sh repl_state.h

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

check-state-c-shrinking: ## Ratchet repl_state.c line count down over time.
	@bash scripts/check-state-c-shrinking.sh scripts/baselines/state-c-lines.txt repl_state.c

check-no-facade-include-in-views: ## Verify view/render files avoid repl_state facade headers.
	@bash scripts/check-no-facade-include-in-views.sh scripts/allowlists/facade-includes-in-views.txt

check-domain-owner-encapsulation: ## Enforce per-domain mutator encapsulation rules as domains migrate.
	@bash scripts/check-domain-encapsulation.sh scripts/allowlists/domain-owner-encapsulation.txt

check-cursor-px-encapsulated: ## Verify cursor pixel wiring is limited to allowlisted migration files.
	@bash scripts/check-cursor-px-encapsulated.sh scripts/allowlists/cursor-px-encapsulated.txt

check-ui-no-repl-state-read: ## Verify UI renderers consume the UiRenderSnapshot, not live repl_state_*().
	@echo "Checking UI render entry points consume UiRenderSnapshot..."
	@bad=$$(grep -nE 'repl_state_[A-Za-z0-9_]+\s*\(' $(UI_SRCS) 2>/dev/null \
		| grep -v -E 'ui_(autocomplete_panel|color_picker|help_overlay|menu_bar|panels|profile_panel|variable_panel|replay_hud)\.c:' \
		|| true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: ui_*.c files outside the input-bridge allowlist read live repl_state_*():"; \
		echo "$$bad"; exit 1; \
	fi
	@bash scripts/check-ui-renderer-signatures.sh scripts/allowlists/ui-renderers-signature.txt
	@echo "ui-no-repl-state-read OK"

check-state-ownership: ## Run state-ownership contract checks (new + tightened existing checks).
	@set -e; \
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
		check-cursor-px-encapsulated \
		check-ui-no-repl-state-read \
		check-editor-ownership-budget \
		check-no-store-text-api \
		check-repl-no-direct-buffer-read \
		check-imrepl-not-editor-mirror \
		check-ui-returns-hits-only; do \
		$(MAKE) --no-print-directory $$target || exit $$?; \
	done

check-public-api-usage: ## Scan public API declarations for unused functions (informational).
	@bash scripts/check-unused-apis.sh

audit-editor-ownership: ## Report editor/REPL/UI ownership drift (informational; see feature/editor-owns-text-completion.md).
	@bash scripts/audit_editor_ownership.sh

check-editor-ownership-budget: ## Ratchet the editor/UI transitional-coupling budget down only.
	@bash scripts/check-editor-ownership-budget.sh scripts/baselines/editor-ownership-budget.txt

check-no-store-text-api: ## Verify repl_command_store_*_with_line[s] API stays gone.
	@bash scripts/check-no-store-text-api.sh

check-repl-no-direct-buffer-read: ## Verify repl_*.c readers go through EditorBufferView, not editor_buffer_line().
	@bash scripts/check-repl-no-direct-buffer-read.sh scripts/allowlists/repl-no-direct-buffer-read.txt

check-imrepl-not-editor-mirror: ## Verify imrepl_ctrl does not grow per-field editor wrappers.
	@bash scripts/check-imrepl-not-editor-mirror.sh

check-ui-returns-hits-only: ## Verify ui_*.c input helpers do not call REPL/editor mutators (ratchet down only).
	@bash scripts/check-ui-returns-hits-only.sh scripts/baselines/ui-returns-hits-only.txt

CHECK_TARGETS = \
	check-gl-boundaries \
	check-layer-coupling \
	check-state-ownership \
	check-public-api-usage

check: ## Run all checks.
	@set -e; \
	for target in $(CHECK_TARGETS); do \
		desc=$$(awk -v target="$$target" 'BEGIN {FS = ":.*## "} $$1 == target { print $$2; exit }' $(firstword $(MAKEFILE_LIST))); \
		printf "\n==> %s\n" "$$desc"; \
		$(MAKE) --no-print-directory $$target || exit $$?; \
	done

test: check-gl-boundaries check-layer-coupling check-state-ownership $(TEST_BINS) ## Run the full automated test suite.
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

test_detailed: test-detailed ## Alias for test-detailed.

test-stubs: ## Build and run tests using local GL/GLU/GLUT stubs, without GL libs.
	$(MAKE) test USE_GL_STUBS=1

# Benchmark targets ------------------------------------------------------
# Built and invoked separately from `make test` because timing is sensitive
# to system load and we don't want a stray slow run failing CI. Use
# BENCH_ARGS to pass through flags, e.g. `make bench BENCH_ARGS="--iters 20"`.
BENCH_ARGS ?=

bench: $(BENCH_BINS) ## Build and run the REPL runtime benchmarks.
	@for b in $(BENCH_BINS); do \
		echo "==> $$b $(BENCH_ARGS)"; \
		./$$b $(BENCH_ARGS) || exit $$?; \
	done

bench-csv: $(BENCH_BINS) ## Run benchmarks with --csv output (machine readable).
	@for b in $(BENCH_BINS); do \
		./$$b --csv $(BENCH_ARGS) || exit $$?; \
	done

# count lines: $(SRCS) $(HDRS)
lines: $(SRCS) $(HDRS) ## Count lines across source and header files.
	@echo "Counting lines of code in source and header files..."
	@wc -l $(SRCS) $(HDRS) | sort -nr

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
ANALYZE_EXCLUDE ?= repl_audio.c
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
	rm -rf sample sample.dSYM \
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

help: ## Show available targets and build-mode notes.
	@printf "Immediate-mode REPL Make targets\n\n"
	@printf "Build modes:\n"
	@printf "  common flags:  %s\n" "$(COMMON_CFLAGS)" | fold -s -w 100 | sed '1!s/^/                 /'
	@printf "  default:       \$$(common_flags) %s \n" "$(filter-out $(COMMON_CFLAGS),$(RELEASE_CFLAGS))"
	@printf "  debug:         \$$(common_flags) %s \n" "$(filter-out $(COMMON_CFLAGS),$(DEBUG_CFLAGS))"
	@printf "  coverage:      \$$(common_flags) %s \n\n" "$(filter-out $(COMMON_CFLAGS),$(COVERAGE_CFLAGS))"
	@printf "GL stubs:        make test-stubs, or add USE_GL_STUBS=1 to any target.\n"
	@printf "No PointParameter: add NO_POINT_PARAMETER=1 to disable glPointParameterfv and use\n"
	@printf "                 glPointSize(5/cam_dist) as a per-frame distance-attenuation fallback.\n"
	@printf "User CFLAGS are appended to the selected build mode.\n\n"
	@printf "Tests:           make test runs test binaries in parallel; set TEST_JOBS=N to limit jobs.\n\n"
	@printf "Individual tests can still be built directly, e.g. make test_eval or make test_repl_core_io.\n\n"
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / && $$1 !~ /^check-/ {printf "  %-24s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

-include $(DEPS)
