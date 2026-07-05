/*
 * glr_compositor.c - see glr_compositor.h.
 *
 * Whole-frame post-processing hook. The chromatic-aberration effect is
 * delegated to the scene layer's fixed-function primitive
 * (render3d_postprocess_filter_render), invoked over the full window rect
 * instead of the scene viewport rect. That primitive captures the named
 * rect from the back buffer into a texture and redraws it with the R/B
 * channel shifts, so handing it the whole window applies the same effect
 * to the entire composited frame (scene + 2D UI).
 *
 * The scene viewport filter and this compositor filter are kept mutually
 * exclusive by the F10 / Shift+F10 controller (see
 * glr_ctrl_router_handle_post_filter_special_key), so they never both run
 * in one frame; the shared scratch texture inside
 * postprocess_filter.c is therefore never contended, and even if both
 * ran it would only grow to the larger (window) rect and stay correct.
 */
#include "app/glr_compositor.h"

#include "render3d/postprocess_filter.h"

void glr_compositor_postprocess_frame(Render3dPostFilterMode mode,
                                      int win_w, int win_h, float t) {
    if (mode <= RENDER3D_POST_FILTER_OFF || mode >= RENDER3D_POST_FILTER_COUNT)
        return;
    if (win_w <= 0 || win_h <= 0)
        return;

    render3d_postprocess_filter_render(mode, 0, 0, win_w, win_h, t);
}
