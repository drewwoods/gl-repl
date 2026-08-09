#!/usr/bin/env bash
#
# The vertex-label depth capture must sit AFTER the frame's glFinish() and
# BEFORE glutSwapBuffers() in gl_repl.c's display callback.
#
# Why this needs a guard rather than a test: glReadPixels of GL_DEPTH_COMPONENT
# is a synchronous whole-pipeline sync point. Issued anywhere earlier in the
# frame it blocks until the driver's queued, vsync-throttled work retires -
# ~14 ms on a render-ahead driver, the entire frame budget. After the glFinish
# the queue is already drained and that wait is already charged to Present, so
# the read costs only its transfer. The correctness of the output is identical
# either way, so no unit test can see the difference; only the ordering can be
# checked, and only in the source.
#
# It also pins the dependency in the other direction: that glFinish is no
# longer just a timestamping nicety. Deleting it as an "optimization" would
# silently turn this capture back into a mid-frame stall.
#
# Measurements: docs/plans/in-review/vertex-label-depth-readback-stall.md,
# reproducible with `make bench-vertex-labels`.
set -euo pipefail

file="gl_repl.c"
capture="glr_ctrl_capture_depth_snapshot"

if [ ! -f "$file" ]; then
    echo "ERROR: $file not found" >&2
    exit 1
fi

# Line numbers of the three calls. Comments mention them too, so match only
# lines that are actual calls: optional leading whitespace, then the call.
finish_line=$(grep -n '^[[:space:]]*glFinish();' "$file" | head -1 | cut -d: -f1 || true)
capture_line=$(grep -n "^[[:space:]]*${capture}();" "$file" | head -1 | cut -d: -f1 || true)
swap_line=$(grep -n '^[[:space:]]*glutSwapBuffers();' "$file" | head -1 | cut -d: -f1 || true)

if [ -z "$finish_line" ]; then
    echo "ERROR: no glFinish() call found in $file." >&2
    echo "       ${capture}() depends on the frame being drained before it" >&2
    echo "       runs; without the glFinish it becomes a mid-frame pipeline" >&2
    echo "       stall costing a full refresh interval on a render-ahead" >&2
    echo "       driver. See docs/plans/in-review/" >&2
    echo "       vertex-label-depth-readback-stall.md." >&2
    exit 1
fi

if [ -z "$capture_line" ]; then
    echo "ERROR: no ${capture}() call found in $file." >&2
    echo "       Vertex-label occlusion needs a depth snapshot captured once" >&2
    echo "       per frame from the host callback." >&2
    exit 1
fi

if [ -z "$swap_line" ]; then
    echo "ERROR: no glutSwapBuffers() call found in $file." >&2
    exit 1
fi

if [ "$capture_line" -le "$finish_line" ]; then
    echo "ERROR: ${capture}() (line $capture_line) must come AFTER" >&2
    echo "       glFinish() (line $finish_line) in $file." >&2
    echo "       Before the drain it is a synchronous whole-pipeline sync in" >&2
    echo "       the middle of the frame: ~14 ms/frame on NVIDIA, versus" >&2
    echo "       ~2.5 ms of pure transfer after it." >&2
    exit 1
fi

if [ "$capture_line" -ge "$swap_line" ]; then
    echo "ERROR: ${capture}() (line $capture_line) must come BEFORE" >&2
    echo "       glutSwapBuffers() (line $swap_line) in $file." >&2
    echo "       After the swap the back buffer's depth is no longer the" >&2
    echo "       frame the labels are about to be drawn over." >&2
    exit 1
fi

echo "depth-capture-after-finish OK (glFinish:$finish_line -> capture:$capture_line -> swap:$swap_line)"
