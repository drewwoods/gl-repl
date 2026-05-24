/*
 * glr_camera_export.c -- ReplExportCameraBridge implementation.
 *
 * The camera-block format owner: translates between camera state and the
 * 4-line `// camera` block + `static float g_angle = N.NNNNf;` preamble
 * that appear in saved files. src/repl/export.c sees only the neutral
 * ReplExportCameraBlock + a single try_consume_import_line entry point;
 * it does not parse or format glRotatef/glTranslatef strings.
 *
 * The format has two variants:
 *   - "save": line 3 is the literal `glRotatef(g_angle, 0,1,0)`
 *     placeholder. The saved file animates camera rotation by writing
 *     g_angle every frame.
 *   - "display": line 3 uses the numeric ry value. Used for the
 *     code-panel preview (g_cam_lines).
 *
 * Older exports also wrote a literal `glRotatef(ry_value, 0,1,0)` line
 * before the g_angle placeholder; the import parser tolerates either
 * form so existing saved files keep working.
 *
 * (Bridge introduced as step 4a of
 * feature/decouple-repl-from-gl-repl-alt.md.)
 */
#include "repl/export.h"
#include "app/glr_camera.h"
#include "app/glr_ctrl.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ----- Save-side formatting ------------------------------------------- */

static void cam_format_save_block(ReplExportCameraBlock *block) {
    GlrCameraState cam = glr_camera();
    snprintf(block->lines[0], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(0.0000f, 0.0000f, %.4ff);", -cam.dist);
    snprintf(block->lines[1], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);", cam.rx);
    snprintf(block->lines[2], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);");
    snprintf(block->lines[3], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(%.4ff, %.4ff, %.4ff);",
             -cam.tx, -cam.ty, -cam.tz);
    block->present = 1;
}

static void cam_format_display_block(ReplExportCameraBlock *block) {
    GlrCameraState cam = glr_camera();
    snprintf(block->lines[0], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(0.0000f, 0.0000f, %.4ff);", -cam.dist);
    snprintf(block->lines[1], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(%.4ff, 1.0f, 0.0f, 0.0f);", cam.rx);
    snprintf(block->lines[2], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glRotatef(%.4ff, 0.0f, 1.0f, 0.0f);", cam.ry);
    snprintf(block->lines[3], REPL_EXPORT_CAMERA_LINE_MAX,
             "  glTranslatef(%.4ff, %.4ff, %.4ff);",
             -cam.tx, -cam.ty, -cam.tz);
    block->present = 1;
}

static void cam_format_save_preamble(char *out, int out_sz) {
    if (!out || out_sz <= 0) return;
    snprintf(out, (size_t)out_sz,
             "static float g_angle = %.4ff;", glr_camera().ry);
}

/* ----- Import-side parser -------------------------------------------- *
 *
 * The state machine matches the saved-file shape:
 *
 *   state 0  expect first glTranslatef (camera distance).
 *   state 1  expect glRotatef rx (axis 1,0,0).
 *   state 2  expect either glRotatef(ry, 0,1,0)  -> stay in state 3
 *                       or glRotatef(g_angle, 0,1,0) -> jump to state 4.
 *   state 3  expect glRotatef(g_angle, ...). Tolerates absence and
 *            falls through to state 4 to try the target translate
 *            on the same line.
 *   state 4  expect glTranslatef target.
 *   state 5  done; later lines are not part of the camera block.
 *
 * The same parser also recognises the `static float g_angle = N.NNNNf;`
 * preamble line — it treats that as a free-standing camera input that
 * sets ry. */
static int g_cam_parse_state = 0;

static const char *cam_line_skip_sep(const char *p) {
    while (*p == 'f' || *p == 'F' || *p == ',' || *p == ' ' || *p == '\t')
        p++;
    return p;
}

static int cam_line_read_floats(const char *p, float *out, int n) {
    for (int i = 0; i < n; i++) {
        p = cam_line_skip_sep(p);
        char *end = NULL;
        out[i] = strtof(p, &end);
        if (end == p) return 0;
        p = end;
    }
    return 1;
}

/* The "static float g_angle = N.NNNNf;" preamble may appear before the
 * camera block. Treat it like another camera input — it sets ry. */
static int cam_try_parse_angle_preamble(const char *text) {
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "static float g_angle", 20) != 0)
        return 0;
    const char *eq = strchr(p, '=');
    if (!eq) return 0;
    eq++;
    char *end = NULL;
    float v = strtof(eq, &end);
    if (end == eq) return 0;
    glr_camera_set_orbit(glr_camera().rx, v);
    return 1;
}

static int cam_try_consume_block_line(const char *text) {
    if (g_cam_parse_state >= 5) return 0;

    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;

    if (g_cam_parse_state == 0 && strncmp(p, "glTranslatef", 12) == 0) {
        p = strchr(p, '(');
        if (!p) return 0;
        p++;
        float v[3];
        if (!cam_line_read_floats(p, v, 3)) return 0;
        glr_camera_set_distance(-v[2]);
        g_cam_parse_state = 1;
        return 1;
    }

    if (g_cam_parse_state == 1 && strncmp(p, "glRotatef", 9) == 0) {
        p = strchr(p, '(');
        if (!p) return 0;
        p++;
        float v[4];
        if (!cam_line_read_floats(p, v, 4)) return 0;
        if (v[1] != 1.0f || v[2] != 0.0f || v[3] != 0.0f) return 0;
        glr_camera_set_orbit(v[0], glr_camera().ry);
        g_cam_parse_state = 2;
        return 1;
    }

    if (g_cam_parse_state == 2 && strncmp(p, "glRotatef", 9) == 0) {
        const char *q = strchr(p, '(');
        if (q && strstr(q, "g_angle")) {
            g_cam_parse_state = 4;
            return 1;
        }
        p = strchr(p, '(');
        if (!p) return 0;
        p++;
        float v[4];
        if (!cam_line_read_floats(p, v, 4)) return 0;
        if (v[1] != 0.0f || v[2] != 1.0f || v[3] != 0.0f) return 0;
        glr_camera_set_orbit(glr_camera().rx, v[0]);
        g_cam_parse_state = 3;
        return 1;
    }

    if (g_cam_parse_state == 3) {
        if (strncmp(p, "glRotatef", 9) == 0) {
            const char *q = strchr(p, '(');
            if (q && strstr(q, "g_angle")) {
                g_cam_parse_state = 4;
                return 1;
            }
        }
        g_cam_parse_state = 4;
        /* fall through to try the target translate on the same line */
    }

    if (g_cam_parse_state == 4 && strncmp(p, "glTranslatef", 12) == 0) {
        p = strchr(p, '(');
        if (!p) return 0;
        p++;
        float v[3];
        if (!cam_line_read_floats(p, v, 3)) return 0;
        glr_camera_set_pan(-v[0], -v[1], -v[2]);
        g_cam_parse_state = 5;
        return 1;
    }

    return 0;
}

static int cam_try_consume_import_line(const char *text) {
    if (cam_try_parse_angle_preamble(text)) return 1;
    return cam_try_consume_block_line(text);
}

static void cam_reset_import(void) {
    g_cam_parse_state = 0;
}

static int cam_consume_example_block_now(const ReplExportCameraBlock *block) {
    if (!block || !block->present)
        return 0;

    cam_reset_import();
    if (!cam_try_consume_import_line(block->lines[0])) return 0;
    if (!cam_try_consume_import_line(block->lines[1])) return 0;
    if (!cam_try_consume_import_line(block->lines[2])) return 0;
    if (!cam_try_consume_import_line("glRotatef(g_angle, 0, 1, 0)")) return 0;
    if (!cam_try_consume_import_line(block->lines[3])) return 0;
    return 1;
}

static void cam_apply_example_block(const ReplExportCameraBlock *block) {
    GlrCameraState start;
    GlrCameraState target;

    if (!block || !block->present)
        return;

    glr_camera_capture(&start);
    if (!cam_consume_example_block_now(block)) {
        cam_reset_import();
        glr_camera_restore(&start);
        return;
    }
    glr_camera_capture(&target);
    cam_reset_import();
    glr_camera_restore(&start);
    glr_camera_ease_to(target.rx, target.ry, target.dist,
                       target.tx, target.ty, target.tz);
    /* Keep the controller's saved-3D snapshot aligned with the example
     * the user is now looking at: if we're dwelling in 2D, the visible
     * camera will ease to this pose under ortho, but without this hook
     * the 2D->3D restoration would still land on the pose captured at
     * 2D entry (typically stale, sometimes flat) instead of the
     * currently-loaded example's intended angle. */
    glr_ctrl_view_record_external_3d_pose(target.rx, target.ry, target.tz);
}

/* ----- Bridge install ------------------------------------------------- */

static const ReplExportCameraBridge g_glr_export_camera_bridge = {
    .fill_save_block        = cam_format_save_block,
    .fill_display_block     = cam_format_display_block,
    .fill_save_preamble     = cam_format_save_preamble,
    .try_consume_import_line = cam_try_consume_import_line,
    .reset_import           = cam_reset_import,
    .apply_example_block    = cam_apply_example_block,
};

void glr_camera_export_install_bridge(void) {
    repl_export_install_camera_bridge(&g_glr_export_camera_bridge);
}
