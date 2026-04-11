CC = gcc
PROJECT_ROOT := $(abspath ../../..)
REPO_INCLUDE := $(PROJECT_ROOT)/include

COMMON_CFLAGS = \
	-Wall -ggdb -g3 \
	-Wno-deprecated-declarations -Wfloat-conversion \
	-std=c2x -DGL_SILENCE_DEPRECATION \
	-I/usr/include \
	-I/opt/homebrew/include \
	-I$(HOME)/src/freeglut-fork/include \
	-I$(REPO_INCLUDE)

RELEASE_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O2

DEBUG_CFLAGS = \
	$(COMMON_CFLAGS) \
	-O0 \
	-fsanitize=address -fno-omit-frame-pointer

BUILD ?= release

ifeq ($(BUILD),debug)
BUILD_CFLAGS = $(DEBUG_CFLAGS)
else
BUILD_CFLAGS = $(RELEASE_CFLAGS)
endif

GLUT_GL_LDFLAGS = \
	-L/opt/homebrew/lib -lm -lpthread \
	-framework IOKit -framework Cocoa -framework OpenGL -framework GLUT

GL_LDFLAGS = \
	-L/opt/homebrew/lib \
	-L$(HOME)/src/freeglut-fork/build/lib \
	-Wl,-rpath,$(HOME)/src/freeglut-fork/build/lib \
	-lglut -lm -lpthread \
	-framework IOKit -framework Cocoa -framework OpenGL

.PHONY: all clean test test-detailed test_detailed lines debug glut help

define run_named_test
	@{ \
		if [ -t 1 ] && [ -z "$$NO_COLOR" ]; then \
			green='\033[32m'; red='\033[31m'; cyan='\033[36m'; reset='\033[0m'; \
		else \
			green=''; red=''; cyan=''; reset=''; \
		fi; \
		printf "%b==> $(1)%b\n" "$$cyan" "$$reset"; \
		if $(2); then \
			printf "%bPASS%b $(1)\n" "$$green" "$$reset"; \
		else \
			rc=$$?; \
			printf "%bFAIL%b $(1)\n" "$$red" "$$reset"; \
			exit $$rc; \
		fi; \
	}
endef

all: sample test_eval test_format test_repl_core_parse test_repl_core_format test_repl_core_commit test_repl_core_io test_repl_core_examples test_repl_core_search ## Build the sample plus all test binaries using release flags.

SRCS = sample.c repl_core.c repl_search.c repl_export.c repl_editor.c repl_examples.c scene_render.c ui_panels.c repl_eval.c cmd_format.c
HDRS = sample.h repl_core.h repl_core_internal.h repl_examples.h scene_render.h ui_panels.h repl_eval.h cmd_format.h
CORE_TEST_SRCS = repl_core.c repl_search.c repl_export.c repl_editor.c repl_examples.c scene_render.c ui_panels.c repl_eval.c cmd_format.c

sample: $(SRCS) $(HDRS) ## Build the main REPL sample using release flags by default.
	$(CC) $(BUILD_CFLAGS) $(CFLAGS) -o $@ $(SRCS) $(GL_LDFLAGS)

test_eval: test_eval.c repl_eval.c repl_eval.h ## Build the expression evaluator unit test binary.
	$(CC) $(BUILD_CFLAGS) $(CFLAGS) -o $@ test_eval.c repl_eval.c -lm -lpthread

test_format: test_format.c cmd_format.c cmd_format.h ## Build the command formatting unit test binary.
	$(CC) $(BUILD_CFLAGS) $(CFLAGS) -o $@ test_format.c cmd_format.c -lm

test_repl_core_parse: test_repl_core_parse.c $(CORE_TEST_SRCS) $(HDRS) ## Build the REPL parser regression test binary.
	$(CC) $(BUILD_CFLAGS) $(CFLAGS) -o $@ test_repl_core_parse.c $(CORE_TEST_SRCS) $(GL_LDFLAGS)

test_repl_core_format: test_repl_core_format.c $(CORE_TEST_SRCS) $(HDRS) ## Build the REPL formatting regression test binary.
	$(CC) $(BUILD_CFLAGS) $(CFLAGS) -o $@ test_repl_core_format.c $(CORE_TEST_SRCS) $(GL_LDFLAGS)

test_repl_core_commit: test_repl_core_commit.c $(CORE_TEST_SRCS) $(HDRS) ## Build the REPL commit/editing regression test binary.
	$(CC) $(BUILD_CFLAGS) $(CFLAGS) -o $@ test_repl_core_commit.c $(CORE_TEST_SRCS) $(GL_LDFLAGS)

test_repl_core_io: test_repl_core_io.c $(CORE_TEST_SRCS) $(HDRS) ## Build the REPL import/export roundtrip regression test binary.
	$(CC) $(BUILD_CFLAGS) $(CFLAGS) -o $@ test_repl_core_io.c $(CORE_TEST_SRCS) $(GL_LDFLAGS)

test_repl_core_examples: test_repl_core_examples.c $(CORE_TEST_SRCS) $(HDRS) ## Build the predefined-example code-panel golden test binary.
	$(CC) $(BUILD_CFLAGS) $(CFLAGS) -o $@ test_repl_core_examples.c $(CORE_TEST_SRCS) $(GL_LDFLAGS)

test_repl_core_search: test_repl_core_search.c $(CORE_TEST_SRCS) $(HDRS) ## Build the REPL source-search regression test binary.
	$(CC) $(BUILD_CFLAGS) $(CFLAGS) -o $@ test_repl_core_search.c $(CORE_TEST_SRCS) $(GL_LDFLAGS)

test: test_eval test_format test_repl_core_parse test_repl_core_format test_repl_core_commit test_repl_core_io test_repl_core_examples test_repl_core_search ## Run the full automated test suite.
	$(call run_named_test,test_eval,./test_eval --run-tests)
	$(call run_named_test,test_format,./test_format)
	$(call run_named_test,test_repl_core_parse,./test_repl_core_parse)
	$(call run_named_test,test_repl_core_format,./test_repl_core_format)
	$(call run_named_test,test_repl_core_commit,./test_repl_core_commit)
	$(call run_named_test,test_repl_core_io,./test_repl_core_io)
	$(call run_named_test,test_repl_core_examples,REPL_EXPORT_CC="$(CC)" REPL_EXPORT_COMPILE_CFLAGS='$(BUILD_CFLAGS) $(CFLAGS)' ./test_repl_core_examples)
	$(call run_named_test,test_repl_core_search,./test_repl_core_search)

test-detailed: test_eval test_format test_repl_core_parse test_repl_core_format test_repl_core_commit test_repl_core_io test_repl_core_examples test_repl_core_search ## Run the full test suite with verbose example export/compile logging.
	$(call run_named_test,test_eval,./test_eval --run-tests)
	$(call run_named_test,test_format,./test_format)
	$(call run_named_test,test_repl_core_parse,./test_repl_core_parse)
	$(call run_named_test,test_repl_core_format,./test_repl_core_format)
	$(call run_named_test,test_repl_core_commit,./test_repl_core_commit)
	$(call run_named_test,test_repl_core_io,./test_repl_core_io)
	$(call run_named_test,test_repl_core_examples,REPL_EXPORT_CC="$(CC)" REPL_EXPORT_COMPILE_CFLAGS='$(BUILD_CFLAGS) $(CFLAGS)' REPL_EXPORT_VERBOSE=1 ./test_repl_core_examples)
	$(call run_named_test,test_repl_core_search,./test_repl_core_search)

test_detailed: test-detailed ## Alias for test-detailed.

# count lines: $(SRCS) $(HDRS)
lines: $(SRCS) $(HDRS) ## Count lines across source and header files.
	@echo "Counting lines of code in source and header files..."
	@wc -l $(SRCS) $(HDRS) | sort -nr

debug: ## Clean and rebuild everything with debug/ASan flags.
	$(MAKE) clean
	$(MAKE) all BUILD=debug

clean: ## Remove built binaries and object files.
	rm -rf sample sample.dSYM \
		test_eval test_eval.dSYM test_format test_format.dSYM \
		test_repl_core_parse test_repl_core_parse.dSYM \
		test_repl_core_format test_repl_core_format.dSYM \
		test_repl_core_commit test_repl_core_commit.dSYM \
		test_repl_core_examples test_repl_core_examples.dSYM \
		test_repl_core_search test_repl_core_search.dSYM \
		test_repl_core_io test_repl_core_io.dSYM \
		*.o

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
	@printf "  debug:         \$$(common_flags) %s \n\n" "$(filter-out $(COMMON_CFLAGS),$(DEBUG_CFLAGS))"
	@printf "User CFLAGS are appended to the selected build mode.\n\n"
	@awk 'BEGIN {FS = ":.*## "}; /^[a-zA-Z0-9_.-]+:.*## / {printf "  %-24s %s\n", $$1, $$2}' $(MAKEFILE_LIST)
