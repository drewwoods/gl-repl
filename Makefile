CC = gcc
PROJECT_ROOT := $(abspath ../../..)
REPO_INCLUDE := $(PROJECT_ROOT)/include
LOCAL_INCLUDE := $(abspath include)

UNAME_S := $(shell uname -s)

ifeq ($(USE_GL_STUBS),1)
GL_HEADER_CFLAGS = \
	-DOPENGL_VIBE_USE_GL_STUBS \
	-I$(LOCAL_INCLUDE)
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
	-I$(REPO_INCLUDE)

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

.PHONY: all clean test test-detailed test_detailed test-stubs lines debug coverage glut help bench bench-csv

all: sample

SRCS = sample.c repl_core.c repl_state.c repl_command_spec.c repl_command_store.c repl_commit.c repl_clipboard.c repl_undo.c repl_camera_controls.c repl_actions.c repl_flatten.c repl_executor.c repl_autocomplete.c repl_autonormal.c repl_scenes.c repl_example_loader.c repl_replay.c repl_search.c repl_export.c repl_editor.c repl_examples.c scene_render.c ui_panels.c repl_eval.c cmd_format.c repl_audio.c profile_panel.c gl_stub_counts.c
HDRS = sample.h repl_state.h repl_core.h repl_core_internal.h repl_command_spec.h repl_command_store.h repl_clipboard.h repl_undo.h repl_camera_controls.h repl_actions.h repl_replay.h repl_examples.h scene_render.h ui_panels.h repl_eval.h cmd_format.h repl_audio.h profile_panel.h
CORE_TEST_SRCS = repl_core.c repl_state.c repl_command_spec.c repl_command_store.c repl_commit.c repl_clipboard.c repl_undo.c repl_camera_controls.c repl_actions.c repl_flatten.c repl_executor.c repl_autocomplete.c repl_autonormal.c repl_scenes.c repl_example_loader.c repl_replay.c repl_search.c repl_export.c repl_editor.c repl_examples.c scene_render.c ui_panels.c repl_eval.c cmd_format.c repl_audio.c profile_panel.c gl_stub_counts.c

OBJDIR = build/$(BUILD)$(if $(filter 1,$(USE_GL_STUBS)),-gl-stubs,)
OBJ_CFLAGS = $(BUILD_CFLAGS) $(CFLAGS)
DEPFLAGS = -MMD -MP

SAMPLE_OBJS = $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))
CORE_TEST_OBJS = $(addprefix $(OBJDIR)/,$(CORE_TEST_SRCS:.c=.o))

TEST_BINS = \
	test_eval \
	test_format \
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
	test_repl_editor \
	test_repl_core_extra \
	test_repl_autonormal

CORE_TEST_BINS = $(filter-out test_eval test_format test_repl_audio,$(TEST_BINS))

# Benchmark binaries follow the same linking pattern as core test binaries
# (they reuse CORE_TEST_OBJS so they work in both real-GL and stubs builds),
# but they are intentionally NOT in TEST_BINS so `make test` does not run
# them — benchmarks are timing-sensitive and should be invoked explicitly.
BENCH_BINS = bench_repl

define core_test_binary
$(1)_OBJS = $$(OBJDIR)/$(1).o $$(CORE_TEST_OBJS)
$(1)_LDLIBS = $$(GL_LDFLAGS)
$(1)_RUN ?= ./$(1)
endef

$(foreach test,$(CORE_TEST_BINS),$(eval $(call core_test_binary,$(test))))
$(foreach bin,$(BENCH_BINS),$(eval $(call core_test_binary,$(bin))))

test_eval_OBJS = $(OBJDIR)/test_eval.o $(OBJDIR)/repl_eval.o
test_eval_LDLIBS = -lm -lpthread
test_eval_RUN = ./test_eval --run-tests

test_format_OBJS = $(OBJDIR)/test_format.o $(OBJDIR)/cmd_format.o
test_format_LDLIBS = -lm
test_format_RUN ?= ./test_format

test_repl_audio_OBJS = $(OBJDIR)/test_repl_audio.o $(OBJDIR)/repl_audio.o
test_repl_audio_LDLIBS = $(GL_LDFLAGS)
test_repl_audio_RUN ?= ./test_repl_audio

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
	$(MAKE) test BUILD=coverage TEST_JOBS=1
	lcov --capture \
		--directory build/coverage \
		--output-file build/coverage/lcov.info \
		--ignore-errors mismatch,empty \
		--exclude '*/test_*.c' \
		--rc branch_coverage=1
	genhtml build/coverage/lcov.info \
		--output-directory build/coverage/html \
		--branch-coverage \
		--title "REPL coverage" \
		--ignore-errors inconsistent
	@echo "Coverage report: build/coverage/html/index.html"

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
		build

glut: ## Rebuild using the Apple GLUT framework instead of freeglut.
	$(MAKE) clean
	$(MAKE) all \
		BUILD="$(BUILD)" \
		CFLAGS="$(CFLAGS) -DUSE_GLUT" \
		GL_LDFLAGS="$(GLUT_GL_LDFLAGS)"

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
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / {printf "  %-24s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

-include $(DEPS)
