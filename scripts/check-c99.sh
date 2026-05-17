#!/bin/bash
# Pedantic C99 ratchet for the shipped/real artifacts.
#
# The whole project builds -std=c99 (non-pedantic) by default so it
# runs on old machines / old GCC. This guard additionally holds the
# *shipped/real* sources — the sample plus the demo drivers and the
# bench harness — to the stricter `-std=c99 -pedantic-errors` bar so
# C99 conformance can't regress. Tests are intentionally excluded:
# the pedantic delta there is real work (zero-length arrays, {} inits,
# old-style prototypes), not a no-op, and tests are not shipped.
#
# Strictness scoping: OUR code dirs are `-I` (so -pedantic-errors
# judges them); platform/vendored GL/GLU/GLUT/freeglut headers — and
# the local GL stub headers — are `-isystem`, so a third-party
# header's own old-style declaration (e.g. freeglut_ext.h) can't fail
# the ratchet while our source stays fully policed.
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
# their own old-style decls don't fail the ratchet while our -I'd code
# stays fully policed. NOT -DGL_STUBS: the minimal stub headers miss
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
    # OUR code -I (pedantic-policed); vendored GL -isystem (exempt).
    if ! "$CC" -std=c99 -pedantic-errors -fsyntax-only \
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
    echo "ERROR: a shipped/real source does not pass the pedantic C99 ratchet" >&2
    echo "(-std=c99 -pedantic-errors). The default build is non-pedantic C99," >&2
    echo "but sample + demo + bench sources must stay -pedantic-errors clean." >&2
    echo "Usual fixes: STATIC_ASSERT (include/c_compat.h) not raw" >&2
    echo "_Static_assert; keep a TU non-empty (-Wempty-translation-unit);" >&2
    echo "prototyped function-pointer typedefs not old-style 'void (*)()'." >&2
    grep -E ': (error|fatal error):' "$LOG" >&2 || cat "$LOG" >&2
    rm -f "$LOG"
    exit 1
fi
rm -f "$LOG"

echo "c99 pedantic ratchet (sample + demos + bench, -std=c99 -pedantic-errors) OK"
