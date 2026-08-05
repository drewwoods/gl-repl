/*
 * tests/support/camera_bridge_stub.h - Recording ReplExportCameraBridge.
 *
 * A test asserting anything about a pose has to install a bridge: without
 * one repl_camera_header_finish() applies nothing and there is no pose to
 * observe. This stub records what it was handed - the pose, the apply mode,
 * and the call count - and hands back a settable destination pose so
 * partial-header merges can be checked against a known baseline.
 *
 * Header-only and shared rather than private to one TU, because
 * test_camera_header_parity and test_camera_apply_modes both need it and a
 * second copy would be one more thing to drift.
 */
#ifndef TESTS_SUPPORT_CAMERA_BRIDGE_STUB_H
#define TESTS_SUPPORT_CAMERA_BRIDGE_STUB_H

#include <stdio.h>
#include <string.h>

#include "repl/camera_header.h"
#include "repl/export.h"

typedef struct {
    ReplCameraPose      destination;   /* what capture_pose reports */
    ReplCameraPose      applied;       /* last pose handed to apply_pose */
    ReplCameraApplyMode mode;          /* ... and the mode it came with */
    int                 apply_count;
    int                 capture_count;
    int                 with_anim_hook;/* last fill_block projection asked for */
} CameraBridgeStubState;

static CameraBridgeStubState g_camera_bridge_stub;

static void camera_bridge_stub_capture(ReplCameraPose *out) {
    g_camera_bridge_stub.capture_count++;
    *out = g_camera_bridge_stub.destination;
}

static void camera_bridge_stub_apply(const ReplCameraPose *pose,
                                     ReplCameraApplyMode mode) {
    g_camera_bridge_stub.applied = *pose;
    g_camera_bridge_stub.mode    = mode;
    g_camera_bridge_stub.apply_count++;
    /* A snapped apply is the new destination, so a second load in the same
     * test merges against the first one's result - as the app would. */
    g_camera_bridge_stub.destination = *pose;
}

static void camera_bridge_stub_fill(ReplExportCameraBlock *block,
                                    int with_anim_hook) {
    const ReplCameraPose *p = &g_camera_bridge_stub.destination;

    g_camera_bridge_stub.with_anim_hook = with_anim_hook;
    memset(block, 0, sizeof(*block));
    snprintf(block->lines[0], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(0.0000f, 0.0000f, %.4ff);   // @camera dist",
             -p->dist);
    snprintf(block->lines[1], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);   // @camera rx", p->rx);
    snprintf(block->lines[2], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(%.4ff, 0.0f, 1.0f, 0.0f);   // @camera ry", p->ry);
    if (with_anim_hook)
        snprintf(block->lines[3], REPL_EXPORT_CAMERA_LINE_MAX,
                 "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);   // @camera spin");
    snprintf(block->lines[4], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(%.4ff, %.4ff, %.4ff);   // @camera pan",
             -p->tx, -p->ty, -p->tz);
    block->present = 1;
}

static const ReplExportCameraBridge g_camera_bridge_stub_vtable = {
    camera_bridge_stub_fill,
    camera_bridge_stub_capture,
    camera_bridge_stub_apply
};

/* Install the stub and clear its recording. `destination` is the pose an
 * unseen role merges from; pass NULL for the zero pose. */
static void camera_bridge_stub_install(const ReplCameraPose *destination) {
    memset(&g_camera_bridge_stub, 0, sizeof(g_camera_bridge_stub));
    if (destination)
        g_camera_bridge_stub.destination = *destination;
    repl_export_install_camera_bridge(&g_camera_bridge_stub_vtable);
}

#endif /* TESTS_SUPPORT_CAMERA_BRIDGE_STUB_H */
