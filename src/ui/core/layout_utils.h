/*
 * layout_utils.h - Pure layout helpers shared across UI layers.
 */
#ifndef UI_CORE_LAYOUT_UTILS_H
#define UI_CORE_LAYOUT_UTILS_H

/* Clamp a floating panel's bottom y within the scene rect, honoring a bottom
 * inset (status bar / lift baseline) plus an edge pad. When the scene is too
 * short to fit the panel between both edges, `prefer_top_on_overflow` chooses
 * whether the fallback parks against the scene top or the bottom inset. */
static inline int ui_clamp_panel_y(int scene_y, int scene_h, int panel_h,
                                   int requested_y,
                                   int prefer_top_on_overflow,
                                   int bottom_inset, int edge_pad) {
    int min_y = scene_y + bottom_inset + edge_pad;
    int max_y = scene_y + scene_h - panel_h - edge_pad;
    if (max_y >= min_y) {
        if (requested_y < min_y) return min_y;
        if (requested_y > max_y) return max_y;
        return requested_y;
    }
    return prefer_top_on_overflow
         ? (scene_y + scene_h - panel_h - edge_pad)
         : min_y;
}

#endif /* UI_CORE_LAYOUT_UTILS_H */
