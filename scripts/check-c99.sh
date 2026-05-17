#!/bin/bash
# C99 sample-source guard (old-machine / old-GCC target).
#
# Goal: the sample must build as C99 on old machines with an old GCC,
# NOT pure pedantic ISO C99. GNU extensions that GCC accepts in
# `-std=c99` mode (without `-pedantic`) are fine — they were always
# part of "just compile it with gcc -std=c99". So this guard is
# `-std=c99` WITHOUT `-pedantic-errors`: it still catches genuine
# C99-portability breakers (C99 makes implicit function declarations an
# error; raw C11 `_Static_assert` is unknown to early-2000s GCC, etc.)
# while tolerating the GNU-isms old GCC happily accepts.
#
# It does NOT build objects or flip the sample's build standard — it is
# `-fsyntax-only` on the local GL/GLU/GLUT stub include path, so a
# platform OpenGL header exposing old-style callback typedefs cannot
# fail the guard (the stub headers are clean) and no object tree needs
# splitting by standard. The default build stays C2x; `make test`
# stays C2x.
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
    # -std=c99, NO -pedantic-errors: model old `gcc -std=c99`, which
    # accepts GNU extensions. Genuine C99 breakers (implicit function
    # declarations, unknown C11 constructs) are still hard errors in
    # C99 mode and still fail here.
    if ! "$CC" -std=c99 -fsyntax-only \
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
    echo "ERROR: sample source does not compile under 'gcc -std=c99'." >&2
    echo "The sample must build as C99 on old machines (default build" >&2
    echo "stays C2x; tests stay C2x). The usual genuine breaker is a" >&2
    echo "C11-ism unknown to old GCC: use STATIC_ASSERT" >&2
    echo "(include/c_compat.h), not raw _Static_assert. (GNU extensions" >&2
    echo "GCC accepts in -std=c99 mode are fine — this is not pedantic" >&2
    echo "ISO C99.)" >&2
    grep -E ': (error|fatal error):' "$LOG" >&2 || cat "$LOG" >&2
    rm -f "$LOG"
    exit 1
fi
rm -f "$LOG"

echo "c99 (sample source builds under gcc -std=c99) OK"
