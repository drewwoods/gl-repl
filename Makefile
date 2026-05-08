CC = gcc
PROJECT_ROOT := $(abspath .)
LOCAL_INCLUDE := $(abspath include)
SRC_DIR := $(abspath src)
GL_STUB_INCLUDE := $(abspath tests/gl-stubs/include)
TEST_DIR := tests
BENCH_DIR := bench

# Color codes for output
RED := \033[0;31m
GREEN := \033[0;32m
YELLOW := \033[0;33m
CYAN := \033[0;36m
NC := \033[0m  # No Color

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
	-I$(SRC_DIR) \
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

.PHONY: all clean test check test-detailed test_detailed test-stubs lines debug coverage glut help bench bench-csv check-gl-boundaries check-layer-coupling check-controller-boundaries check-scene-no-repl-state-mut check-state-boundaries check-views-no-owners check-pure-scene-no-repl-state check-ui-no-repl-state-mut check-public-api-usage check-state-ownership check-no-write-through-view check-runtime-state-value-fields check-public-state-no-writable-pointers check-views-flat-types check-state-read-getters-return-values check-views-by-value-snapshot check-ui-renderer-takes-view check-renderer-no-direct-mutators check-output-actualization check-state-c-shrinking check-no-facade-include-in-views check-domain-owner-encapsulation check-ui-no-repl-state-read check-editor-ownership-budget check-no-store-text-api check-repl-no-direct-buffer-read check-imrepl-not-editor-mirror check-ui-returns-hits-only check-ui-panels-no-mutators check-replay-ui-isolation check-color-picker-ui-isolation check-variable-panel-forwarders check-replay-forwarders check-no-repl-commit check-no-repl-editor-input-shim check-no-set-status-in-repl-parser check-no-set-status-in-compile-apply check-no-test-default-output audit-editor-ownership callgraph-static callgraph-static-entry callgraph-profile callgraph-graphviz callgraph-html callgraph-files FORCE

all: sample

# Used to force rebuild if you list as a prerequisite, e.g. `test_eval: FORCE $(test_eval_OBJS)`.
FORCE:

SRCS = sample.c imrepl_ctrl.c repl_core.c repl_debug.c repl_state.c editor_state.c editor_commit.c editor_services.c editor_input.c editor_help_session.c editor_completion.c src/ui/state.c variable_panel_state.c replay_state.c repl_config.c repl_command_spec.c repl_parser.c repl_source_scope.c repl_command_store.c repl_compile.c repl_apply.c editor_clipboard.c editor_undo.c repl_camera_controls.c repl_actions.c src/ui/layout.c src/ui/code_panel_layout.c editor_code_panel_document.c repl_flatten.c repl_executor.c editor_autocomplete.c src/ui/autocomplete_panel.c repl_autonormal.c repl_scenes.c repl_example_loader.c replay.c repl_replay_annotations.c editor_search.c repl_export.c repl_examples.c src/scene/render.c geometry_guides.c transform_guides.c src/scene/grid.c src/scene/axes.c src/scene/backdrop.c src/scene/lights.c src/scene/overlays.c src/ui/panels.c src/ui/menu_bar.c color_picker_ui.c color_picker.c src/ui/tabbed_overlay.c repl_help_text.c src/ui/variable_panel.c replay_ui_hud.c variable_panel_drag.c editor_inline_rename.c repl_eval.c cmd_format.c repl_audio.c src/ui/profile_panel.c prof.c tests/gl-stubs/gl_stub_counts.c
HDRS = sample.h editor_search.h imrepl_ctrl.h repl_state.h editor_state.h editor_commit.h editor_services.h editor_input.h editor_help_session.h editor_completion.h src/ui/state.h src/ui/state_types.h variable_panel_state.h replay_state.h repl_config.h repl_core.h repl_core_internal.h repl_debug.h repl_command_spec.h repl_parser.h repl_source_scope.h repl_command_store.h repl_compile.h repl_apply.h src/ui/layout.h repl_pipeline.h editor_clipboard.h editor_undo.h repl_camera_controls.h repl_actions.h src/ui/code_panel_layout.h editor_code_panel_document.h replay.h repl_replay_annotations.h repl_examples.h src/scene/render_types.h guides_shared.h geometry_guides.h transform_guides.h transform_utils.h outline_offset.h src/scene/grid.h src/scene/axes.h src/scene/render.h src/scene/backdrop.h src/scene/lights.h src/scene/overlays.h src/ui/panels.h src/ui/menu_bar.h color_picker_ui.h color_picker.h src/ui/tabbed_overlay.h repl_help_text.h src/ui/variable_panel.h replay_ui_hud.h variable_panel_drag.h src/ui/autocomplete_panel.h editor_inline_rename.h repl_eval.h cmd_format.h repl_audio.h src/ui/profile_panel.h prof.h
CORE_TEST_SRCS = repl_core.c imrepl_ctrl.c repl_debug.c repl_state.c editor_state.c editor_commit.c editor_services.c editor_input.c editor_help_session.c editor_completion.c src/ui/state.c variable_panel_state.c replay_state.c repl_config.c repl_command_spec.c repl_parser.c repl_source_scope.c repl_command_store.c repl_compile.c repl_apply.c editor_clipboard.c editor_undo.c repl_camera_controls.c repl_actions.c src/ui/layout.c src/ui/code_panel_layout.c editor_code_panel_document.c repl_flatten.c repl_executor.c editor_autocomplete.c src/ui/autocomplete_panel.c repl_autonormal.c repl_scenes.c repl_example_loader.c replay.c repl_replay_annotations.c editor_search.c repl_export.c repl_examples.c src/scene/render.c geometry_guides.c transform_guides.c src/scene/grid.c src/scene/axes.c src/scene/backdrop.c src/scene/lights.c src/scene/overlays.c src/ui/panels.c src/ui/menu_bar.c color_picker_ui.c color_picker.c src/ui/tabbed_overlay.c repl_help_text.c src/ui/variable_panel.c replay_ui_hud.c variable_panel_drag.c editor_inline_rename.c repl_eval.c cmd_format.c repl_audio.c src/ui/profile_panel.c prof.c tests/gl-stubs/gl_stub_counts.c

REPL_SRCS = $(filter repl_%.c,$(SRCS))
SCENE_SRCS = $(filter src/scene/%.c,$(SRCS))
UI_SRCS = $(filter src/ui/%.c,$(SRCS))
SCENE_HDRS = $(filter src/scene/%.h,$(HDRS))
UI_HDRS = $(filter src/ui/%.h,$(HDRS))
STATE_NEUTRAL_SRCS = cmd_format.c prof.c tests/gl-stubs/gl_stub_counts.c

# Object lists used to build the standalone teapot_demo without dragging in
# any REPL editor/controller code. Scene + prof — the scene module no
# longer touches repl_eval (replay-baseline restore is dispatched through a
# function pointer the controller installs; geometry-guide arg parsing is
# done in the controller before snapshot is built).
TEAPOT_DEMO_DEP_SRCS = $(SCENE_SRCS) prof.c

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
	test_repl_export_all_commands \
	test_repl_core_examples \
	test_repl_core_search \
	test_repl_core_search_extra \
	test_editor_completion \
	test_ui_menu_bar \
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

test_repl_code_panel_layout_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_code_panel_layout.o $(OBJDIR)/src/ui/code_panel_layout.o
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

test_repl_replay_OBJS = $(OBJDIR)/$(TEST_DIR)/test_repl_replay.o $(filter-out $(OBJDIR)/replay.o,$(CORE_TEST_OBJS))

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

# Standalone demo binary that drives the scene module with a teapot callback.
# Proves the scene/ subtree links cleanly without the editor/UI/controller code.
TEAPOT_DEMO_OBJS = $(OBJDIR)/tools/teapot_demo/teapot.o \
                   $(addprefix $(OBJDIR)/,$(TEAPOT_DEMO_DEP_SRCS:.c=.o))

teapot_demo: $(TEAPOT_DEMO_OBJS) ## Build the standalone teapot demo.
	$(CC) $(OBJ_CFLAGS) -o $@ $(TEAPOT_DEMO_OBJS) $(GL_LDFLAGS)

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
	@echo "    Checking GL/GLU drawing calls isolation..."
	@! grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*[[:space:]]*\(' $(REPL_SRCS) | grep -v '^repl_executor\.c:' | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GL/GLU calls found outside repl_executor.c$(NC)" && exit 1)
	@echo "    Checking GL/GLU calls in sample.h..."
	@! grep -nE '\b(gl[A-Z]|glu[A-Z])[A-Za-z0-9]*[[:space:]]*\(' sample.h | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GL/GLU calls found in sample.h$(NC)" && exit 1)
	@echo "    Checking GLUT input/feedback calls isolation..."
	@! grep -nE '\bglut[A-Z][A-Za-z0-9]*[[:space:]]*\(' $(REPL_SRCS) | grep -vE '^repl_executor\.c:' | grep -vE '^([^:]+:)?[0-9]+:[[:space:]]*(/\*|\*|//)' | grep -vE '"' || (echo "    $(RED)ERROR: GLUT calls found outside repl_executor.c$(NC)" && exit 1)
	@echo "    GL/GLUT boundaries $(GREEN)OK$(NC)"

check-layer-coupling: ## Verify UI and scene layers don't include each other's headers.
	@echo "    Checking UI/scene layer coupling..."
	@! grep -nE '#include\s+"scene/' $(UI_SRCS) $(UI_HDRS) || (echo "    $(RED)ERROR: UI files must not include scene headers$(NC)" && exit 1)
	@! grep -nE '#include\s+"ui/' $(SCENE_SRCS) $(SCENE_HDRS) || (echo "    $(RED)ERROR: scene files must not include UI headers$(NC)" && exit 1)
	@echo "    Layer coupling $(GREEN)OK$(NC)"


check-controller-boundaries: ## Verify controller owns the scene/UI wiring boundary.
	@echo "Checking controller boundaries..."
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"scene/' $(REPL_SRCS) imrepl_ctrl.c \
		| grep -v '^imrepl_ctrl\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: scene headers included outside imrepl_ctrl.c:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"ui/' $(REPL_SRCS) imrepl_ctrl.c \
		| grep -vE '^(imrepl_ctrl|repl_(actions|editor|export))\.c$$' || true); \
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
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"repl_state\.h"' $(SCENE_SRCS) $(STATE_NEUTRAL_SRCS) 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: scene or state-neutral files include repl_state.h:$(NC)"; \
		echo "$$bad"; exit 1; \
	fi
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"repl_core_internal\.h"' \
		imrepl_ctrl.c $(SCENE_SRCS) $(UI_SRCS) $(STATE_NEUTRAL_SRCS) 2>/dev/null \
		| grep -vE '^ui_(color_picker|panels)\.c$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: unapproved view/utility files include repl_core_internal.h:$(NC)"; \
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

check-views-no-owners: ## Verify scene/UI files do not include repl_state_owners.h.
	@echo "Checking scene/UI view files do not include repl_state_owners.h..."
	@bad=$$(grep -lE '#[[:space:]]*include[[:space:]]+"repl_state_owners\.h"' $(SCENE_SRCS) $(UI_SRCS) 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
		echo "$(RED)ERROR: scene/UI view files include repl_state_owners.h:$(NC)"; \
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
		check-imrepl-not-editor-mirror \
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
		check-no-test-default-output; do \
		printf "  $(YELLOW)▶$(NC) $$target\n"; \
		$(MAKE) --no-print-directory $$target 2>&1 | sed 's/^/    /' | sed $$'s/ OK / \033[0;32mOK\033[0m /g; s/ OK$$/ \033[0;32mOK\033[0m/' || exit $$?; \
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

check-ui-panels-no-mutators: ## Hard guard: src/ui/panels.c references no input-dispatch mutators (Phase J2.2).
	@bash scripts/check-ui-panels-no-mutators.sh

check-replay-ui-isolation: ## Hard guard: replay_ui_*.c is feature-UI — no editor / REPL mutators or parser/compile/apply calls.
	@bash scripts/check-replay-ui-isolation.sh

check-color-picker-ui-isolation: ## Strict guard: color_picker_ui*.c is pure renderer/hit-test over ColorPickerView — no mutators, no live state reads, no parser/compile/apply.
	@bash scripts/check-color-picker-ui-isolation.sh

check-variable-panel-forwarders: ## Ratchet legacy variable_panel forwarder API uses (editor_state_variable_drag*, ui_state_variable_panel*, repl_var_drag_*).
	@bash scripts/check-variable-panel-forwarders.sh scripts/baselines/variable-panel-forwarders.txt

check-replay-forwarders: ## Ratchet legacy repl_state_replay* forwarder API uses (replay peer is the owner).
	@bash scripts/check-replay-forwarders.sh scripts/baselines/replay-forwarders.txt

check-no-repl-commit: ## Verify repl_commit.{c,h} stays deleted (commit dispatch lives in editor_commit.c).
	@bash scripts/check-no-repl-commit.sh

check-no-repl-editor-input-shim: ## Verify editor_input.c does not delegate to legacy repl_*_func entry points.
	@bash scripts/check-no-repl-editor-input-shim.sh

check-no-set-status-in-repl-parser: ## Ratchet set_status calls inside repl_parser.c (parser diagnostics flow via ctx->err_buf).
	@bash scripts/check-no-set-status-in-repl-parser.sh scripts/baselines/repl-parser-set-status.txt

check-no-set-status-in-compile-apply: ## Verify repl_compile.c / repl_apply.c never call set_status (Phase C purity).
	@bash scripts/check-no-set-status-in-compile-apply.sh

check-no-test-default-output: ## Hard guard: tests may not call repl_save_default_output() (writes ./output.c in repo root).
	@bash scripts/check-no-test-default-output.sh

CHECK_TARGETS = \
	check-gl-boundaries \
	check-layer-coupling \
	check-state-ownership \
	check-public-api-usage

check: ## Run all checks.
	@set -e; \
	for target in $(CHECK_TARGETS); do \
		desc=$$(awk -v target="$$target" 'BEGIN {FS = ":.*## "} $$1 == target { print $$2; exit }' $(firstword $(MAKEFILE_LIST))); \
		printf "$(CYAN)\n==> %s$(NC)\n" "$$desc"; \
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

capacity-matrix: ## Print state-scaling matrix: per-tunable bytes-per-unit, current totals, and undo/redo ring footprint.
	@$(CC) $(COMMON_CFLAGS) -o build/capacity_matrix tools/capacity_matrix.c
	@./build/capacity_matrix

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
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / && $$1 !~ /^check-/ {printf "  %-24s %s\n", $$1, $$2}' $(MAKEFILE_LIST) | sort

-include $(DEPS)
