#ifndef SCENE_OVERLAYS_H
#define SCENE_OVERLAYS_H

int  scene_overlay_flat_block_matches_cursor(int begin_idx, int is_tess);
void scene_overlays_render_outlines(int show_current_poly,
                                    int replay_tess_preview);

#endif /* SCENE_OVERLAYS_H */
