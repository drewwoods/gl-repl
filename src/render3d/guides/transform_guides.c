/*
 * transform_guides.c - transform edit-guide planning/rendering.
 */
#include "transform_guides.h"
#include "repl/transform_utils.h"
#include "render3d/palette.h"
#include "render3d/occluded_ghost.h"

#include <ctype.h>  /* isspace */
#include <math.h>   /* sqrtf, fminf, fmodf, cosf, sinf, fabsf, M_PI */
#include <stdio.h>  /* snprintf */
#include <string.h> /* strncmp, strlen */

static void transform_guides_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void transform_guides_pop_state(void) {
    glPopAttrib();
}

/* Common unlit-overlay GL state for every guide draw helper. Cull is
 * disabled because the cone arrowheads are open fans the user's
 * glEnable(GL_CULL_FACE) would clip; smooth shading is required for the
 * cones' ring-shaded vertex colors to interpolate. Depth writes are off:
 * the guide is a blended overlay that must not self-occlude - coplanar
 * self-overlaps (a >360 deg rotate arc lapping itself, the dial/ticks/
 * sector sharing the rotation plane) would otherwise z-fight their own
 * first lap. Depth *testing* against scene geometry still applies in
 * the solid pass. The one exception is draw_cone_head's colorless depth
 * prepass: the cone is a solid, not a coplanar wash, and needs real
 * self-occlusion (cap behind side fan) to look right from every view angle.
 * All of it is restored by the paired transform_guides_pop_state(). */
static void transform_guides_begin_overlay_state(void) {
    transform_guides_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glShadeModel(GL_SMOOTH);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
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

/* Arrowheads are filled cones (a fan of TG_CONE_SLICES side triangles
 * plus a base cap). The solid silhouette stays readable at any viewing
 * angle and survives the depth-off ghost pass, where glLineStipple dashes
 * lines but leaves filled triangles whole. */
#define TG_CONE_SLICES   12

/* Shaft styling: a wide near-black contrast halo drawn under a bright
 * core line. The halo buys legibility over same-hue geometry and busy
 * backdrops; the core carries the guide color at full weight so the
 * guide reads instantly instead of riding on the traveling pulse. */
#define TG_SHAFT_CORE_W     2.5f
#define TG_SHAFT_HALO_W     5.5f
#define TG_SHAFT_CORE_ALPHA 0.85f
#define TG_HALO_ALPHA       0.40f
#define TG_HALO_R 0.02f
#define TG_HALO_G 0.02f
#define TG_HALO_B 0.05f

/* Guide-specific ghost-pass multiplier, deliberately brighter than the
 * shared RENDER3D_OCCLUDED_GHOST_ALPHA (0.4): a transform guide usually
 * sits fully inside the geometry it shapes (the mesh at the cursor is
 * exactly what the translate/rotate moves), so the occluded pass
 * carries more of the read than for other scene overlays. */
#define TG_GHOST_ALPHA_MUL 0.55f

/* A filled cone alpha-stacks its front and back side surfaces while depth is
 * disabled, unlike the stippled one-pixel shaft. Keep its hidden pass much
 * fainter than the rest of the guide; at 0.22 * 0.95, two overlapping side
 * layers composite to about 0.37. */
#define TG_CONE_GHOST_ALPHA_MUL 0.22f

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

/* Per-pass alpha multiplier. The render dispatcher draws each guide twice:
 * a depth-test-off ghost pass at TG_GHOST_ALPHA_MUL so geometry can't
 * fully hide the guide, then a depth-tested solid pass at 1.0 on top.
 * The value is threaded as a parameter through every draw helper so an
 * early-return or exception couldn't strand a ghost-alpha state and
 * cripple subsequent frames. */
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

/* Snap near-zero (incl. -0.0) to +0.0 so a label never reads "-0.00". */
static float tg_snap_zero(float v) {
    return (fabsf(v) < 0.005f) ? 0.0f : v;
}

/* Shared two-run "<name> <numbers>" label emitter for the translate and
 * scale guides, mirroring the normal glyph's " n" + "=(...)" pattern so a
 * transform guide cannot be misread as a normal readout. Depth-test
 * off so the text is always legible. Emits the glyphs directly (rather than
 * via render3d overlays) so this TU stays free of an overlays.o link
 * dependency - the isolated guides test links only the guide objects. */
static void draw_named_value_label(const Render3dGuideSnapshot *snapshot,
                                   const float pos[3],
                                   const char *name, const char *detail) {
    transform_guides_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.80f, 0.95f, 1.0f, 0.95f);
    render3d_guide_record_label(snapshot, pos, FONT_MONO, name,
                                  FONT_TINY, detail);
    glRasterPos3f(pos[0], pos[1], pos[2]);
    for (const char *c = name; *c; c++)
        glutBitmapCharacter(FONT_MONO, (unsigned char)*c);
    for (const char *c = detail; *c; c++)
        glutBitmapCharacter(FONT_TINY, (unsigned char)*c);
    transform_guides_pop_state();
}

/* Draw the translate endpoint's world position as a small text label at the
 * arrow tip. `tip_local` is the tip in the currently loaded guide frame (so
 * glRasterPos lands it on the arrowhead); `tip_world` is the same point in
 * world space - the position the model matrix leaves the origin at through
 * this line. Drawn once. */
static void draw_translate_endpoint_label(const Render3dGuideSnapshot *snapshot,
                                          const float tip_local[3],
                                          const float tip_world[3]) {
    char buf[48];
    snprintf(buf, sizeof(buf), " (%.2f, %.2f, %.2f)",
             tg_snap_zero(tip_world[0]), tg_snap_zero(tip_world[1]),
             tg_snap_zero(tip_world[2]));
    draw_named_value_label(snapshot, tip_local, " move", buf);
}

/* Scale-factor label at the guide's most prominent arrow tip. 0xD7 is the
 * ISO-8859-1 multiplication sign (the GLUT bitmap fonts carry full Latin-1,
 * same trick as the rotate label's 0xB0 degree sign), so the readout says
 * "multiply by these factors" rather than looking like a position. */
static void draw_scale_factor_label(const Render3dGuideSnapshot *snapshot,
                                    const float pos[3],
                                    float sx, float sy, float sz) {
    char buf[48];
    snprintf(buf, sizeof(buf), " \xD7(%.2f, %.2f, %.2f)",
             tg_snap_zero(sx), tg_snap_zero(sy), tg_snap_zero(sz));
    draw_named_value_label(snapshot, pos, " scale", buf);
}

/* Walk flat cmds strictly before cursor_flat_idx, applying only transform
 * cmds to a fresh identity matrix. Returns the full scene-world frame at the
 * cursor line, so Frame guide mode can account for prior rotations as well as
 * translations. */
static void compute_before_cursor_matrix(const Render3dGuideSnapshot *snapshot,
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
static void compute_after_cursor_origin(const Render3dGuideSnapshot *snapshot,
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

/* Solid shaft from a to b: a wide near-black contrast halo drawn first,
 * then the bright guide-colored core on top. The TG_SHAFT_* constants keep
 * the core legible over same-hue geometry. */
static void draw_shaft_segment(const Render3dGuideSnapshot *snapshot,
                               const float a[3], const float b[3],
                               const float rgb[3], float alpha_mul) {
    float as = snapshot->alpha_scale;
    glLineWidth(TG_SHAFT_HALO_W);
    tg_color4f(TG_HALO_R, TG_HALO_G, TG_HALO_B, TG_HALO_ALPHA, alpha_mul);
    glBegin(GL_LINES);
    glVertex3fv(a);
    glVertex3fv(b);
    glEnd();
    glLineWidth(TG_SHAFT_CORE_W);
    tg_color4f(rgb[0], rgb[1], rgb[2],
               fminf(TG_SHAFT_CORE_ALPHA * as, 1.0f), alpha_mul);
    glBegin(GL_LINES);
    glVertex3fv(a);
    glVertex3fv(b);
    glEnd();
    glLineWidth(1.0f);
}

/* Helper: filled cone arrowhead at `tip` pointing along `dir` (unit).
 * Side fan + base cap, with ring-angle vertex shading (bright on one
 * flank fading to dim on the other) faking a little dimensionality
 * without lighting, and a thin dark rim keeping the silhouette crisp
 * against same-hue geometry. Radius is head_len * TG_FIN_FRAC, keeping the
 * cone proportional to the shaft. */
static void draw_cone_head(const float tip[3], const float dir[3],
                           float head_len, const float rgb[3],
                           float alpha_mul) {
    float r[3], b[3];
    int ghost_pass = alpha_mul < 1.0f;
    float cone_alpha_mul = ghost_pass ? TG_CONE_GHOST_ALPHA_MUL : alpha_mul;
    make_arrow_basis(dir, r, b);
    float base[3] = {
        tip[0] - dir[0] * head_len,
        tip[1] - dir[1] * head_len,
        tip[2] - dir[2] * head_len
    };
    float rad = head_len * TG_FIN_FRAC;
    float ring[TG_CONE_SLICES + 1][3];
    float shade[TG_CONE_SLICES + 1];
    for (int k = 0; k <= TG_CONE_SLICES; k++) {
        float th = (float)(2.0 * M_PI) * (float)k / (float)TG_CONE_SLICES;
        float c = cosf(th), s = sinf(th);
        ring[k][0] = base[0] + (r[0] * c + b[0] * s) * rad;
        ring[k][1] = base[1] + (r[1] * c + b[1] * s) * rad;
        ring[k][2] = base[2] + (r[2] * c + b[2] * s) * rad;
        shade[k] = 0.62f + 0.38f * c;
    }

    /* Seed only the nearest solid-cone surface into depth before drawing any
     * color. Straight-arrow callers draw the cone before the shaft, so the
     * later shaft fragments can now pass in front of the head or fail behind
     * it instead of the cone always winning by draw order. LEQUAL lets the
     * color pass land on the depths written here. The ghost pass deliberately
     * skips this because it must remain visible through scene geometry. */
    if (!ghost_pass) {
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glBegin(GL_TRIANGLE_FAN);
        glVertex3fv(tip);
        for (int k = 0; k <= TG_CONE_SLICES; k++)
            glVertex3fv(ring[k]);
        glEnd();
        glBegin(GL_TRIANGLE_FAN);
        glVertex3fv(base);
        for (int k = TG_CONE_SLICES; k >= 0; k--)
            glVertex3fv(ring[k]);
        glEnd();
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
    }

    /* Side surface: fan from the (brightest) tip around the ring. */
    glBegin(GL_TRIANGLE_FAN);
    tg_color4f(rgb[0], rgb[1], rgb[2], 0.95f, cone_alpha_mul);
    glVertex3fv(tip);
    for (int k = 0; k <= TG_CONE_SLICES; k++) {
        tg_color4f(rgb[0] * shade[k], rgb[1] * shade[k], rgb[2] * shade[k],
                   0.95f, cone_alpha_mul);
        glVertex3fv(ring[k]);
    }
    glEnd();

    /* The solid pass keeps the darker base so the back reads as the back. The
     * depth-off ghost omits it: a third blended layer made hidden heads look
     * nearly opaque, especially when viewed along the arrow direction. */
    if (!ghost_pass) {
        glBegin(GL_TRIANGLE_FAN);
        tg_color4f(rgb[0] * 0.5f, rgb[1] * 0.5f, rgb[2] * 0.5f,
                   0.95f, cone_alpha_mul);
        glVertex3fv(base);
        for (int k = TG_CONE_SLICES; k >= 0; k--)
            glVertex3fv(ring[k]);
        glEnd();
    }

    /* Dark rim around the base circle. */
    glLineWidth(1.5f);
    tg_color4f(TG_HALO_R, TG_HALO_G, TG_HALO_B, 0.55f, cone_alpha_mul);
    glBegin(GL_LINE_LOOP);
    for (int k = 0; k < TG_CONE_SLICES; k++)
        glVertex3fv(ring[k]);
    glEnd();
    glLineWidth(1.0f);
}

/* Pulse shader for a straight segment in the axes-pulse style: a solid
 * halo+core base shaft, a bright dot traveling a -> b, and a short trail
 * behind the dot. */
static void draw_pulse_segment(const Render3dGuideSnapshot *snapshot,
                               const float a[3], const float b[3],
                               const float rgb[3], float alpha_mul) {
    float as = snapshot->alpha_scale;
    draw_shaft_segment(snapshot, a, b, rgb, alpha_mul);

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

    glPointSize(6.0f);
    glBegin(GL_POINTS);
    tg_color4f(rgb[0], rgb[1], rgb[2], fminf(glow * as, 1.0f), alpha_mul);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glPointSize(1.0f);
}

/* Arrow starting at p_after in the current local frame and extending by the
 * translate command's vector. Shaft is a solid halo+core line with an
 * axes-pulse-style traveling dot; a filled cone at the tip keeps the
 * direction unambiguous. Shaft color is (|tx|,|ty|,|tz|)/max mapped to RGB. */
static void draw_translate_guide(const Render3dGuideSnapshot *snapshot,
                                 const GLCmd *cmd, const float p_after[3],
                                 float alpha_mul) {
    float tx = cmd->args[0], ty = cmd->args[1], tz = cmd->args[2];
    float p0[3] = { p_after[0], p_after[1], p_after[2] };
    float p1[3] = { p0[0] + tx, p0[1] + ty, p0[2] + tz };

    float dlen = sqrtf(tx*tx + ty*ty + tz*tz);
    if (dlen < 1e-6f)
        return;
    float dir[3] = { tx / dlen, ty / dlen, tz / dlen };

    float head_len = clamp_head_len(dlen, TG_HEAD_LEN_FRAC,
                                    TG_HEAD_LEN_MIN, TG_HEAD_LEN_MAX);
    float base[3] = {
        p1[0] - dir[0] * head_len,
        p1[1] - dir[1] * head_len,
        p1[2] - dir[2] * head_len
    };

    float rgb[3];
    xform_axis_color(tx, ty, tz, rgb);
    float head_rgb[3] = {
        rgb[0] * 0.6f + 0.4f,
        rgb[1] * 0.6f + 0.4f,
        rgb[2] * 0.6f + 0.4f
    };

    transform_guides_begin_overlay_state();

    draw_cone_head(p1, dir, head_len, head_rgb, alpha_mul);
    draw_pulse_segment(snapshot, p0, base, rgb, alpha_mul);

    glPointSize(4.0f);
    glBegin(GL_POINTS);
    tg_color4f(rgb[0], rgb[1], rgb[2], 0.7f, alpha_mul);
    glVertex3f(p0[0], p0[1], p0[2]);
    glEnd();
    glPointSize(1.0f);

    transform_guides_pop_state();
}

/* Scale guide. Draws the identity reference (origin -> axis*1, gray) with a
 * bright tick at the 1.0 position on each axis, then a pulse arrow showing
 * only the distortion - from the 1.0 tick to the scaled tip. Scale of 1 on
 * an axis draws no pulse (identity); negative factors produce an arrow that
 * passes through origin. The non-origin branch is the World-mode variant
 * where p_start is the actual anchor point and the 1.0 reference is p_start
 * itself; at exact scale (1,1,1) only the marker is drawn. */
static void draw_scale_guide(const Render3dGuideSnapshot *snapshot,
                             const GLCmd *cmd, const float p_start[3],
                             float alpha_mul) {
    float sx = cmd->args[0], sy = cmd->args[1], sz = cmd->args[2];
    float p0[3] = { p_start[0], p_start[1], p_start[2] };
    float plen = sqrtf(p0[0]*p0[0] + p0[1]*p0[1] + p0[2]*p0[2]);

    transform_guides_begin_overlay_state();

    if (plen > 1e-4f) {
        float p1[3] = { p0[0]*sx, p0[1]*sy, p0[2]*sz };
        float delta[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
        float dlen = sqrtf(delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2]);

        float tick = 0.08f;
        glLineWidth(2.0f);
        render3d_clr_a(RENDER3D_CLR_GUIDE_REF_TICK, 0.9f * alpha_mul);
        glBegin(GL_LINES);
        glVertex3f(p0[0]-tick, p0[1], p0[2]); glVertex3f(p0[0]+tick, p0[1], p0[2]);
        glVertex3f(p0[0], p0[1]-tick, p0[2]); glVertex3f(p0[0], p0[1]+tick, p0[2]);
        glVertex3f(p0[0], p0[1], p0[2]-tick); glVertex3f(p0[0], p0[1], p0[2]+tick);
        glEnd();
        glPointSize(4.0f);
        glBegin(GL_POINTS);
        render3d_clr_a(RENDER3D_CLR_GUIDE_REF_POINT, 1.0f * alpha_mul);
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

            draw_cone_head(p1, dir, head_len, head_rgb, alpha_mul);
            draw_pulse_segment(snapshot, p0, base, rgb, alpha_mul);

            /* Factor readout at the arrow tip, once (solid pass only - the
             * label helper is depth-off, so a ghost-pass copy would just
             * double-print). */
            if (alpha_mul >= 1.0f)
                draw_scale_factor_label(snapshot, p1, sx, sy, sz);
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
        int label_axis = -1;   /* axis with the largest |f - 1| deviation */
        float label_dev = 0.0f;
        for (int a = 0; a < 3; a++) {
            float f = factors[a];
            const float *ax = axes[a];
            const float *pa = axes[perp_a[a]];
            const float *pb = axes[perp_b[a]];

            glLineWidth(1.5f);
            render3d_clr_a(RENDER3D_CLR_GUIDE_REF,
                        fminf(0.45f * snapshot->alpha_scale, 1.0f) * alpha_mul);
            glBegin(GL_LINES);
            glVertex3f(0.0f, 0.0f, 0.0f);
            glVertex3f(ax[0], ax[1], ax[2]);
            glEnd();

            glLineWidth(2.0f);
            render3d_clr_a(RENDER3D_CLR_GUIDE_REF_TICK, 0.9f * alpha_mul);
            glBegin(GL_LINES);
            glVertex3f(ax[0] - pa[0]*tick, ax[1] - pa[1]*tick, ax[2] - pa[2]*tick);
            glVertex3f(ax[0] + pa[0]*tick, ax[1] + pa[1]*tick, ax[2] + pa[2]*tick);
            glVertex3f(ax[0] - pb[0]*tick, ax[1] - pb[1]*tick, ax[2] - pb[2]*tick);
            glVertex3f(ax[0] + pb[0]*tick, ax[1] + pb[1]*tick, ax[2] + pb[2]*tick);
            glEnd();
            glPointSize(3.0f);
            glBegin(GL_POINTS);
            render3d_clr_a(RENDER3D_CLR_GUIDE_REF_POINT, 1.0f * alpha_mul);
            glVertex3f(ax[0], ax[1], ax[2]);
            glEnd();
            glPointSize(1.0f);

            if (fabsf(f - 1.0f) < 1e-4f) continue;
            if (fabsf(f - 1.0f) > label_dev) {
                label_dev = fabsf(f - 1.0f);
                label_axis = a;
            }

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

            float head_rgb[3] = {
                axis_rgb[a][0]*0.6f + 0.4f,
                axis_rgb[a][1]*0.6f + 0.4f,
                axis_rgb[a][2]*0.6f + 0.4f
            };
            draw_cone_head(tip, dir, head_len, head_rgb, alpha_mul);
            draw_pulse_segment(snapshot, from, base, axis_rgb[a], alpha_mul);
        }

        /* Factor readout at the most-deviating axis tip, once (solid pass
         * only, same double-print rationale as the world branch). No label
         * for an identity scale - there is no arrow to explain. */
        if (alpha_mul >= 1.0f && label_axis >= 0) {
            float lp[3] = {
                axes[label_axis][0] * factors[label_axis],
                axes[label_axis][1] * factors[label_axis],
                axes[label_axis][2] * factors[label_axis]
            };
            draw_scale_factor_label(snapshot, lp, sx, sy, sz);
        }
    }

    glDisable(GL_BLEND);
    transform_guides_pop_state();
}

/* --- draw_rotate_guide phases ---
 *
 * One geometry generator (build_rotate_arc, Rodrigues rotation of a
 * start point) + one pulse animator. When the anchor point sits on the
 * rotation axis (no orbit circle to sweep - notably the default FRAME
 * guide mode, whose anchor is the local origin), the guide substitutes
 * a synthetic start point on the axis-perpendicular plane at a
 * gizmo-sized radius, so the same arc/dial/cone/label path renders a
 * Blender-style planar rotation dial instead of degenerating. */

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

/* Perpendicular {u, v} basis for a unit rotation axis (ax, ay, az):
 * u is computed from a world-up helper and normalized, then v = a x u.
 * Places the synthetic dial start point for on-axis anchors. */
static void rotate_axis_basis(float ax, float ay, float az,
                              float u[3], float v[3]) {
    float helper[3] = {1.0f, 0.0f, 0.0f};
    if (fabsf(ax) > 0.9f) { helper[0] = 0.0f; helper[1] = 1.0f; }
    u[0] = helper[1]*az - helper[2]*ay;
    u[1] = helper[2]*ax - helper[0]*az;
    u[2] = helper[0]*ay - helper[1]*ax;
    float ul = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (ul < 1e-6f) ul = 1.0f;
    u[0] /= ul; u[1] /= ul; u[2] /= ul;
    v[0] = ay*u[2] - az*u[1];
    v[1] = az*u[0] - ax*u[2];
    v[2] = ax*u[1] - ay*u[0];
}

/* Dial ring radius for on-axis anchors, as a fraction of the drawn
 * axis length. Sized to ring typical unit-scale geometry rather than
 * hide inside it. */
#define TG_DIAL_RADIUS_FRAC 0.85f

/* Animate a traveling glow dot + trail along arc[0..segs]. Same
 * shape as draw_pulse_segment's straight-line version, but the
 * sample positions are interpolated along the arc segments. */
static void draw_rotate_pulse(const Render3dGuideSnapshot *snapshot,
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

    glPointSize(5.0f);
    glBegin(GL_POINTS);
    tg_color4f(bright[0], bright[1], bright[2], glow, alpha_mul);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glPointSize(1.0f);
}

/* Swept-angle text label near the arc midpoint. Mirrors
 * draw_translate_endpoint_label: depth-test off and drawn once (the
 * caller gates it to the solid pass) so the number is always legible.
 * 0xB0 is the ISO-8859-1 degree sign - the GLUT bitmap fonts carry the
 * full 8-bit Latin-1 set. */
static void draw_rotate_angle_label(const Render3dGuideSnapshot *snapshot,
                                    const float pos[3], float angle_deg) {
    char buf[24];
    snprintf(buf, sizeof(buf), " %+.0f\xB0", (double)angle_deg);

    transform_guides_push_state();
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.80f, 0.95f, 1.0f, 0.95f);
    render3d_guide_record_label(snapshot, pos, FONT_SMALL, buf, NULL, NULL);
    glRasterPos3f(pos[0], pos[1], pos[2]);
    for (const char *c = buf; *c; c++)
        glutBitmapCharacter(FONT_SMALL, (unsigned char)*c);
    transform_guides_pop_state();
}

static void draw_rotate_guide(const Render3dGuideSnapshot *snapshot,
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

    transform_guides_begin_overlay_state();

    /* Rotation axis line through the origin */
    float axis_len = (plen > 1.0f ? plen : 1.0f) * 1.1f;
    float as = snapshot->alpha_scale;
    glLineWidth(2.0f);
    tg_color4f(rgb[0], rgb[1], rgb[2], fminf(0.55f * as, 1.0f), alpha_mul);
    glBegin(GL_LINES);
    glVertex3f(-ax*axis_len, -ay*axis_len, -az*axis_len);
    glVertex3f( ax*axis_len,  ay*axis_len,  az*axis_len);
    glEnd();

    /* Effective sweep-start point. When the anchor sits (near) the
     * rotation axis its orbit circle degenerates - notably the default
     * FRAME guide mode, whose anchor is the local origin - so substitute
     * a point on the axis-perpendicular plane through the anchor at a
     * gizmo-sized radius. Everything downstream (arc, dial, cone, label)
     * then renders one uniform design that remains visible beside unit-scale
     * geometry. */
    float axial0 = p_start[0]*ax + p_start[1]*ay + p_start[2]*az;
    float radial0[3] = {
        p_start[0] - ax*axial0,
        p_start[1] - ay*axial0,
        p_start[2] - az*axial0
    };
    float rlen0 = sqrtf(radial0[0]*radial0[0] + radial0[1]*radial0[1] +
                        radial0[2]*radial0[2]);
    int on_axis = (rlen0 < 0.05f);
    float p_eff[3] = { p_start[0], p_start[1], p_start[2] };
    if (on_axis) {
        float u[3], v[3];
        rotate_axis_basis(ax, ay, az, u, v);
        float dial_r = axis_len * TG_DIAL_RADIUS_FRAC;
        p_eff[0] = ax*axial0 + u[0]*dial_r;
        p_eff[1] = ay*axial0 + u[1]*dial_r;
        p_eff[2] = az*axial0 + u[2]*dial_r;
    }

    const int segs = TG_ARC_SEGS;
    float angle_rad = angle_deg * (float)(M_PI / 180.0);
    float arc[TG_ARC_SEGS + 1][3];
    build_rotate_arc(p_eff, ax, ay, az, angle_rad, segs, arc);

    /* Rotation-dial context: a faint full ring showing the circle the
     * sweep rides, and a translucent sector fan filling the swept angle
     * from the ring's center like a protractor. Together they make the
     * guide read as *rotation* at a glance, where the bare arc could
     * pass for a curved translate. Drawn before the arc stroke so the
     * solid arc sits on top. */
    {
        float circle[TG_ARC_SEGS + 1][3];
        build_rotate_arc(p_eff, ax, ay, az, (float)(2.0 * M_PI), segs,
                         circle);
        float center[3] = { ax*axial0, ay*axial0, az*axial0 };

        /* Sector sweep capped at one full turn: past 360 deg the fan would
         * lap itself and double-blend the overlapped wedge. The arc,
         * pulse, and label still carry the full wrapped sweep. */
        const float (*sector_pts)[3] = arc;
        float sector[TG_ARC_SEGS + 1][3];
        const float tau = (float)(2.0 * M_PI);
        if (fabsf(angle_rad) > tau) {
            build_rotate_arc(p_eff, ax, ay, az,
                             (angle_rad > 0.0f ? tau : -tau), segs, sector);
            sector_pts = sector;
        }

        glLineWidth(1.5f);
        tg_color4f(rgb[0], rgb[1], rgb[2], fminf(0.32f * as, 1.0f), alpha_mul);
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i <= segs; i++) glVertex3fv(circle[i]);
        glEnd();
        glLineWidth(1.0f);

        tg_color4f(rgb[0], rgb[1], rgb[2], fminf(0.18f * as, 1.0f), alpha_mul);
        glBegin(GL_TRIANGLE_FAN);
        glVertex3fv(center);
        for (int i = 0; i <= segs; i++) glVertex3fv(sector_pts[i]);
        glEnd();

        /* Radial edges bounding the sector. */
        glLineWidth(1.5f);
        tg_color4f(rgb[0], rgb[1], rgb[2], fminf(0.45f * as, 1.0f), alpha_mul);
        glBegin(GL_LINES);
        glVertex3fv(center); glVertex3fv(sector_pts[0]);
        glVertex3fv(center); glVertex3fv(sector_pts[segs]);
        glEnd();
        glLineWidth(1.0f);
    }

    /* Base arc line: dark contrast halo under a solid core (the arc's
     * curved twin of draw_shaft_segment). */
    glLineWidth(TG_SHAFT_HALO_W);
    tg_color4f(TG_HALO_R, TG_HALO_G, TG_HALO_B, TG_HALO_ALPHA, alpha_mul);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= segs; i++) glVertex3fv(arc[i]);
    glEnd();
    glLineWidth(TG_SHAFT_CORE_W);
    tg_color4f(rgb[0], rgb[1], rgb[2],
               fminf(TG_SHAFT_CORE_ALPHA * as, 1.0f), alpha_mul);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= segs; i++) glVertex3fv(arc[i]);
    glEnd();
    glLineWidth(1.0f);

    /* Animated traveling pulse along the arc */
    draw_rotate_pulse(snapshot, arc, segs, rgb, bright, alpha_mul);

    /* Sweep-direction cone at the arc end, along the end tangent - the
     * arc alone doesn't say which way the rotation goes. Sized from the
     * approximate arc length so short arcs get a proportional nubbin. */
    float tan_dir[3] = {
        arc[segs][0] - arc[segs - 1][0],
        arc[segs][1] - arc[segs - 1][1],
        arc[segs][2] - arc[segs - 1][2]
    };
    float tan_len = sqrtf(tan_dir[0]*tan_dir[0] + tan_dir[1]*tan_dir[1] +
                          tan_dir[2]*tan_dir[2]);
    if (tan_len > 1e-6f) {
        tan_dir[0] /= tan_len; tan_dir[1] /= tan_len; tan_dir[2] /= tan_len;
        float arc_len = tan_len * (float)segs; /* uniform-angle sampling */
        float head_len = clamp_head_len(arc_len, TG_HEAD_LEN_FRAC,
                                        TG_HEAD_LEN_MIN, TG_HEAD_LEN_MAX);
        draw_cone_head(arc[segs], tan_dir, head_len, bright, alpha_mul);
    }

    /* Start marker - only for a real off-axis anchor; the synthetic
     * dial start point is not a position the user authored. */
    if (!on_axis) {
        glPointSize(4.0f);
        glBegin(GL_POINTS);
        tg_color4f(rgb[0], rgb[1], rgb[2], 0.7f, alpha_mul);
        glVertex3f(p_start[0], p_start[1], p_start[2]);
        glEnd();
        glPointSize(1.0f);
    }

    /* Swept-angle label at the arc midpoint, once (solid pass only -
     * the label helper is depth-off, so a ghost-pass copy would just
     * double-print). Pushed radially outward from the rotation axis so
     * the text clears the arc stroke and sector fill. */
    if (alpha_mul >= 1.0f) {
        int mid = segs / 2;
        float m_ax = arc[mid][0]*ax + arc[mid][1]*ay + arc[mid][2]*az;
        float radial[3] = {
            arc[mid][0] - ax*m_ax,
            arc[mid][1] - ay*m_ax,
            arc[mid][2] - az*m_ax
        };
        float rlen = sqrtf(radial[0]*radial[0] + radial[1]*radial[1] +
                           radial[2]*radial[2]);
        float label_pos[3] = { arc[mid][0], arc[mid][1], arc[mid][2] };
        if (rlen > 1e-4f) {
            float push = (0.08f + rlen * 0.10f) / rlen;
            label_pos[0] += radial[0] * push;
            label_pos[1] += radial[1] * push;
            label_pos[2] += radial[2] * push;
        }
        draw_rotate_angle_label(snapshot, label_pos, angle_deg);
    }

    transform_guides_pop_state();
}

/* True when the live input buffer text still matches the last committed
 * source form for the cursor row (ignoring outer whitespace and the
 * commit semicolon that editor input omits). A whitespace-only
 * difference doesn't change the parsed args but does represent an edit
 * in progress - guides hide until the user commits. The empty-input
 * case (no edit yet) is treated as matched.
 *
 * The function name reflects what is actually compared: input vs
 * committed text. It does NOT compare parsed args between input and source. */
static int transform_input_matches_committed(const Render3dGuideSnapshot *snapshot) {
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

/* Identify a live transform command from the input-buffer prefix, mirroring
 * geometry_guides.c's input_is_vertex_kind. Returns the CmdType
 * (CMD_TRANSLATE3F / CMD_SCALEF / CMD_ROTATEF) once the user has typed the
 * opening paren, or -1 otherwise. The args themselves are pre-evaluated by
 * the controller into snapshot->xform_args (the scene module never evals). */
static int transform_input_kind(const char *input, int input_len) {
    if (!input) return -1;
    if (input_len >= 13 && strncmp(input, "glTranslatef(", 13) == 0)
        return CMD_TRANSLATE3F;
    if (input_len >= 9 && strncmp(input, "glScalef(", 9) == 0)
        return CMD_SCALEF;
    if (input_len >= 10 && strncmp(input, "glRotatef(", 10) == 0)
        return CMD_ROTATEF;
    return -1;
}

/* Compose a GLCmd from the live cursor args for `kind`, filling slots the
 * user hasn't typed yet with the transform identity: 1 for scale, 0 for
 * translate/rotate (per the requested "assume defaults while typing"
 * behavior). Only type + args are populated - the draw helpers read
 * nothing else. */
static GLCmd transform_live_cmd(const Render3dGuideSnapshot *snapshot, int kind) {
    GLCmd c;
    memset(&c, 0, sizeof c);
    c.type = (CmdType)kind;
    c.valid = 1;
    float dflt = (kind == CMD_SCALEF) ? 1.0f : 0.0f;
    int n = (kind == CMD_ROTATEF) ? 4 : 3;
    c.num_args = n;
    for (int i = 0; i < n; i++)
        c.args[i] = snapshot->xform_filled[i] ? snapshot->xform_args[i] : dflt;
    return c;
}

/* Max in-scope affecting transforms we track for replay focus selection.
 * Bounded scratch; deeper stacks just cap (the nearest few are what matter). */
#define TG_MAX_INSCOPE_XFORMS 64

/* Collect the flat indices of the transforms in scope at `anchor_flat_idx`,
 * walking the flat program backward and honoring glPushMatrix/glPopMatrix/
 * glLoadIdentity. Writes newest-first, so out[0] is the nearest transform
 * before the anchor draw. Returns the count (capped at out_cap).
 *
 * The scope accounting lives in transform_utils.h's TransformScopeScan
 * (shared with src/repl/autonormal.c's flat affecting-transform
 * resolver); this collector yields flat indices (not deduped source
 * lines) so the renderer can anchor on a specific expansion. */
static int collect_inscope_transform_flat_indices(const Render3dGuideSnapshot *snapshot,
                                                  int anchor_flat_idx,
                                                  int *out, int out_cap) {
    TransformScopeScan scan;
    int count = 0;
    int idx;

    transform_scope_scan_init(&scan, snapshot->flat_program.cmds,
                              anchor_flat_idx);
    while (count < out_cap && (idx = transform_scope_scan_next(&scan)) >= 0)
        out[count++] = idx;
    return count;
}

/* Choose which flat transform to guide for the current replay draw (a vertex
 * or a glutSolid* anchor):
 *  (a) if the edit cursor is parked on a committed transform source line, the
 *      nearest in-scope flat expansion of that exact source line (so moving
 *      the cursor onto a transform during replay focuses it on the live
 *      draw), else
 *  (b) the nearest in-scope affecting transform before the anchor.
 * Returns the flat index, or -1 when no transform affects the anchor. */
static int render3d_replay_transform_focus_flat_idx(const Render3dGuideSnapshot *snapshot,
                                                 int anchor_flat_idx) {
    int inscope[TG_MAX_INSCOPE_XFORMS];
    int n = collect_inscope_transform_flat_indices(snapshot, anchor_flat_idx,
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
        /* Cursor is on a transform that doesn't reach this vertex - fall
         * through to the default nearest in-scope transform. */
    }
    return inscope[0];
}

int render3d_transform_guides_prepare(const Render3dGuideSnapshot *snapshot,
                                   Render3dTransformGuidePlan *plan) {
    if (!snapshot || !plan)
        return 0;

    *plan = (Render3dTransformGuidePlan){
        .active = 0,
        .consumed = 0,
        .cursor_flat_idx = -1,
        .after_flat_idx = -1,
    };

    if (!snapshot->show_guides)
        return 0;

    /* Replay path: instead of the edit cursor, pick the transform
     * shaping the draw the replay step emitted (a glVertex/gluVertex or a
     * glutSolid*) and guide *it* exactly as the live edit-mode guide would. The
     * plan is shaped identically to the edit path - cursor_flat_idx is the
     * chosen transform's flat index, after_flat_idx the first following flat
     * command from a different source line (WORLD-mode after-cursor anchor) - so
     * the shared FRAME/WORLD render path draws it in the transform's own frame,
     * not on the draw (which already sits at its post-transform position). */
    if (snapshot->replaying) {
        int anchor = snapshot->replay_focus_anchor_flat_idx;
        int flat_count = snapshot->flat_program.cmd_count;
        if (anchor < 0 || anchor >= flat_count)
            return 0;
        const GLCmd *flat_cmds = snapshot->flat_program.cmds;
        const GLCmd *acmd = &flat_cmds[anchor];
        /* The replay anchor is whatever draw the step landed on - a glVertex /
         * gluVertex or a glutSolid* (repl_cmd_consumes_current_color). The guide
         * draws in the transform's own frame (not on the anchor's position), so
         * a glut solid, which carries no vertex position, works the same way. */
        if (!acmd->valid || !repl_cmd_consumes_current_color(acmd->type))
            return 0;
        int xform = render3d_replay_transform_focus_flat_idx(snapshot, anchor);
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
    if (edit_line_idx < 0)
        return 0;
    /* A committed-but-invalid cursor line carries no guide (it failed to
     * parse). Brand-new lines past the source tail have no source cmd yet
     * and skip this check. */
    if (edit_line_idx < snapshot->source_cmd_count &&
        !snapshot->source_cmds[edit_line_idx].valid)
        return 0;

    /* Two ways to activate the edit-mode guide:
     *  (live) the input buffer is a transform being typed (glTranslatef( /
     *         glScalef( / glRotatef() - render live with the cursor args and
     *         identity defaults for untyped slots, even before commit.
     *  (parked) no live transform input, but the cursor sits on a committed
     *         transform source line whose buffer matches it - the historic
     *         behavior, rendered from the committed flat args. */
    int live_kind = transform_input_kind(snapshot->input, snapshot->input_len);
    if (live_kind < 0) {
        if (edit_line_idx >= snapshot->source_cmd_count)
            return 0;
        const GLCmd *source_cmd = &snapshot->source_cmds[edit_line_idx];
        if (!transform_input_matches_committed(snapshot))
            return 0;
        if (!(source_cmd->type == CMD_TRANSLATE3F ||
              source_cmd->type == CMD_ROTATEF ||
              source_cmd->type == CMD_SCALEF)) {
            return 0;
        }
    }

    const GLCmd *flat_cmds = snapshot->flat_program.cmds;
    int flat_cmd_count = snapshot->flat_program.cmd_count;
    /* The flat command originating from the cursor row: its first expansion,
     * or the last one when the snapshot asks for the last unrolled copy. */
    for (int flat_cmd_idx = 0; flat_cmd_idx < flat_cmd_count; flat_cmd_idx++) {
        if (!flat_cmds[flat_cmd_idx].valid)
            continue;
        if (flat_cmds[flat_cmd_idx].src_cmd_idx == edit_line_idx) {
            plan->cursor_flat_idx = flat_cmd_idx;
            if (!snapshot->prefer_last_instance)
                break;
        }
    }
    if (plan->cursor_flat_idx < 0) {
        /* No flat expansion for the cursor line. For a committed/parked guide
         * that means nothing to anchor on; bail. For a live transform being
         * typed on a brand-new (not-yet-flattened) line, anchor at the
         * insertion point - the first flat command at/after the cursor's
         * source position (or the program tail). The shared FRAME/WORLD math
         * walks [0, idx) / [idx, count), so the guide lands in the frame the
         * new line will occupy; it renders via the on_end flush when the
         * anchor is the tail (no per-cmd walk index matches it). */
        if (live_kind < 0)
            return 0;
        int ins = flat_cmd_count;
        for (int i = 0; i < flat_cmd_count; i++) {
            if (!flat_cmds[i].valid) continue;
            if (flat_cmds[i].src_cmd_idx >= edit_line_idx) { ins = i; break; }
        }
        plan->cursor_flat_idx = ins;
        plan->after_flat_idx = ins;
        plan->active = 1;
        return 1;
    }

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

void render3d_transform_guides_render_if_due(const Render3dGuideSnapshot *snapshot,
                                          Render3dTransformGuidePlan *plan,
                                          int flat_cmd_idx,
                                          const float cam_view[16]) {
    if (!snapshot || !plan || !cam_view)
        return;
    if (!plan->active || plan->consumed)
        return;
    if (flat_cmd_idx != plan->cursor_flat_idx)
        return;

    /* Choose the command to draw. While the user is actively typing a
     * transform whose buffer diverges from the committed source (or there is
     * no committed source - a brand-new line), draw from the live cursor args
     * with identity defaults for untyped slots. When the buffer matches the
     * committed line (incl. the empty no-edit-yet case) we draw the committed
     * flat args exactly as before. Never during replay (it anchors on a
     * replay-chosen flat transform, not the input buffer). */
    int live_kind = transform_input_kind(snapshot->input, snapshot->input_len);
    int use_live = !snapshot->replaying && live_kind >= 0 &&
                   !transform_input_matches_committed(snapshot);
    GLCmd live_synth;
    const GLCmd *live_cmd;
    if (use_live) {
        live_synth = transform_live_cmd(snapshot, live_kind);
        live_cmd = &live_synth;
    } else {
        live_cmd = &snapshot->flat_program.cmds[flat_cmd_idx];
    }
    float guide_origin[3];
    /* Before-cursor world frame, kept so the translate endpoint label can map
     * the local arrow tip into world space. Only filled (have_frame) in FRAME
     * mode; in WORLD mode the guide is already drawn in world coordinates. */
    float frame[16];
    int have_frame = 0;

    /* Replay shares the FRAME/WORLD anchoring below: the plan already
     * pointed cursor_flat_idx at the replay-chosen transform, so the guide
     * draws in that transform's own frame exactly as the live edit guide does,
     * rather than on the vertex (which already sits post-transform). */
    glPushMatrix();
    if (snapshot->xform_guide_mode == RENDER3D_XFORM_GUIDE_FRAME) {
        float guide_mv[16];
        compute_before_cursor_matrix(snapshot, plan->cursor_flat_idx, frame);
        have_frame = 1;
        mat4_mul_col_major(cam_view, frame, guide_mv);
        glLoadMatrixf(guide_mv);
        guide_origin[0] = guide_origin[1] = guide_origin[2] = 0.0f;
    } else {
        glLoadMatrixf(cam_view);
        compute_after_cursor_origin(snapshot, plan->after_flat_idx, guide_origin);
    }

    /* Pass 0: depth-test off, TG_GHOST_ALPHA_MUL alpha - a ghost the
     * geometry can't hide (rotation guides in particular often sit inside
     * the rotating mesh). Lines take the shared occluded-ghost stipple;
     * the filled cone heads render whole (stipple only dashes lines), so
     * an occluded arrowhead still reads as a solid translucent shape.
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
            glLineStipple(1, RENDER3D_OCCLUDED_GHOST_STIPPLE);
            alpha_mul = TG_GHOST_ALPHA_MUL;
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

    /* Endpoint world-position label for a translate: the position the model
     * matrix leaves the origin at through this line. Drawn once, over both
     * passes, at the arrow tip. It lives here (not in draw_translate_guide)
     * because mapping the tip into world space needs the before-cursor frame;
     * scale/rotate readouts (factors / swept angle) are frame-independent and
     * label themselves inside their draw helpers. */
    if (live_cmd->type == CMD_TRANSLATE3F) {
        float tip_local[3] = {
            guide_origin[0] + live_cmd->args[0],
            guide_origin[1] + live_cmd->args[1],
            guide_origin[2] + live_cmd->args[2]
        };
        float tip_world[3];
        if (have_frame)
            mat4_point_col_major(frame, tip_local, tip_world);
        else /* WORLD mode: guide already drawn in world coordinates */
            tip_world[0] = tip_local[0],
            tip_world[1] = tip_local[1],
            tip_world[2] = tip_local[2];
        draw_translate_endpoint_label(snapshot, tip_local, tip_world);
    }

    glPopMatrix();
    plan->consumed = 1;
}
