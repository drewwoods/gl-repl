#!/bin/bash
# Hard guard: project-local headers must use #include "X.h", not
# #include <X.h>.
#
# Convention:
#   #include <stdio.h>        — system / standard library  (angle)
#   #include <GL/gl.h>        — system GL                  (angle)
#   #include <miniaudio.h>    — vendored third-party       (angle)
#   #include "keys.h"         — project-local              (quoted)
#   #include "gl_includes.h"  — project-local              (quoted)
#   #include "support/cpuprof.h" — project-local subdir       (quoted)
#
# The set below enumerates the project-local headers that have
# historically been included with angle brackets (because they sit on
# the -I. or -Iinclude line). Once standardised, the rule is simple:
# any future header in a project-local directory should use quoted
# form; this guard catches a regression on the same set.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# Project-local headers that must use "" not <>.
# (Bare names — these resolve via -I., -Iinclude, or are top-level
# project files. Subdirectory-namespaced project headers like
# "support/cpuprof.h" are already disambiguated by the path.)
LOCAL_HEADERS=(c_compat.h gl_includes.h keys.h gl_2d.h)

# Scan tracked .c / .h files only — gitignored output artifacts
# (output.c, my-scene.c, quit-recovery.c) are produced by the running
# binary, not source we own.
tracked=$(git ls-files '*.c' '*.h' 2>/dev/null)

violations=""
for h in "${LOCAL_HEADERS[@]}"; do
    h_esc=$(printf '%s' "$h" | sed 's/\./\\./g')
    # Build the pattern once; grep -F won't help because we need ^#include + <H>
    hits=$(printf '%s\n' $tracked \
            | xargs grep -nE "^#include[ \t]+<${h_esc}>" 2>/dev/null || true)
    if [ -n "$hits" ]; then
        violations="${violations}${hits}
"
    fi
done

if [ -z "$violations" ]; then
    echo "include-style OK (project-local headers use quoted form)"
    exit 0
fi

echo "ERROR: project-local headers must use #include \"X.h\", not <X.h>." >&2
echo "Affected files:" >&2
printf '%s' "$violations" >&2
echo "" >&2
echo "Fix: change the angle-bracket form to the quoted form. System / vendored" >&2
echo "headers (<stdio.h>, <GL/gl.h>, <miniaudio.h>, etc.) keep the angle form." >&2
exit 1
