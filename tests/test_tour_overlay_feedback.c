/*
 * test_tour_overlay_feedback.c - real-GL geometry checks for the controlled-
 * tour overlay + HUD, captured with GL_FEEDBACK.
 *
 * Why a real-GL test
 * ------------------
 * The transport state machine is fully covered under GL stubs
 * (test_glr_tour_transport), but two properties are only expressed as drawn
 * geometry and the no-op stubs rasterize nothing:
 *
 *   1. Seek overlay suppression. A backstep fast-replays the event prefix with
 *      historical ring/echo/ripple SUPPRESSED and only the final replayed
 *      event's overlay kept. The ring/echo state is file-static in
 *      glr_pointer_script.c with no accessor, so the only way to observe it
 *      without touching production is to look at what the overlay pass draws.
 *   2. HUD containment. The HUD panel must never be forced wider than the scene
 *      (else it spills into a side code panel) — a geometric property of the
 *      panel rectangle.
 *
 * Rather than read pixels, this uses GL_FEEDBACK (the same mechanism the PLY
 * exporter uses in glr_mesh_export.c): in feedback mode primitives are not
 * rasterized; their post-transform vertices are written to a buffer as tokens.
 * With GL_2D each vertex is (x, y) in window pixels — exact and deterministic,
 * no anti-aliasing fuzz. The production render entry points
 * (glr_pointer_script_render_overlay / tour_ui_hud_render) are called AS-IS
 * inside a feedback bracket; they don't know they're being captured, so no
 * test-only hook lands in production. Bitmap text is opaque to feedback (it
 * emits only a raster-position token, not glyph geometry), so this asserts on
 * the ring line-loops and the HUD panel rect, not on text.
 *
 * In GL_TEST_BINS / `make gl-tests` (needs a real context; a display, or
 * FREEGLUT_OSMESA=1 headless) — NOT in `make test` / `make test-stubs`.
 */
#include "gl_includes.h"
#include "app/glr_pointer_script.h"
#include "app/glr_ctrl.h"
#include "ui/app/state.h"
#include "ui/app/layout.h"
#include "ui/app/snapshot.h"
#include "ui/subsystems/tour_hud.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) && !defined(FREEGLUT_OSMESA)
#include <ApplicationServices/ApplicationServices.h>
#endif

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, g, e)  TEST_ASSERT_INT(&g_harness, label, g, e)

#define WIN_W 1200
#define WIN_H 800

/* --- GL_2D feedback token walker ---------------------------------------- */

#define FB_CAP    32768
#define VERT_CAP  4096
static GLfloat g_fb[FB_CAP];
static float g_vx[VERT_CAP];
static float g_vy[VERT_CAP];

/* Extract every vertex (x, y) from a GL_2D feedback buffer of `n` values into
 * g_vx/g_vy. Returns the vertex count (clamped to VERT_CAP). Token layout is
 * the classic feedback grammar: a token float, then that token's operands. */
static int parse_gl2d_vertices(const GLfloat *fb, int n) {
    int i = 0, count = 0;
    while (i < n) {
        int tok = (int)fb[i++];
        int nverts;
        switch (tok) {
        case GL_PASS_THROUGH_TOKEN: i += 1; continue;  /* one scalar operand */
        case GL_POINT_TOKEN:
        case GL_BITMAP_TOKEN:
        case GL_DRAW_PIXEL_TOKEN:
        case GL_COPY_PIXEL_TOKEN:   nverts = 1; break;
        case GL_LINE_TOKEN:
        case GL_LINE_RESET_TOKEN:   nverts = 2; break;
        case GL_POLYGON_TOKEN:
            if (i >= n) return count;
            nverts = (int)fb[i++];
            break;
        default:
            return count;              /* unknown token: stop defensively */
        }
        for (int k = 0; k < nverts; k++) {
            if (i + 1 >= n) return count;
            if (count < VERT_CAP) {
                g_vx[count] = fb[i];
                g_vy[count] = fb[i + 1];
                count++;
            }
            i += 2;                     /* GL_2D: two floats per vertex */
        }
    }
    return count;
}

static int count_near(int count, float cx, float cy, float r) {
    int c = 0;
    for (int i = 0; i < count; i++) {
        float dx = g_vx[i] - cx, dy = g_vy[i] - cy;
        if (dx * dx + dy * dy <= r * r)
            c++;
    }
    return c;
}

/* Capture the tour overlay pass in feedback mode, returning the vertex count. */
static int capture_overlay(void) {
    glViewport(0, 0, WIN_W, WIN_H);   /* overlay's gl2d_begin sets no viewport */
    glFeedbackBuffer(FB_CAP, GL_2D, g_fb);
    glRenderMode(GL_FEEDBACK);
    glr_pointer_script_render_overlay(WIN_W, WIN_H);
    int n = glRenderMode(GL_RENDER);
    if (n < 0) return -1;             /* buffer overflow */
    return parse_gl2d_vertices(g_fb, n);
}

/* Capture the tour HUD pass in feedback mode. */
static int capture_hud(void) {
    UiRenderSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.viewport = ui_state_viewport();
    snap.tour = glr_pointer_script_tour_view();
    glFeedbackBuffer(FB_CAP, GL_2D, g_fb);
    glRenderMode(GL_FEEDBACK);
    tour_ui_hud_render(&snap);         /* sets its own viewport + ortho */
    int n = glRenderMode(GL_RENDER);
    if (n < 0) return -1;
    return parse_gl2d_vertices(g_fb, n);
}

/* --- tour driving (mirrors test_glr_tour_transport) --------------------- */

static void start_tour(const char *const *lines, int n) {
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(WIN_W, WIN_H);
    ui_state_pointer_set(100, 100, -1);
    glr_pointer_script_start_tour("Overlay Tour", "overlay.pointer", lines, n);
    glr_pointer_script_frame();        /* baseline captured -> Playing */
}

static void step_right(int times) {
    for (int i = 0; i < times; i++)
        glr_pointer_script_handle_tour_special(GLUT_KEY_RIGHT);
}

static int run_until_paused(int max_frames) {
    for (int i = 0; i < max_frames; i++) {
        if (glr_pointer_script_tour_view().state == GLR_TOUR_PAUSED)
            return 1;
        glr_pointer_script_frame();
    }
    return glr_pointer_script_tour_view().state == GLR_TOUR_PAUSED;
}

/* The ring at (400, 300) mouse-space maps to (400, WIN_H - 300) in the y-up
 * gl2d overlay: its center for the region query. */
#define RING_CX 400.0f
#define RING_CY (WIN_H - 300.0f)
#define RING_R  45.0f

/* --- tests -------------------------------------------------------------- */

/* Backstep past a ring to a non-ring boundary suppresses the historical ring:
 * the overlay pass draws no geometry near the ring's screen position. */
static void test_seek_suppresses_historical_ring(void) {
    const char *lines[] = {
        "ring 400 300 5",    /* event 0: a long ring, earlier in the prefix */
        "move 100 100",      /* event 1 */
        "move 200 200",      /* event 2 */
        "move 300 300",      /* event 3 */
    };
    start_tour(lines, 4);
    step_right(3);           /* completed 3, paused, current -1 */
    ASSERT_INT("stepped to boundary 3",
               glr_pointer_script_tour_view().completed_events, 3);

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);   /* target = 2 */
    ASSERT_TRUE("seek settles", run_until_paused(20));
    ASSERT_INT("landed at boundary 2",
               glr_pointer_script_tour_view().completed_events, 2);

    int verts = capture_overlay();
    ASSERT_TRUE("overlay captured (cursor present)", verts > 0);
    ASSERT_INT("historical ring suppressed (no geometry at ring point)",
               count_near(verts, RING_CX, RING_CY, RING_R), 0);
}

/* Backstep that lands ON a ring keeps that final replayed event's overlay:
 * the ring is drawn at its screen position. */
static void test_seek_keeps_final_ring(void) {
    const char *lines[] = {
        "move 100 100",      /* event 0 */
        "ring 400 300 5",    /* event 1: the final replayed event at target 2 */
        "move 200 200",      /* event 2 */
        "move 300 300",      /* event 3 */
    };
    start_tour(lines, 4);
    step_right(3);           /* completed 3, paused */

    glr_pointer_script_handle_tour_special(GLUT_KEY_LEFT);   /* target = 2 */
    ASSERT_TRUE("seek settles", run_until_paused(20));

    int verts = capture_overlay();
    ASSERT_TRUE("final replayed ring drawn at its point",
                count_near(verts, RING_CX, RING_CY, RING_R) > 0);
}

/* Stepping past a ring clears its (frozen-clock) overlay: after a Right that
 * lands on a non-ring event, no ring geometry remains. Guards the frozen-ring
 * bug directly. */
static void test_step_past_ring_clears_overlay(void) {
    const char *lines[] = {
        "ring 400 300 5",    /* event 0 */
        "move 100 100",      /* event 1 */
    };
    start_tour(lines, 2);
    step_right(1);           /* lands ON the ring: it is drawn */
    ASSERT_TRUE("ring present at its point after landing on it",
                count_near(capture_overlay(), RING_CX, RING_CY, RING_R) > 0);

    step_right(1);           /* steps PAST the ring onto the move */
    ASSERT_INT("ring overlay cleared after stepping past",
               count_near(capture_overlay(), RING_CX, RING_CY, RING_R), 0);
}

/* Validate the HUD render path end-to-end: the drawn panel sits at the scene's
 * top-left inset and its captured width matches tour_hud_panel_width(scene_w)
 * exactly — tying the real GL output to the pure width function whose clamp is
 * guarded arithmetically in test_glr_tour_transport. (Feedback clips at the
 * window edge, so it cannot observe off-window overflow directly; the width
 * cross-check is what actually pins the panel size.) */
static void test_hud_render_matches_width_helper(void) {
    const char *lines[] = { "move 100 100", "move 200 200" };
    glr_ctrl_reset_all();
    ui_state_viewport_set_size(1200, 800);
    ui_state_pointer_set(100, 100, -1);
    glr_pointer_script_start_tour("HUD Tour", "hud.pointer", lines, 2);
    glr_pointer_script_frame();   /* -> Playing, HUD active */
    glr_pointer_script_toggle_tour_hud();   /* expand: full-width panel */

    int sx, sy, sw, sh;
    ui_layout_scene_rect(&sx, &sy, &sw, &sh);
    int expect_w = tour_hud_panel_width(sw);
    ASSERT_TRUE("scene wide enough that the HUD renders", expect_w > 0);

    int verts = capture_hud();
    ASSERT_TRUE("HUD geometry captured", verts > 0);

    float min_x = g_vx[0], max_x = g_vx[0];
    float min_y = g_vy[0], max_y = g_vy[0];
    for (int i = 1; i < verts; i++) {
        if (g_vx[i] < min_x) min_x = g_vx[i];
        if (g_vx[i] > max_x) max_x = g_vx[i];
        if (g_vy[i] < min_y) min_y = g_vy[i];
        if (g_vy[i] > max_y) max_y = g_vy[i];
    }
    fprintf(stderr,
            "  hud scene=(%d,%d,%d,%d) drawn x[%.0f..%.0f] y[%.0f..%.0f]"
            " expect_w=%d\n",
            sx, sy, sw, sh, min_x, max_x, min_y, max_y, expect_w);

    ASSERT_TRUE("HUD left edge at the scene inset", min_x >= (float)sx - 1.0f);
    ASSERT_TRUE("HUD stays within the scene vertically",
                min_y >= (float)sy - 1.0f && max_y <= (float)(sy + sh) + 1.0f);
    /* The drawn panel width equals the pure helper's value (± 1px for the
     * half-pixel border insets). */
    ASSERT_TRUE("drawn panel width matches tour_hud_panel_width()",
                (max_x - min_x) >= (float)expect_w - 1.5f &&
                (max_x - min_x) <= (float)expect_w + 1.5f);
}

int main(int argc, char **argv) {
#if defined(__APPLE__) && !defined(FREEGLUT_OSMESA)
    uint32_t display_count = 0;
    if (CGGetActiveDisplayList(0, NULL, &display_count) != kCGErrorSuccess ||
        display_count == 0) {
        fprintf(stderr, "test_tour_overlay_feedback: no active macOS display; "
                        "skipping\n");
        return 0;
    }
#endif
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH);
    glutInitWindowSize(WIN_W, WIN_H);
    if (glutCreateWindow("tour-overlay-feedback-test") <= 0) {
        fprintf(stderr, "test_tour_overlay_feedback: no GL context "
                        "(need a display); skipping\n");
        return 0;
    }

    printf("--- tour overlay/HUD feedback tests (real GL context) ---\n");
    test_seek_suppresses_historical_ring();
    test_seek_keeps_final_ring();
    test_step_past_ring_clears_overlay();
    test_hud_render_matches_width_helper();
    return test_harness_report(&g_harness, "tour_overlay_feedback");
}
