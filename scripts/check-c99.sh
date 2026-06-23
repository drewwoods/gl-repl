#!/bin/bash
# C99 build guard for the shipped/real artifacts (non-pedantic).
#
# The whole project builds -std=c99 so it runs on old machines / old
# GCC. This guard syntax-checks the shipped/real sources — the gl-repl
# plus the demo drivers and bench harness — under `gcc -std=c99` to
# catch genuine C99-portability breakers early.
#
# NOT -pedantic-errors. A real-GCC check (gracemont) showed the only
# -pedantic-errors failures across the whole shipped set were 22 hits
# of ONE benign rule: C99 forbids the implicit pointer-to-array
# const-qualifier conversion (`float (*)[N]` -> `const float (*)[N]`)
# that C2X explicitly re-allows. The code is correct and runs under
# C99/C2X/Clang; only `gcc -std=c99 -pedantic` flags it, and clearing
# it would mean de-const-ing ~7 files of legitimate const-correctness.
# Per the project's "don't pedantic if it's more than a few lines"
# rule, that's out — the guard stays non-pedantic. It still has teeth:
# C99 makes implicit function declarations a hard error even
# non-pedantic, and undeclared symbols fail. (STATIC_ASSERT vs raw
# _Static_assert and "keep TUs non-empty" remain coding conventions
# for genuinely old GCC, just not machine-enforced here.)
#
# Strictness scoping: OUR code dirs are `-I`; platform/vendored
# GL/GLU/GLUT/freeglut headers are `-isystem` (a third-party header's
# own old-style decl, e.g. freeglut_ext.h, must not fail the guard).
#
# Fast: -fsyntax-only, no codegen, no link.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

CC=${CC:-gcc}
ROOT=$(pwd)
LOG=/tmp/check-c99.log
: > "$LOG"

# Ratchet source set: gl-repl object set ($(SRCS), passed in as
# C99_SRCS so it can't drift from what the build compiles) PLUS the
# demo drivers and bench harness under tools/ and bench/. Tests
# (tests/*.c, tests/support/) are deliberately NOT included. $(SRCS)
# already contains tests/gl-stubs/gl_stub_counts.c (linked into the
# gl-repl); the find adds tools/** and bench/**.
if [ -n "${C99_SRCS:-}" ]; then
    SAMPLE_FILES="${C99_SRCS}"
else
    SAMPLE_FILES="$(printf '%s ' audio.c cmd_format.c gl_repl.c \
        tests/gl-stubs/gl_stub_counts.c; find src -name '*.c')"
fi
FILES="$(printf '%s\n' ${SAMPLE_FILES}; \
         find tools bench -name '*.c' 2>/dev/null | sort)"

# Prefer the REAL GL/GLU/GLUT/freeglut headers (the superset that
# declares every symbol render3d_demo/bench/gl-repl use) as -isystem, so
# their own old-style decls don't fail the guard while our -I'd code
# stays -I'd. Nonexistent dirs are harmless (the compiler ignores
# them), so this stays portable across macOS (homebrew + vendored
# third_party/freeglut) and Linux (system /usr/include).
#
# Fallback: on a machine with NO system GL dev headers at all, use the
# repo's vendored stub headers (-DGL_STUBS) instead of hard-failing on
# a missing <GL/gl.h>. The stubs ship precisely for GL-devel-less
# machines; running the guard against them still catches the genuine
# C99 breakers (implicit decls, unknown symbols, C11-isms) it exists
# for, which is strictly better than not running the guard at all.
SYS_GL="-isystem /usr/include \
        -isystem /usr/local/include \
        -isystem /opt/homebrew/include \
        -isystem $ROOT/third_party/freeglut/include"
GL_DEFS=""
have_real_gl=0
for d in /usr/include /usr/local/include /opt/homebrew/include \
         "$ROOT/third_party/freeglut/include"; do
    if [ -e "$d/GL/gl.h" ] || [ -e "$d/OpenGL/gl.h" ]; then
        have_real_gl=1
        break
    fi
done
if [ "$have_real_gl" -eq 0 ]; then
    SYS_GL="-isystem $ROOT/tests/gl-stubs/include"
    GL_DEFS="-DGL_STUBS"
fi

fail=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    # Non-pedantic by design (see header). OUR code -I; vendored GL
    # -isystem (a third-party header's old-style decl must not fail us).
    # -D_GNU_SOURCE mirrors the real build (Makefile COMMON_CFLAGS):
    # without it, strict -std=c99 hides POSIX decls like
    # CLOCK_MONOTONIC, so the guard must compile the shipped sources
    # the same way they are actually built. Likewise the force-includes:
    # the real build injects config.h + prof_sections.h into every TU via
    # OBJ_CFLAGS (-include ...), so the instrumentation sites get the
    # ProfSection catalog with no explicit #include. Mirror that here.
    if ! "$CC" -std=c99 -fsyntax-only \
            -Wall -Wno-deprecated-declarations -Wfloat-conversion \
            -D_GNU_SOURCE -DGL_SILENCE_DEPRECATION $GL_DEFS \
            -include "$ROOT/config.h" -include "$ROOT/prof_sections.h" \
            -I"$ROOT" -I"$ROOT/src" -I"$ROOT/include" -I"$ROOT/tests" \
            $SYS_GL \
            "$f" 2>> "$LOG"; then
        fail=1
    fi
done <<EOF
$FILES
EOF

if [ "$fail" -ne 0 ]; then
    echo "ERROR: a shipped/real source does not build under 'gcc -std=c99'" >&2
    echo "(gl-repl + demo + bench). The project targets C99 for old" >&2
    echo "machines. Usual genuine breakers: a C11-ism unknown to old GCC" >&2
    echo "(use STATIC_ASSERT from include/c_compat.h, not raw" >&2
    echo "_Static_assert), or an implicit function declaration / unknown" >&2
    echo "symbol (a hard error in C99 even non-pedantic)." >&2
    grep -E ': (error|fatal error):' "$LOG" >&2 || cat "$LOG" >&2
    rm -f "$LOG"
    exit 1
fi
rm -f "$LOG"

echo "c99 build guard (gl-repl + demos + bench, gcc -std=c99) OK"
