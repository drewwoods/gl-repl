/*
 * glr_overlay_setup.h - per-frame setup for the 3D edit overlays.
 *
 * The overlay passes (outlines, vertex points, vertex labels, normal arrows,
 * cursor guides) live in src/subsystems/edit_overlays/ and read no controller
 * state. This module is the seam that feeds them: it resolves the cursor's
 * block, the presentation toggles and the cursor-line guide arguments into an
 * OverlayWalkCtx and an OverlaySnapshotPack. The controller installs the pack
 * as user_data on the render3d post-overlay hooks, rebuilds it once per frame
 * after the render config is fully populated, and tests and bench_repl call the
 * fills directly so they time and assert exactly what a frame draws.
 */
#ifndef GLR_OVERLAY_SETUP_H
#define GLR_OVERLAY_SETUP_H

#include "render3d/render_types.h"                    /* Render3dRenderConfig */
#include "subsystems/edit_overlays/edit_overlays.h"  /* CursorBlockState, OverlayWalkCtx, OverlaySnapshotPack */

/* The CursorBlockState the 3D overlays walk with for `edit_line`: the flat
 * block cache's current block plus the source-scope extents. Reads the flat
 * block cache as-is - the frame path refreshes it first, and any other caller
 * must call repl_flatten_refresh_current_block_highlight(edit_line) before
 * filling. Lets a test run the vertex-label walker and compare it with the
 * code panel's gutter numbers. */
void glr_overlay_fill_cursor_block_state(int edit_line,
                                         CursorBlockState *out);

/* The whole OverlayWalkCtx the 3D overlay passes walk with for `edit_line`,
 * under the current presentation settings (scope, outlines, highlight). Same
 * flat-block-cache contract as glr_overlay_fill_cursor_block_state(). One fill
 * for the frame and for bench_repl's outline rows, so the benchmark times
 * exactly what a frame draws. */
void glr_overlay_fill_walk_ctx(int edit_line, OverlayWalkCtx *out);

/* The frame's overlay pack. The pointer is stable for the process lifetime, so
 * the controller can hand it to the render3d hooks before the pack is built. */
OverlaySnapshotPack *glr_overlay_pack(void);

/* Rebuild the frame's pack from live state for the editor's cursor line. Call
 * it last in the scene-config builder: it copies multisample_enabled,
 * line_smooth_enabled and alpha_scale out of `cfg`, so a call before those are
 * assigned would publish uninitialized values. `cfg` may be NULL (the config
 * fields then default to 0 / alpha 1). */
void glr_overlay_build_pack(const Render3dRenderConfig *cfg);

#endif /* GLR_OVERLAY_SETUP_H */
