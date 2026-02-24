CC = gcc

CFLAGS = \
	-Wall -ggdb -O0 -g3 \
	-Wno-deprecated-declarations -Wfloat-conversion \
	-fsanitize=address -fno-omit-frame-pointer \
	-std=c2x -DGL_SILENCE_DEPRECATION \
	-I/usr/include \
	-I/opt/homebrew/include \
	-I$(HOME)/src/freeglut-fork/include \
	-I$(HOME)/code/openGL/samples/gen-ai/OpenGL-Vibe/include

GL_LDFLAGS = \
	-L/opt/homebrew/lib \
	-L$(HOME)/src/freeglut-fork/build/lib \
	-Wl,-rpath,$(HOME)/src/freeglut-fork/build/lib \
	-lglut -lm -lpthread \
	-framework IOKit -framework Cocoa -framework OpenGL

.PHONY: all clean test lines

all: sample test_eval test_format \
	test_repl_core_parse test_repl_core_format test_repl_core_commit test_repl_core_io

SRCS = sample.c repl_core.c scene_render.c ui_panels.c repl_eval.c cmd_format.c
HDRS = sample.h repl_core.h repl_core_internal.h scene_render.h ui_panels.h repl_eval.h cmd_format.h
CORE_TEST_SRCS = repl_core.c scene_render.c ui_panels.c repl_eval.c cmd_format.c

sample: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(GL_LDFLAGS)

test_eval: test_eval.c repl_eval.c repl_eval.h
	$(CC) $(CFLAGS) -o $@ test_eval.c repl_eval.c -lm -lpthread

test_format: test_format.c cmd_format.c cmd_format.h
	$(CC) $(CFLAGS) -o $@ test_format.c cmd_format.c -lm

test_repl_core_parse: test_repl_core_parse.c $(CORE_TEST_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_repl_core_parse.c $(CORE_TEST_SRCS) $(GL_LDFLAGS)

test_repl_core_format: test_repl_core_format.c $(CORE_TEST_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_repl_core_format.c $(CORE_TEST_SRCS) $(GL_LDFLAGS)

test_repl_core_commit: test_repl_core_commit.c $(CORE_TEST_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_repl_core_commit.c $(CORE_TEST_SRCS) $(GL_LDFLAGS)

test_repl_core_io: test_repl_core_io.c $(CORE_TEST_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_repl_core_io.c $(CORE_TEST_SRCS) $(GL_LDFLAGS)

test: test_eval test_format test_repl_core_parse test_repl_core_format test_repl_core_commit test_repl_core_io
	./test_eval --run-tests
	./test_format
	./test_repl_core_parse
	./test_repl_core_format
	./test_repl_core_commit
	./test_repl_core_io

# count lines: $(SRCS) $(HDRS)
lines: $(SRCS) $(HDRS)
	@echo "Counting lines of code in source and header files..."
	@wc -l $(SRCS) $(HDRS) | sort -nr

clean:
	rm -rf sample sample.dSYM \
		test_eval test_eval.dSYM test_format test_format.dSYM \
		test_repl_core_parse test_repl_core_parse.dSYM \
		test_repl_core_format test_repl_core_format.dSYM \
		test_repl_core_commit test_repl_core_commit.dSYM \
		test_repl_core_io test_repl_core_io.dSYM \
		*.o
