#!/bin/bash
# C99 build guard for the shipped/real artifacts (non-pedantic).
#
# The whole project builds -std=c99 so it runs on old machines / old
# GCC. This guard syntax-checks the shipped/real sources — the sample
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

# Ratchet source set: sample object set ($(SRCS), passed in as
# C99_SRCS so it can't drift from what the build compiles) PLUS the
# demo drivers and bench harness under tools/ and bench/. Tests
# (tests/*.c, tests/support/) are deliberately NOT included. $(SRCS)
# already contains tests/gl-stubs/gl_stub_counts.c (linked into the
# sample); the find adds tools/** and bench/**.
if [ -n "${C99_SRCS:-}" ]; then
    SAMPLE_FILES="${C99_SRCS}"
else
    SAMPLE_FILES="$(printf '%s ' audio.c cmd_format.c prof.c sample.c \
        tests/gl-stubs/gl_stub_counts.c; find src -name '*.c')"
fi
FILES="$(printf '%s\n' ${SAMPLE_FILES}; \
         find tools bench -name '*.c' 2>/dev/null | sort)"

# Use the REAL GL/GLU/GLUT/freeglut headers (the superset that
# declares every symbol scene_demo/bench/sample use) as -isystem, so
# their own old-style decls don't fail the guard while our -I'd code
# stays -I'd. NOT -DGL_STUBS: the minimal stub headers miss
# real-GL symbols the demos use; the empty-TU fixes already keep the
# GL_STUBS-gated files non-empty without it. Nonexistent dirs are
# harmless (the compiler ignores them), so this stays portable across
# macOS (homebrew/freeglut-fork) and Linux (system /usr/include).
SYS_GL="-isystem /usr/include \
        -isystem /usr/local/include \
        -isystem /opt/homebrew/include \
        -isystem $HOME/src/freeglut-fork/include"

fail=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    # Non-pedantic by design (see header). OUR code -I; vendored GL
    # -isystem (a third-party header's old-style decl must not fail us).
    if ! "$CC" -std=c99 -fsyntax-only \
            -Wall -Wno-deprecated-declarations -Wfloat-conversion \
            -DGL_SILENCE_DEPRECATION \
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
    echo "(sample + demo + bench). The project targets C99 for old" >&2
    echo "machines. Usual genuine breakers: a C11-ism unknown to old GCC" >&2
    echo "(use STATIC_ASSERT from include/c_compat.h, not raw" >&2
    echo "_Static_assert), or an implicit function declaration / unknown" >&2
    echo "symbol (a hard error in C99 even non-pedantic)." >&2
    grep -E ': (error|fatal error):' "$LOG" >&2 || cat "$LOG" >&2
    rm -f "$LOG"
    exit 1
fi
rm -f "$LOG"

echo "c99 build guard (sample + demos + bench, gcc -std=c99) OK"
