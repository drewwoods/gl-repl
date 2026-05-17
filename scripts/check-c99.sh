#!/bin/bash
# C99 sample-source guard.
#
# The sample is built C2x by default; this guard syntax-checks the
# sample/product translation units under `-std=c99 -pedantic-errors`
# so the source stays valid C99 (a deliberate retro-portability goal).
# It does NOT build objects or flip the sample's build standard — it is
# `-fsyntax-only` on the local GL/GLU/GLUT stub include path, so a
# platform OpenGL header exposing old-style callback typedefs cannot
# fail the guard (the stub headers are clean) and no object tree needs
# splitting by standard.
#
# Scope: the sample product source set (root TUs + src/**.c), matching
# Makefile $(SRCS). Test-only / generated harness files are out of the
# C99 promise — `make test` stays C2x.
#
# Fast: -fsyntax-only, no codegen, no link.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

CC=${CC:-gcc}
GL_STUBS_INC=tests/gl-stubs/include
ROOT=$(pwd)
LOG=/tmp/check-c99.log
: > "$LOG"

# The exact sample object source set. Authoritatively passed in as
# Makefile $(SRCS) (C99_SRCS) so coverage can never drift from what the
# sample/stub build actually compiles — $(SRCS) includes root TUs,
# src/**.c, AND tests/gl-stubs/gl_stub_counts.c (linked into sample).
# The fallback (direct invocation without make) mirrors that set,
# including the stub-count TU, so a regression there is still caught.
if [ -n "${C99_SRCS:-}" ]; then
    FILES=$(printf '%s\n' ${C99_SRCS})
else
    FILES=$(printf '%s\n' audio.c cmd_format.c prof.c sample.c \
                          tests/gl-stubs/gl_stub_counts.c; \
            find src -name '*.c' | sort)
fi

fail=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    if ! "$CC" -std=c99 -pedantic-errors -fsyntax-only \
            -Wall -Wno-deprecated-declarations -Wfloat-conversion \
            -DGL_SILENCE_DEPRECATION -DGL_STUBS \
            -I"$GL_STUBS_INC" -I"$ROOT" -I"$ROOT/src" -I"$ROOT/include" \
            "$f" 2>> "$LOG"; then
        fail=1
    fi
done <<EOF
$FILES
EOF

if [ "$fail" -ne 0 ]; then
    echo "ERROR: sample source does not syntax-check under -std=c99 -pedantic-errors." >&2
    echo "The sample is C99-portable by contract (default build stays C2x;" >&2
    echo "tests stay C2x). Keep new code C99-clean: use STATIC_ASSERT" >&2
    echo "(include/c_compat.h) not raw _Static_assert, plain __VA_ARGS__" >&2
    echo "not GNU ', ##__VA_ARGS__', a single typedef per name, and" >&2
    echo "prototyped function-pointer casts (no old-style 'void (*)()')." >&2
    grep -E ': (error|warning):' "$LOG" >&2 || cat "$LOG" >&2
    rm -f "$LOG"
    exit 1
fi
rm -f "$LOG"

echo "c99 (sample source -std=c99 -pedantic-errors) OK"
