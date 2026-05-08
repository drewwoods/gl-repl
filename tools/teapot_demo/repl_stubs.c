/*
 * tools/teapot_demo/repl_stubs.c
 *
 * No-op stubs for REPL-side functions referenced by src/scene/overlays.c
 * but never actually invoked when the demo runs without REPL state
 * (the demo's SceneRenderConfig leaves show_vpoints / show_vnums /
 * show_normals at 0, so the overlay functions short-circuit before they
 * would walk the user program). The stubs let the linker resolve the
 * symbols without pulling in replay.c and its REPL-state dependencies.
 *
 * If the demo ever wants to use the cursor-aware overlays it would need
 * to link against replay.c plus a real flat-program source — at which
 * point it isn't a "scene-only" demo anymore.
 */
#include "replay.h"

void repl_walk_user_vertices(const ReplVertexWalkContext *ctx,
                             const ReplVertexWalkCallbacks *cb,
                             void *user_data) {
    (void)ctx;
    (void)cb;
    (void)user_data;
}
