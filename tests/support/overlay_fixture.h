#ifndef TESTS_SUPPORT_OVERLAY_FIXTURE_H
#define TESTS_SUPPORT_OVERLAY_FIXTURE_H

#include <stddef.h>

#include "app/glr_ctrl.h"           /* glr_ctrl_reset_all */
#include "app/glr_overlay_setup.h"  /* glr_overlay_fill_walk_ctx */
#include "app/glr_state.h"          /* glr_state_presentation_mut */
#include "editor/input.h"           /* editor_feed_line */
#include "editor/state.h"           /* editor_state_edit_line_set */
#include "repl/flatten.h"           /* repl_flatten_commands */
#include "repl/flatten_query.h"     /* repl_flatten_refresh_current_block_highlight */
#include "subsystems/edit_overlays/edit_overlays.h" /* OverlayWalkCtx, OverlayScope */

/* Overlay fixtures built from real pipeline output.
 *
 * A hand-built flat array (mk_cmd) can describe a program the pipeline never
 * produces - two contours sharing one gluEnd(), provenance that disagrees
 * with the source rows - and a test over it then pins behavior no scene can
 * reach. These helpers take the SOURCE rows a user would type and run the
 * same steps a frame runs: commit each line, flatten, refresh the cursor's
 * block cache, then fill the OverlayWalkCtx through glr_overlay_fill_walk_ctx
 * (the one fill the frame and bench_repl use), so the cursor block state, the
 * single-polygon primitive range and the contour range are the ones the
 * overlay passes are really handed.
 *
 * Default to these. Keep a hand-built mk_cmd array only for a shape the parser
 * cannot produce (an invalid command in the middle of a program, a flat range
 * that deliberately disagrees with the source extent, ...) and say why next to
 * it.
 *
 * The fill leaves every presentation toggle at the live default, so a test
 * sets the ones it exercises on the returned context (show_vertex_outlines,
 * highlight_current_poly, ...). */

/* Reset the app, commit `lines` as the document and flatten it with the edit
 * line on `cursor` (-1 = no cursor row). Returns 0 if a line is rejected. */
static inline int overlay_fixture_load(const char *const *lines, int count,
                                       int cursor) {
    glr_ctrl_reset_all();
    for (int i = 0; i < count; i++) {
        if (!editor_feed_line(lines[i]))
            return 0;
    }
    if (cursor >= 0)
        editor_state_edit_line_set(cursor);
    repl_flatten_commands(cursor);
    return 1;
}

/* Fill `out` for `cursor` over the program already loaded, under overlay scope
 * `scope`. Use it directly to sweep cursor rows (or scopes) over a document
 * from overlay_fixture_load() or repl_load_example(). The live scope setting is
 * put back afterwards. */
static inline void overlay_fixture_fill(int cursor, OverlayScope scope,
                                        OverlayWalkCtx *out) {
    GlrPresentationState *presentation = glr_state_presentation_mut();
    int saved_scope = presentation->overlay_scope;

    presentation->overlay_scope = (int)scope;
    repl_flatten_refresh_current_block_highlight(cursor);
    glr_overlay_fill_walk_ctx(cursor, out);
    presentation->overlay_scope = saved_scope;
}

/* Load and fill in one step. Returns 0 if a source line is rejected. */
static inline int overlay_fixture_build(const char *const *lines, int count,
                                        int cursor, OverlayScope scope,
                                        OverlayWalkCtx *out) {
    if (!overlay_fixture_load(lines, count, cursor))
        return 0;
    overlay_fixture_fill(cursor, scope, out);
    return 1;
}

/* OVERLAY_FIXTURE(&ctx, cursor, scope, "glBegin(GL_QUADS);", ...) - the source
 * rows inline. Evaluates to 1 on success, 0 if a row is rejected. */
#define OVERLAY_FIXTURE(out, cursor, scope, ...)                              \
    overlay_fixture_build((const char *const[]){ __VA_ARGS__ },               \
        (int)(sizeof((const char *const[]){ __VA_ARGS__ }) /                  \
              sizeof(const char *)),                                          \
        (cursor), (scope), (out))

#endif /* TESTS_SUPPORT_OVERLAY_FIXTURE_H */
