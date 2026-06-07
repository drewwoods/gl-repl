/*
 * transform_guides.c - transform edit-guide planning/rendering.
 */
#include "transform_guides.h"
#include "transform_utils.h"
#include "scene/palette.h"
#include "scene/occluded_ghost.h"

#include <ctype.h>  /* isspace */
#include <math.h>   /* sqrtf, fminf, fmodf, cosf, sinf, fabsf, M_PI */
#include <string.h> /* strncmp, strlen */

static void transform_guides_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void transform_guides_pop_state(void) {
    glPopAttrib();
}

/* Shared arrowhead sizing, used by every translate/scale guide path:
 * head length = clamp(segment_len * FRAC, MIN, MAX), fins are FIN_FRAC
 * of the head length. TG_ARC_SEGS is the rotate-guide arc resolution;
 * the arc vertex buffer is sized TG_ARC_SEGS + 1 so the two stay
 * coupled by construction instead of by hand. */
#define TG_HEAD_LEN_FRAC 0.22f
#define TG_HEAD_LEN_MAX  0.25f
#define TG_HEAD_LEN_MIN  0.06f
/* Translate/scale World-mode axis tips ride closer to the per-axis
 * gizmo geometry, where the standard MAX/MIN caps look oversized;
 * the axis branch uses a tighter pair. */
#define TG_HEAD_LEN_AXIS_MAX 0.20f
#define TG_HEAD_LEN_AXIS_MIN 0.05f
#define TG_FIN_FRAC      0.45f
#define TG_ARC_SEGS      48

/* Clamp the head length to the [min, max] ladder. When dlen itself is
 * below min we still want a proportional little nubbin, hence the
 * dlen*0.5 path; otherwise saturate at min. Caller picks which
 * (max, min) pair applies (axis-branch tip vs the default tip). */
static float clamp_head_len(float dlen, float frac, float min_len, float max_len) {
    float head_len = dlen * frac;
    if (head_len > max_len) head_len = max_len;
    if (head_len < min_len) head_len = (dlen < min_len ? dlen * 0.5f : min_len);
    return head_len;
}

/* Per-pass alpha multiplier (formerly the file-static
 * g_guide_alpha_mul). The render dispatcher draws each guide twice:
 * a depth-test-off ghost pass at SCENE_OCCLUDED_GHOST_ALPHA (~0.4)
 * so rotated geometry can't fully hide the guide, then a
 * depth-tested solid pass at 1.0 on top. The value is threaded as a
 * parameter through every draw helper so an early-return or
 * exception couldn't strand a 40%-alpha state and cripple
 * subsequent frames. */
static void tg_color4f(float r, float g, float b, float a, float alpha_mul) {
    a *= alpha_mul;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    glColor4f(r, g, b, a);
}

/* Build an orthonormal {r, b} pair perpendicular to a unit `dir`.
 * Used by every arrowhead/fin builder (draw_translate_guide,
 * draw_arrow_head, draw_scale_guide's axis branch) to get a frame for
 * the four fin offsets. The 0.9f axis-aligned threshold picks the world
 * up-axis the cross product picks against; outputs are normalized when
 * `dir` isn't already parallel to the chosen up.
 *
 * dir is assumed unit-length (callers already normalize). r and b
 * receive unit-length perpendicular vectors completing the basis. */
static void make_arrow_basis(const float dir[3], float r[3], float b[3]) {
    float ux, uy, uz;
    if (fabsf(dir[1]) < 0.9f) { ux = 0.0f; uy = 1.0f; uz = 0.0f; }
    else                      { ux = 1.0f; uy = 0.0f; uz = 0.0f; }
    r[0] = dir[1]*uz - dir[2]*uy;
    r[1] = dir[2]*ux - dir[0]*uz;
    r[2] = dir[0]*uy - dir[1]*ux;
    float rlen = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    if (rlen > 1e-8f) { r[0] /= rlen; r[1] /= rlen; r[2] /= rlen; }
    b[0] = r[1]*dir[2] - r[2]*dir[1];
    b[1] = r[2]*dir[0] - r[0]*dir[2];
    b[2] = r[0]*dir[1] - r[1]*dir[0];
}

static void mat4_mul_col_major(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            out[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

/* Walk flat cmds strictly before cursor_flat_idx, applying only transform
 * cmds to a fresh identity matrix. Returns the full scene-world frame at the
 * cursor line, so Frame guide mode can account for prior rotations as well as
 * translations. */
static void compute_before_cursor_matrix(const SceneGuideSnapshot *snapshot,
                                         int cursor_flat_idx,
                                         float out[16]) {
    const GLCmd *flat_cmds = snapshot->flat_program.cmds;
    glPushMatrix();
    glLoadIdentity();
    int depth = 0;
    for (int i = 0; i < cursor_flat_idx; i++) {
        if (!flat_cmds[i].valid) continue;
        if (repl_cmd_is_transform(flat_cmds[i].type))
            apply_tracked_transform(&flat_cmds[i], &depth);
    }
    glGetFloatv(GL_MODELVIEW_MATRIX, out);
    unwind_transform_stack(&depth);
    glPopMatrix();
}

/* Walk flat cmds forward from first_after_idx, applying only transform cmds
 * to a fresh identity matrix (keeps its own push/pop depth so nested blocks
 * don't leak). Stops at the first rendering action so transforms that come
 * after an intervening draw don't bleed into the guide. Returns the origin
 * transformed by the accumulated matrix. */
static void compute_after_cursor_origin(const SceneGuideSnapshot *snapshot,
                                        int first_after_idx,
                                        float out[3]) {
    const GLCmd *flat_cmds = snapshot->flat_program.cmds;
    int flat_cmd_count = snapshot->flat_program.cmd_count;

    out[0] = out[1] = out[2] = 0.0f;
    glPushMatrix();
    glLoadIdentity();
    int depth = 0;
    for (int i = first_after_idx; i < flat_cmd_count; i++) {
        if (!flat_cmds[i].valid) continue;
        if (repl_cmd_starts_geometry_emit(flat_cmds[i].type)) break;
        if (repl_cmd_is_transform(flat_cmds[i].type))
            apply_tracked_transform(&flat_cmds[i], &depth);
    }
    float m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
    unwind_transform_stack(&depth);
    glPopMatrix();
}

/* Color from the normalized absolute component values of a vector. Maps
 * (|x|,|y|,|z|) / max to RGB so an axis-aligned translation reads as a pure
 * axis color (X=red, Y=green, Z=blue) and diagonals blend. Falls back to a
 * neutral gray for zero vectors. */
static void xform_axis_color(float x, float y, float z, float out[3]) {
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    float m = ax;
    if (ay > m) m = ay;
    if (az > m) m = az;
    if (m < 1e-6f) {
        out[0] = out[1] = out[2] = 0.85f;
        return;
    }
    out[0] = ax / m;
    out[1] = ay / m;
    out[2] = az / m;
}

/* Pulse shader for a straight segment in the axes-pulse style: a dim solid
 * base line, a bright dot traveling a→b, and a short trail behind the dot. */
static void draw_pulse_segment(const SceneGuideSnapshot *snapshot,
                               const float a[3], const float b[3],
                               const float rgb[3], float alpha_mul) {
    float as = snapshot->alpha_scale;
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    tg_color4f(rgb[0], rgb[1], rgb[2], fminf(0.30f * as, 1.0f), alpha_mul);
    glVertex3f(a[0], a[1], a[2]);
    glVertex3f(b[0], b[1], b[2]);
    glEnd();

    float ph = fmodf(snapshot->anim_time * 0.6f, 1.0f);
    float glow = sinf(ph * (float)M_PI) * 0.8f + 0.2f;
    float pos[3] = {
        a[0] + (b[0] - a[0]) * ph,
        a[1] + (b[1] - a[1]) * ph,
        a[2] + (b[2] - a[2]) * ph
    };
    float tp = ph - 0.25f;
    if (tp < 0.0f) tp = 0.0f;
    float trail[3] = {
        a[0] + (b[0] - a[0]) * tp,
        a[1] + (b[1] - a[1]) * tp,
        a[2] + (b[2] - a[2]) * tp
    };

    glLineWidth(3.5f);
    glBegin(GL_LINES);
    tg_color4f(rgb[0], rgb[1], rgb[2], fminf(0.05f * as, 1.0f), alpha_mul);
    glVertex3f(trail[0], trail[1], trail[2]);
    tg_color4f(rgb[0], rgb[1], rgb[2], fminf(glow * 0.75f * as, 1.0f), alpha_mul);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glLineWidth(1.0f);

    glPointSize(8.0f);
    glBegin(GL_POINTS);
    tg_color4f(rgb[0], rgb[1], rgb[2], fminf(glow * as, 1.0f), alpha_mul);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glPointSize(1.0f);
}

/* Arrow starting at p_after in the current local frame and extending by the
 * translate command's vector. Shaft is an axes-pulse-style traveling dot
 * over a dim base line; the solid 4-fin arrowhead at the tip keeps the
 * direction unambiguous. Shaft color is (|tx|,|ty|,|tz|)/max mapped to RGB. */
static void draw_translate_guide(const SceneGuideSnapshot *snapshot,
                                 const GLCmd *cmd, const float p_after[3],
                                 float alpha_mul) {
    float tx = cmd->args[0], ty = cmd->args[1], tz = cmd->args[2];
    float p0[3] = { p_after[0], p_after[1], p_after[2] };
    float p1[3] = { p0[0] + tx, p0[1] + ty, p0[2] + tz };

    float dlen = sqrtf(tx*tx + ty*ty + tz*tz);
    if (dlen < 1e-6f)
        return;
    float dir[3] = { tx / dlen, ty / dlen, tz / dlen };
    float dx = dir[0], dy = dir[1], dz = dir[2];

    float rvec[3], bvec[3];
    make_arrow_basis(dir, rvec, bvec);
    float rx = rvec[0], ry = rvec[1], rz = rvec[2];
    float bx = bvec[0], by = bvec[1], bz = bvec[2];

    float head_len = clamp_head_len(dlen, TG_HEAD_LEN_FRAC,
                                    TG_HEAD_LEN_MIN, TG_HEAD_LEN_MAX);
    float fin = head_len * TG_FIN_FRAC;
    float base[3] = {
        p1[0] - dx * head_len,
        p1[1] - dy * head_len,
        p1[2] - dz * head_len
    };

    float rgb[3];
    xform_axis_color(tx, ty, tz, rgb);
    float head_rgb[3] = {
        rgb[0] * 0.6f + 0.4f,
        rgb[1] * 0.6f + 0.4f,
        rgb[2] * 0.6f + 0.4f
    };

    transform_guides_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    draw_pulse_segment(snapshot, p0, base, rgb, alpha_mul);

    glLineWidth(3.0f);
    tg_color4f(head_rgb[0], head_rgb[1], head_rgb[2], 0.95f, alpha_mul);
    glBegin(GL_LINES);
    for (int i = 0; i < 4; i++) {
        float sx = (i == 0 ?  rx : i == 1 ? -rx : i == 2 ?  bx : -bx);
        float sy = (i == 0 ?  ry : i == 1 ? -ry : i == 2 ?  by : -by);
        float sz = (i == 0 ?  rz : i == 1 ? -rz : i == 2 ?  bz : -bz);
        glVertex3f(p1[0], p1[1], p1[2]);
        glVertex3f(base[0] + sx * fin, base[1] + sy * fin, base[2] + sz * fin);
    }
    glVertex3f(base[0] + rx*fin, base[1] + ry*fin, base[2] + rz*fin);
    glVertex3f(base[0] + bx*fin, base[1] + by*fin, base[2] + bz*fin);
    glVertex3f(base[0] + bx*fin, base[1] + by*fin, base[2] + bz*fin);
    glVertex3f(base[0] - rx*fin, base[1] - ry*fin, base[2] - rz*fin);
    glVertex3f(base[0] - rx*fin, base[1] - ry*fin, base[2] - rz*fin);
    glVertex3f(base[0] - bx*fin, base[1] - by*fin, base[2] - bz*fin);
    glVertex3f(base[0] - bx*fin, base[1] - by*fin, base[2] - bz*fin);
    glVertex3f(base[0] + rx*fin, base[1] + ry*fin, base[2] + rz*fin);
    glEnd();
    glLineWidth(1.0f);

    glPointSize(6.0f);
    glBegin(GL_POINTS);
    tg_color4f(rgb[0], rgb[1], rgb[2], 0.7f, alpha_mul);
    glVertex3f(p0[0], p0[1], p0[2]);
    glEnd();
    glPointSize(9.0f);
    glBegin(GL_POINTS);
    tg_color4f(head_rgb[0], head_rgb[1], head_rgb[2], 1.0f, alpha_mul);
    glVertex3f(p1[0], p1[1], p1[2]);
    glEnd();
    glPointSize(1.0f);

    glDisable(GL_BLEND);
    transform_guides_pop_state();
}

/* Arc from p_start swept by glRotatef(angle, ax,ay,az) about local origin. */
/* Helper: small 4-fin arrowhead from `tip` pointing along `dir` (unit). */
static void draw_arrow_head(const float tip[3], const float dir[3], float head_len) {
    float r[3], b[3];
    make_arrow_basis(dir, r, b);
    float base[3] = {
        tip[0] - dir[0] * head_len,
        tip[1] - dir[1] * head_len,
        tip[2] - dir[2] * head_len
    };
    float fin = head_len * TG_FIN_FRAC;
    glBegin(GL_LINES);
    for (int k = 0; k < 4; k++) {
        float sx = (k == 0 ?  r[0] : k == 1 ? -r[0] : k == 2 ?  b[0] : -b[0]);
        float sy = (k == 0 ?  r[1] : k == 1 ? -r[1] : k == 2 ?  b[1] : -b[1]);
        float sz = (k == 0 ?  r[2] : k == 1 ? -r[2] : k == 2 ?  b[2] : -b[2]);
        glVertex3f(tip[0], tip[1], tip[2]);
        glVertex3f(base[0] + sx * fin, base[1] + sy * fin, base[2] + sz * fin);
    }
    glEnd();
}

/* Scale guide. Draws the identity reference (origin → axis·1, gray) with a
 * bright tick at the 1.0 position on each axis, then a pulse arrow showing
 * only the distortion - from the 1.0 tick to the scaled tip. Scale of 1 on
 * an axis draws no pulse (identity); negative factors produce an arrow that
 * passes through origin. The non-origin branch is the World-mode variant
 * where p_start is the actual anchor point and the 1.0 reference is p_start
 * itself; at exact scale (1,1,1) only the marker is drawn. */
static void draw_scale_guide(const SceneGuideSnapshot *snapshot,
                             const GLCmd *cmd, const float p_start[3],
                             float alpha_mul) {
    float sx = cmd->args[0], sy = cmd->args[1], sz = cmd->args[2];
    float p0[3] = { p_start[0], p_start[1], p_start[2] };
    float plen = sqrtf(p0[0]*p0[0] + p0[1]*p0[1] + p0[2]*p0[2]);

    transform_guides_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (plen > 1e-4f) {
        float p1[3] = { p0[0]*sx, p0[1]*sy, p0[2]*sz };
        float delta[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
        float dlen = sqrtf(delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2]);

        float tick = 0.08f;
        glLineWidth(2.0f);
        scene_clr_a(SCENE_CLR_GUIDE_REF_TICK, 0.9f * alpha_mul);
        glBegin(GL_LINES);
        glVertex3f(p0[0]-tick, p0[1], p0[2]); glVertex3f(p0[0]+tick, p0[1], p0[2]);
        glVertex3f(p0[0], p0[1]-tick, p0[2]); glVertex3f(p0[0], p0[1]+tick, p0[2]);
        glVertex3f(p0[0], p0[1], p0[2]-tick); glVertex3f(p0[0], p0[1], p0[2]+tick);
        glEnd();
        glPointSize(6.0f);
        glBegin(GL_POINTS);
        scene_clr_a(SCENE_CLR_GUIDE_REF_POINT, 1.0f * alpha_mul);
        glVertex3f(p0[0], p0[1], p0[2]);
        glEnd();
        glPointSize(1.0f);

        if (dlen > 1e-4f) {
            float rgb[3];
            xform_axis_color(sx - 1.0f, sy - 1.0f, sz - 1.0f, rgb);
            float head_rgb[3] = {
                rgb[0]*0.6f + 0.4f,
                rgb[1]*0.6f + 0.4f,
                rgb[2]*0.6f + 0.4f
            };
            float dir[3] = { delta[0]/dlen, delta[1]/dlen, delta[2]/dlen };
            float head_len = clamp_head_len(dlen, TG_HEAD_LEN_FRAC,
                                            TG_HEAD_LEN_MIN, TG_HEAD_LEN_MAX);
            float base[3] = {
                p1[0] - dir[0] * head_len,
                p1[1] - dir[1] * head_len,
                p1[2] - dir[2] * head_len
            };

            draw_pulse_segment(snapshot, p0, base, rgb, alpha_mul);

            glLineWidth(3.0f);
            tg_color4f(head_rgb[0], head_rgb[1], head_rgb[2], 0.95f, alpha_mul);
            draw_arrow_head(p1, dir, head_len);
            glLineWidth(1.0f);

            glPointSize(9.0f);
            glBegin(GL_POINTS);
            tg_color4f(head_rgb[0], head_rgb[1], head_rgb[2], 1.0f, alpha_mul);
            glVertex3f(p1[0], p1[1], p1[2]);
            glEnd();
            glPointSize(1.0f);
        }
    } else {
        const float axes[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
        const int perp_a[3] = { 1, 0, 0 };
        const int perp_b[3] = { 2, 2, 1 };
        /* Bucket-3 local guide seed (see scene/palette.h): per-axis
         * X/Y/Z base the scale guide both draws directly and
         * arithmetically brightens (*0.6+0.4) for the arrowhead - same
         * computed-from-a-base character as xform_axis_color(), so it
         * stays local data, not palette tokens. */
        const float axis_rgb[3][3] = {
            {1.0f, 0.3f, 0.3f},
            {0.3f, 1.0f, 0.3f},
            {0.3f, 0.3f, 1.0f}
        };
        const float factors[3] = { sx, sy, sz };
        const float tick = 0.06f;
        for (int a = 0; a < 3; a++) {
            float f = factors[a];
            const float *ax = axes[a];
            const float *pa = axes[perp_a[a]];
            const float *pb = axes[perp_b[a]];

            glLineWidth(1.5f);
            scene_clr_a(SCENE_CLR_GUIDE_REF,
                        fminf(0.45f * snapshot->alpha_scale, 1.0f) * alpha_mul);
            glBegin(GL_LINES);
            glVertex3f(0.0f, 0.0f, 0.0f);
            glVertex3f(ax[0], ax[1], ax[2]);
            glEnd();

            glLineWidth(2.0f);
            scene_clr_a(SCENE_CLR_GUIDE_REF_TICK, 0.9f * alpha_mul);
            glBegin(GL_LINES);
            glVertex3f(ax[0] - pa[0]*tick, ax[1] - pa[1]*tick, ax[2] - pa[2]*tick);
            glVertex3f(ax[0] + pa[0]*tick, ax[1] + pa[1]*tick, ax[2] + pa[2]*tick);
            glVertex3f(ax[0] - pb[0]*tick, ax[1] - pb[1]*tick, ax[2] - pb[2]*tick);
            glVertex3f(ax[0] + pb[0]*tick, ax[1] + pb[1]*tick, ax[2] + pb[2]*tick);
            glEnd();
            glPointSize(5.0f);
            glBegin(GL_POINTS);
            scene_clr_a(SCENE_CLR_GUIDE_REF_POINT, 1.0f * alpha_mul);
            glVertex3f(ax[0], ax[1], ax[2]);
            glEnd();
            glPointSize(1.0f);

            if (fabsf(f - 1.0f) < 1e-4f) continue;

            float tip[3] = { ax[0]*f, ax[1]*f, ax[2]*f };
            float from[3] = { ax[0], ax[1], ax[2] };
            float delta[3] = { tip[0]-from[0], tip[1]-from[1], tip[2]-from[2] };
            float dlen = sqrtf(delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2]);
            float dir[3] = { delta[0]/dlen, delta[1]/dlen, delta[2]/dlen };

            float head_len = clamp_head_len(dlen, TG_HEAD_LEN_FRAC,
                                            TG_HEAD_LEN_AXIS_MIN,
                                            TG_HEAD_LEN_AXIS_MAX);
            float base[3] = {
                tip[0] - dir[0] * head_len,
                tip[1] - dir[1] * head_len,
                tip[2] - dir[2] * head_len
            };

            draw_pulse_segment(snapshot, from, base, axis_rgb[a], alpha_mul);

            glLineWidth(2.5f);
            tg_color4f(axis_rgb[a][0]*0.6f + 0.4f,
                      axis_rgb[a][1]*0.6f + 0.4f,
                      axis_rgb[a][2]*0.6f + 0.4f, 0.95f, alpha_mul);
            draw_arrow_head(tip, dir, head_len);
            glLineWidth(1.0f);

            glPointSize(7.0f);
            glBegin(GL_POINTS);
            tg_color4f(axis_rgb[a][0]*0.6f + 0.4f,
                      axis_rgb[a][1]*0.6f + 0.4f,
                      axis_rgb[a][2]*0.6f + 0.4f, 1.0f, alpha_mul);
            glVertex3f(tip[0], tip[1], tip[2]);
            glEnd();
            glPointSize(1.0f);
        }
    }

    glDisable(GL_BLEND);
    transform_guides_pop_state();
}

/* --- draw_rotate_guide phases ---
 *
 * Three distinct geometry generators + one shared pulse animator,
 * conditionally swept depending on whether the rotation origin is
 * (effectively) p_start (build_rotate_arc) or coincides with the
 * world origin (build_rotate_helix). The pulse runs along arc[]
 * regardless. Extracted into named helpers so a reader doesn't have
 * to mentally separate the rotation-matrix multiply from the helix
 * basis construction from the post-build draw. */

/* Rodrigues rotation: rotate p_start by angle_rad about the unit
 * axis (ax, ay, az), sampled at segs+1 points along [0, angle_rad].
 * arc[0] = p_start, arc[segs] is the final tip. */
static void build_rotate_arc(const float p_start[3],
                             float ax, float ay, float az,
                             float angle_rad, int segs,
                             float arc[][3]) {
    for (int i = 0; i <= segs; i++) {
        float t = (float)i / (float)segs;
        float th = angle_rad * t;
        float c = cosf(th), s = sinf(th), k = 1.0f - c;
        float px = p_start[0], py = p_start[1], pz = p_start[2];
        arc[i][0] = (c + ax*ax*k)    * px + (ax*ay*k - az*s) * py + (ax*az*k + ay*s) * pz;
        arc[i][1] = (ay*ax*k + az*s) * px + (c + ay*ay*k)    * py + (ay*az*k - ax*s) * pz;
        arc[i][2] = (az*ax*k - ay*s) * px + (az*ay*k + ax*s) * py + (c + az*az*k)    * pz;
    }
}

/* Origin-anchored rotation has no usable arc — the rotated p_start is
 * the same point. Build a helix instead, sampled along an axial span
 * proportional to angle_rad / tau. The {u, v} basis is computed from
 * a world-up helper, normalized, then v = a × u. */
static void build_rotate_helix(float ax, float ay, float az,
                               float angle_rad, float axis_len,
                               int segs, float arc[][3]) {
    float helper[3] = {1.0f, 0.0f, 0.0f};
    if (fabsf(ax) > 0.9f) { helper[0] = 0.0f; helper[1] = 1.0f; }
    float u[3] = {
        helper[1]*az - helper[2]*ay,
        helper[2]*ax - helper[0]*az,
        helper[0]*ay - helper[1]*ax,
    };
    float ul = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (ul < 1e-6f) ul = 1.0f;
    u[0] /= ul; u[1] /= ul; u[2] /= ul;
    float v[3] = {
        ay*u[2] - az*u[1],
        az*u[0] - ax*u[2],
        ax*u[1] - ay*u[0],
    };

    float radius = axis_len * 0.28f;
    float pitch  = axis_len * 0.50f;
    const float tau = 6.28318530717958647692f;
    float axial_span = pitch * (fabsf(angle_rad) / tau);
    if (axial_span > axis_len * 1.4f) axial_span = axis_len * 1.4f;
    float axial_start = -axial_span * 0.5f;

    for (int i = 0; i <= segs; i++) {
        float t = (float)i / (float)segs;
        float th = angle_rad * t;
        float c = cosf(th), s = sinf(th);
        float a = axial_start + axial_span * t;
        arc[i][0] = ax*a + radius * (c*u[0] + s*v[0]);
        arc[i][1] = ay*a + radius * (c*u[1] + s*v[1]);
        arc[i][2] = az*a + radius * (c*u[2] + s*v[2]);
    }
}

/* Animate a traveling glow dot + trail along arc[0..segs]. Same
 * shape as draw_pulse_segment's straight-line version, but the
 * sample positions are interpolated along the arc segments. */
static void draw_rotate_pulse(const SceneGuideSnapshot *snapshot,
                              const float arc[][3], int segs,
                              const float rgb[3], const float bright[3],
                              float alpha_mul) {
    float as = snapshot->alpha_scale;
    float ph = fmodf(snapshot->anim_time * 0.6f, 1.0f);
    float glow = sinf(ph * (float)M_PI) * 0.8f + 0.2f;

    float fpos = ph * (float)segs;
    int i_pos = (int)fpos;
    if (i_pos >= segs) i_pos = segs - 1;
    float fr = fpos - (float)i_pos;
    float pos[3] = {
        arc[i_pos][0] + (arc[i_pos+1][0] - arc[i_pos][0]) * fr,
        arc[i_pos][1] + (arc[i_pos+1][1] - arc[i_pos][1]) * fr,
        arc[i_pos][2] + (arc[i_pos+1][2] - arc[i_pos][2]) * fr,
    };
    float tp = ph - 0.25f;
    if (tp < 0.0f) tp = 0.0f;
    float ftp = tp * (float)segs;
    int i_tp = (int)ftp;
    if (i_tp >= segs) i_tp = segs - 1;
    float ftr = ftp - (float)i_tp;
    float trail[3] = {
        arc[i_tp][0] + (arc[i_tp+1][0] - arc[i_tp][0]) * ftr,
        arc[i_tp][1] + (arc[i_tp+1][1] - arc[i_tp][1]) * ftr,
        arc[i_tp][2] + (arc[i_tp+1][2] - arc[i_tp][2]) * ftr,
    };

    glLineWidth(3.5f);
    glBegin(GL_LINE_STRIP);
    tg_color4f(rgb[0], rgb[1], rgb[2], fminf(0.05f * as, 1.0f), alpha_mul);
    glVertex3f(trail[0], trail[1], trail[2]);
    for (int i = i_tp + 1; i <= i_pos; i++) {
        float u = (float)(i - i_tp) / (float)(i_pos - i_tp + 1);
        tg_color4f(rgb[0], rgb[1], rgb[2],
                   fminf((0.05f + (glow * 0.7f) * u) * as, 1.0f), alpha_mul);
        glVertex3fv(arc[i]);
    }
    tg_color4f(rgb[0], rgb[1], rgb[2], fminf(glow * 0.75f * as, 1.0f), alpha_mul);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glLineWidth(1.0f);

    glPointSize(8.0f);
    glBegin(GL_POINTS);
    tg_color4f(bright[0], bright[1], bright[2], glow, alpha_mul);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glPointSize(1.0f);
}

static void draw_rotate_guide(const SceneGuideSnapshot *snapshot,
                              const GLCmd *cmd, const float p_start[3],
                              float alpha_mul) {
    float angle_deg = cmd->args[0];
    while (angle_deg > 720.0f) angle_deg -= 360.0f;
    while (angle_deg < -720.0f) angle_deg += 360.0f;
    float ax = cmd->args[1], ay = cmd->args[2], az = cmd->args[3];
    float alen = sqrtf(ax*ax + ay*ay + az*az);
    if (alen < 1e-6f) return;
    if (fabsf(angle_deg) < 1e-4f) return;

    float rgb[3];
    xform_axis_color(ax, ay, az, rgb);
    float bright[3] = {
        rgb[0]*0.6f + 0.4f,
        rgb[1]*0.6f + 0.4f,
        rgb[2]*0.6f + 0.4f
    };

    ax /= alen;
    ay /= alen;
    az /= alen;

    float plen = sqrtf(p_start[0]*p_start[0] + p_start[1]*p_start[1] +
                       p_start[2]*p_start[2]);

    transform_guides_push_state();
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Rotation axis line through the origin */
    float axis_len = (plen > 0.5f ? plen : 0.5f) * 1.1f;
    float as = snapshot->alpha_scale;
    glLineWidth(2.0f);
    tg_color4f(rgb[0], rgb[1], rgb[2], fminf(0.55f * as, 1.0f), alpha_mul);
    glBegin(GL_LINES);
    glVertex3f(-ax*axis_len, -ay*axis_len, -az*axis_len);
    glVertex3f( ax*axis_len,  ay*axis_len,  az*axis_len);
    glEnd();

    /* Build either a swept arc (off-origin p_start) or a helix
     * (origin-anchored — no meaningful arc to sweep). */
    const int segs = TG_ARC_SEGS;
    float angle_rad = angle_deg * (float)(M_PI / 180.0);
    float arc[TG_ARC_SEGS + 1][3];
    if (plen >= 0.05f)
        build_rotate_arc(p_start, ax, ay, az, angle_rad, segs, arc);
    else
        build_rotate_helix(ax, ay, az, angle_rad, axis_len, segs, arc);

    /* Base arc line */
    glLineWidth(2.0f);
    tg_color4f(rgb[0], rgb[1], rgb[2], fminf(0.30f * as, 1.0f), alpha_mul);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= segs; i++) glVertex3fv(arc[i]);
    glEnd();

    /* Animated traveling pulse along the arc */
    draw_rotate_pulse(snapshot, arc, segs, rgb, bright, alpha_mul);

    /* Endpoint markers */
    glPointSize(6.0f);
    glBegin(GL_POINTS);
    tg_color4f(rgb[0], rgb[1], rgb[2], 0.7f, alpha_mul);
    glVertex3f(p_start[0], p_start[1], p_start[2]);
    glEnd();
    glPointSize(10.0f);
    glBegin(GL_POINTS);
    tg_color4f(bright[0], bright[1], bright[2], 0.95f, alpha_mul);
    glVertex3fv(arc[segs]);
    glEnd();
    glPointSize(1.0f);

    glDisable(GL_BLEND);
    transform_guides_pop_state();
}

/* True when the live input buffer text still matches the last committed
 * source form for the cursor row (ignoring outer whitespace and the
 * commit semicolon that editor input omits). A whitespace-only
 * difference doesn't change the parsed args but does represent an edit
 * in progress — guides hide until the user commits. The empty-input
 * case (no edit yet) is treated as matched.
 *
 * The function name reflects what is actually compared: input vs
 * committed text. It does NOT compare parsed args between input and
 * source — a future enhancement could (see plans/in-review/
 * src-scene-code-smell-audit.md #9). */
static int transform_input_matches_committed(const SceneGuideSnapshot *snapshot) {
    const char *source = snapshot->edit_line_committed_text
                         ? snapshot->edit_line_committed_text : "";
    while (*source && isspace((unsigned char)*source)) source++;

    int source_len = (int)strlen(source);
    while (source_len > 0 &&
           (source[source_len - 1] == ';' ||
            isspace((unsigned char)source[source_len - 1]))) {
        source_len--;
    }

    return ((source_len == snapshot->input_len &&
             strncmp(snapshot->input, source, (size_t)source_len) == 0) ||
            snapshot->input_len == 0);
}

/* Max in-scope affecting transforms we track for replay focus selection.
 * Bounded scratch; deeper stacks just cap (the nearest few are what matter). */
#define TG_MAX_INSCOPE_XFORMS 64

/* Collect the flat indices of the transforms in scope at `vertex_flat_idx`,
 * walking the flat program backward and honoring glPushMatrix/glPopMatrix/
 * glLoadIdentity. Writes newest-first, so out[0] is the nearest transform
 * before the vertex. Returns the count (capped at out_cap).
 *
 * This mirrors the flat affecting-transform walk in src/repl/autonormal.c
 * (repl_find_affecting_transforms_for_flat_vertex), but stays in the scene
 * module and yields flat indices (not deduped source lines) so the renderer
 * can anchor on a specific expansion. The scene module must not depend on
 * repl/core, hence the small re-implementation over the shared GLCmd model. */
static int collect_inscope_transform_flat_indices(const SceneGuideSnapshot *snapshot,
                                                  int vertex_flat_idx,
                                                  int *out, int out_cap) {
    const GLCmd *cmds = snapshot->flat_program.cmds;
    int count = 0;
    int popped_depth = 0;
    for (int i = vertex_flat_idx - 1; i >= 0 && count < out_cap; i--) {
        if (!cmds[i].valid) continue;
        CmdType t = cmds[i].type;
        if (t == CMD_POP_MATRIX) {
            popped_depth++;
        } else if (t == CMD_PUSH_MATRIX) {
            if (popped_depth > 0) popped_depth--;
        } else if (t == CMD_LOAD_IDENTITY) {
            if (popped_depth == 0) break;
        } else if (t == CMD_TRANSLATE3F || t == CMD_SCALEF || t == CMD_ROTATEF) {
            if (popped_depth == 0) out[count++] = i;
        }
    }
    return count;
}

/* Choose which flat transform to guide for the current replay vertex:
 *  (a) if the edit cursor is parked on a committed transform source line, the
 *      nearest in-scope flat expansion of that exact source line (so moving
 *      the cursor onto a transform during replay focuses it on the live
 *      vertex), else
 *  (b) the nearest in-scope affecting transform before the vertex.
 * Returns the flat index, or -1 when no transform affects the vertex. */
static int scene_replay_transform_focus_flat_idx(const SceneGuideSnapshot *snapshot,
                                                 int vertex_flat_idx) {
    int inscope[TG_MAX_INSCOPE_XFORMS];
    int n = collect_inscope_transform_flat_indices(snapshot, vertex_flat_idx,
                                                   inscope, TG_MAX_INSCOPE_XFORMS);
    if (n == 0)
        return -1;

    int cursor = snapshot->edit_line_idx;
    if (cursor >= 0 && cursor < snapshot->source_cmd_count &&
        snapshot->source_cmds[cursor].valid &&
        (snapshot->source_cmds[cursor].type == CMD_TRANSLATE3F ||
         snapshot->source_cmds[cursor].type == CMD_ROTATEF ||
         snapshot->source_cmds[cursor].type == CMD_SCALEF) &&
        transform_input_matches_committed(snapshot)) {
        /* inscope[] is newest-first, so the first src match is the closest
         * preceding expansion of the cursor's transform line. */
        for (int k = 0; k < n; k++)
            if (snapshot->flat_program.cmds[inscope[k]].src_cmd_idx == cursor)
                return inscope[k];
        /* Cursor is on a transform that doesn't reach this vertex — fall
         * through to the default nearest in-scope transform. */
    }
    return inscope[0];
}

int scene_transform_guides_prepare(const SceneGuideSnapshot *snapshot,
                                   SceneTransformGuidePlan *plan) {
    if (!snapshot || !plan)
        return 0;

    *plan = (SceneTransformGuidePlan){
        .active = 0,
        .consumed = 0,
        .cursor_flat_idx = -1,
        .after_flat_idx = -1,
    };

    if (!snapshot->show_guides)
        return 0;

    /* Replay path (req 6): instead of the edit cursor, pick the transform
     * shaping the vertex the replay step emitted and guide *it* exactly as the
     * live edit-mode guide would. The plan is shaped identically to the edit
     * path — cursor_flat_idx is the chosen transform's flat index, after_flat_idx
     * the first following flat command from a different source line (WORLD-mode
     * after-cursor anchor) — so the shared FRAME/WORLD render path draws it in
     * the transform's own frame, not on the vertex (which already sits at its
     * post-transform position). */
    if (snapshot->replaying) {
        int vtx = snapshot->replay_focus_vertex_flat_idx;
        int flat_count = snapshot->flat_program.cmd_count;
        if (vtx < 0 || vtx >= flat_count)
            return 0;
        const GLCmd *flat_cmds = snapshot->flat_program.cmds;
        const GLCmd *vcmd = &flat_cmds[vtx];
        if (!vcmd->valid || !repl_cmd_emits_vertex(vcmd->type))
            return 0;
        int xform = scene_replay_transform_focus_flat_idx(snapshot, vtx);
        if (xform < 0)
            return 0;
        plan->cursor_flat_idx = xform;
        int xform_src = flat_cmds[xform].src_cmd_idx;
        plan->after_flat_idx = flat_count;
        for (int i = xform + 1; i < flat_count; i++) {
            if (!flat_cmds[i].valid) continue;
            if (flat_cmds[i].src_cmd_idx == xform_src) continue;
            plan->after_flat_idx = i;
            break;
        }
        plan->active = 1;
        return 1;
    }

    int edit_line_idx = snapshot->edit_line_idx;
    if (edit_line_idx < 0 || edit_line_idx >= snapshot->source_cmd_count)
        return 0;

    const GLCmd *source_cmd = &snapshot->source_cmds[edit_line_idx];
    if (!source_cmd->valid)
        return 0;
    if (!transform_input_matches_committed(snapshot))
        return 0;
    if (!(source_cmd->type == CMD_TRANSLATE3F ||
          source_cmd->type == CMD_ROTATEF ||
          source_cmd->type == CMD_SCALEF)) {
        return 0;
    }

    const GLCmd *flat_cmds = snapshot->flat_program.cmds;
    int flat_cmd_count = snapshot->flat_program.cmd_count;
    /* First emitted flat command originating from the cursor row. */
    for (int flat_cmd_idx = 0; flat_cmd_idx < flat_cmd_count; flat_cmd_idx++) {
        if (!flat_cmds[flat_cmd_idx].valid)
            continue;
        if (flat_cmds[flat_cmd_idx].src_cmd_idx == edit_line_idx) {
            plan->cursor_flat_idx = flat_cmd_idx;
            break;
        }
    }
    if (plan->cursor_flat_idx < 0)
        return 0;

    /* First flat command after the cursor row, used for after-cursor anchoring
     * in world mode; if absent, anchor after the program tail. */
    for (int flat_cmd_idx = plan->cursor_flat_idx + 1;
         flat_cmd_idx < flat_cmd_count;
         flat_cmd_idx++) {
        if (!flat_cmds[flat_cmd_idx].valid) continue;
        if (flat_cmds[flat_cmd_idx].src_cmd_idx == edit_line_idx) continue;
        plan->after_flat_idx = flat_cmd_idx;
        break;
    }
    if (plan->after_flat_idx < 0)
        plan->after_flat_idx = flat_cmd_count;

    plan->active = 1;
    return 1;
}

void scene_transform_guides_render_if_due(const SceneGuideSnapshot *snapshot,
                                          SceneTransformGuidePlan *plan,
                                          int flat_cmd_idx,
                                          const float cam_view[16]) {
    if (!snapshot || !plan || !cam_view)
        return;
    if (!plan->active || plan->consumed)
        return;
    if (flat_cmd_idx != plan->cursor_flat_idx)
        return;

    const GLCmd *flat_cmds = snapshot->flat_program.cmds;
    const GLCmd *live_cmd = &flat_cmds[flat_cmd_idx];
    float guide_origin[3];

    /* Replay (req 6) shares the FRAME/WORLD anchoring below: the plan already
     * pointed cursor_flat_idx at the replay-chosen transform, so the guide
     * draws in that transform's own frame exactly as the live edit guide does,
     * rather than on the vertex (which already sits post-transform). */
    glPushMatrix();
    if (snapshot->xform_guide_mode == SCENE_XFORM_GUIDE_FRAME) {
        float frame[16];
        float guide_mv[16];
        compute_before_cursor_matrix(snapshot, plan->cursor_flat_idx, frame);
        mat4_mul_col_major(cam_view, frame, guide_mv);
        glLoadMatrixf(guide_mv);
        guide_origin[0] = guide_origin[1] = guide_origin[2] = 0.0f;
    } else {
        glLoadMatrixf(cam_view);
        compute_after_cursor_origin(snapshot, plan->after_flat_idx, guide_origin);
    }

    /* Pass 0: depth-test off, 0.4x alpha - a ghost the geometry can't hide
     * (rotation guides in particular often sit inside the rotating mesh).
     * Pass 1: depth-tested, full alpha, drawn over the ghost so the guide
     * still reads crisply where it isn't occluded. The draw helpers each
     * glPushAttrib(GL_ALL_ATTRIB_BITS), so they capture and restore
     * whatever depth state we set here; the outer push/pop guards the
     * caller's depth state across both passes. */
    glPushAttrib(GL_DEPTH_BUFFER_BIT);
    for (int pass = 0; pass < 2; pass++) {
        float alpha_mul;
        if (pass == 0) {
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_LINE_STIPPLE);
            glLineStipple(1, SCENE_OCCLUDED_GHOST_STIPPLE);
            alpha_mul = SCENE_OCCLUDED_GHOST_ALPHA;
        } else {
            glEnable(GL_DEPTH_TEST);
            alpha_mul = 1.0f;
        }

        if (live_cmd->type == CMD_TRANSLATE3F)
            draw_translate_guide(snapshot, live_cmd, guide_origin, alpha_mul);
        else if (live_cmd->type == CMD_SCALEF)
            draw_scale_guide(snapshot, live_cmd, guide_origin, alpha_mul);
        else
            draw_rotate_guide(snapshot, live_cmd, guide_origin, alpha_mul);

        glDisable(GL_LINE_STIPPLE);
    }
    glPopAttrib();

    glPopMatrix();
    plan->consumed = 1;
}
