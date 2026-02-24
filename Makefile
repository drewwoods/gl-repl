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

all: sample test_eval test_format

SRCS = sample.c scene_render.c ui_panels.c repl_eval.c cmd_format.c
HDRS = sample.h scene_render.h ui_panels.h repl_eval.h cmd_format.h

sample: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(GL_LDFLAGS)

test_eval: test_eval.c repl_eval.c repl_eval.h
	$(CC) $(CFLAGS) -o $@ test_eval.c repl_eval.c -lm -lpthread

test_format: test_format.c cmd_format.c cmd_format.h
	$(CC) $(CFLAGS) -o $@ test_format.c cmd_format.c -lm

test: test_eval test_format
	./test_eval --run-tests
	./test_format

# count lines: $(SRCS) $(HDRS)
lines: $(SRCS) $(HDRS)
	@echo "Counting lines of code in source and header files..."
	@wc -l $(SRCS) $(HDRS) | sort -nr

clean:
	rm -rf sample sample.dSYM test_eval test_eval.dSYM test_format test_format.dSYM *.o
