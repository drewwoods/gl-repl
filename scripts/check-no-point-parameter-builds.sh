#!/bin/bash
# Hard guard: repl_executor.c must syntax-check with -DNO_POINT_PARAMETER
# without including glr_camera.h or any other app/UI header.
#
# The compile-time pitfall the guard catches: repl_executor.c used
# to declare `ReplCameraState glr_camera(void);` inline and call
# `glr_camera().dist` from the legacy point-size fallback. That
# worked while glr_camera.c was still in REPL_DEMO_DEP_SRCS, but
# step 7e removed it — and the inline forward declaration left an
# `unknown type name 'ReplCameraState'` error behind whenever
# `make sample NO_POINT_PARAMETER=1` or `make repl_demo
# NO_POINT_PARAMETER=1` was forced.
#
# Step 7e routes camera distance through a controller-installed
# callback (repl_executor_install_camera_distance_source) so the
# fallback compiles without reaching for app-shell types. This
# guard is the regression test: -fsyntax-only is fast (~1s) and
# directly exercises the relevant translation unit.

set -euo pipefail

CC=${CC:-gcc}
GL_STUBS_INC=tests/gl-stubs/include
ROOT=$(pwd)

if ! "$CC" -fsyntax-only -DNO_POINT_PARAMETER -DGL_SILENCE_DEPRECATION \
        -DGL_STUBS \
        -I"$GL_STUBS_INC" -I"$ROOT" -I"$ROOT/src" -I"$ROOT/include" \
        -std=c2x -Wall -Wno-deprecated-declarations -Wfloat-conversion \
        repl_executor.c \
        2> /tmp/check-no-point-parameter-builds.log; then
    echo "ERROR: repl_executor.c does not syntax-check with NO_POINT_PARAMETER=1." >&2
    echo "Step 7e moved the camera-distance fallback off direct glr_camera() calls" >&2
    echo "and onto a controller-installed callback. New code in repl_executor.c" >&2
    echo "must not reach for app/UI types from the NO_POINT_PARAMETER fallback path." >&2
    cat /tmp/check-no-point-parameter-builds.log >&2
    rm -f /tmp/check-no-point-parameter-builds.log
    exit 1
fi
rm -f /tmp/check-no-point-parameter-builds.log

echo "no-point-parameter-builds OK"
