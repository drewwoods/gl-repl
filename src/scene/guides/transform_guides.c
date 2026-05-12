/*
 * scene_transform_guides.c - transform edit-guide planning/rendering.
 */
#include "transform_guides.h"
#include "transform_utils.h"

static void transform_guides_push_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
}

static void transform_guides_pop_state(void) {
    glPopAttrib();
}

/* Cmds that emit geometry - hitting one of these means "something just got
 * drawn with the current modelview", so further transforms shouldn't factor
 * into the cursor-line's guide. */
static int is_geometry_emit_cmd(CmdType type) {
    return (type == CMD_BEGIN ||
            type == CMD_GLUT_TORUS || type == CMD_GLUT_CUBE ||
            type == CMD_GLUT_SPHERE || type == CMD_GLUT_TEAPOT ||
            type == CMD_GLUT_CONE ||
            type == CMD_TESS_BEGIN_POLYGON);
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
        if (is_geometry_emit_cmd(flat_cmds[i].type)) break;
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
                               const float rgb[3]) {
    float as = snapshot->alpha_scale;
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glColor4f(rgb[0], rgb[1], rgb[2], fminf(0.30f * as, 1.0f));
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
    glColor4f(rgb[0], rgb[1], rgb[2], fminf(0.05f * as, 1.0f));
    glVertex3f(trail[0], trail[1], trail[2]);
    glColor4f(rgb[0], rgb[1], rgb[2], fminf(glow * 0.75f * as, 1.0f));
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glLineWidth(1.0f);

    glPointSize(8.0f);
    glBegin(GL_POINTS);
    glColor4f(rgb[0], rgb[1], rgb[2], fminf(glow * as, 1.0f));
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glPointSize(1.0f);
}

/* Arrow starting at p_after in the current local frame and extending by the
 * translate command's vector. Shaft is an axes-pulse-style traveling dot
 * over a dim base line; the solid 4-fin arrowhead at the tip keeps the
 * direction unambiguous. Shaft color is (|tx|,|ty|,|tz|)/max mapped to RGB. */
static void draw_translate_guide(const SceneGuideSnapshot *snapshot,
                                 const GLCmd *cmd, const float p_after[3]) {
    float tx = cmd->args[0], ty = cmd->args[1], tz = cmd->args[2];
    float p0[3] = { p_after[0], p_after[1], p_after[2] };
    float p1[3] = { p0[0] + tx, p0[1] + ty, p0[2] + tz };

    float dlen = sqrtf(tx*tx + ty*ty + tz*tz);
    if (dlen < 1e-6f)
        return;
    float dx = tx / dlen, dy = ty / dlen, dz = tz / dlen;

    float ux, uy, uz;
    if (fabsf(dy) < 0.9f) { ux = 0.0f; uy = 1.0f; uz = 0.0f; }
    else                  { ux = 1.0f; uy = 0.0f; uz = 0.0f; }
    float rx = dy*uz - dz*uy;
    float ry = dz*ux - dx*uz;
    float rz = dx*uy - dy*ux;
    float rlen = sqrtf(rx*rx + ry*ry + rz*rz);
    if (rlen > 1e-8f) { rx /= rlen; ry /= rlen; rz /= rlen; }
    float bx = ry*dz - rz*dy;
    float by = rz*dx - rx*dz;
    float bz = rx*dy - ry*dx;

    float head_len = dlen * 0.22f;
    if (head_len > 0.25f) head_len = 0.25f;
    if (head_len < 0.06f) head_len = (dlen < 0.06f ? dlen * 0.5f : 0.06f);
    float fin = head_len * 0.45f;
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

    draw_pulse_segment(snapshot, p0, base, rgb);

    glLineWidth(3.0f);
    glColor4f(head_rgb[0], head_rgb[1], head_rgb[2], 0.95f);
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
    glColor4f(rgb[0], rgb[1], rgb[2], 0.7f);
    glVertex3f(p0[0], p0[1], p0[2]);
    glEnd();
    glPointSize(9.0f);
    glBegin(GL_POINTS);
    glColor4f(head_rgb[0], head_rgb[1], head_rgb[2], 1.0f);
    glVertex3f(p1[0], p1[1], p1[2]);
    glEnd();
    glPointSize(1.0f);

    glDisable(GL_BLEND);
    if (snapshot->user_lighting_enabled) glEnable(GL_LIGHTING);
    transform_guides_pop_state();
}

/* Arc from p_start swept by glRotatef(angle, ax,ay,az) about local origin. */
/* Helper: small 4-fin arrowhead from `tip` pointing along `dir` (unit). */
static void draw_arrow_head(const float tip[3], const float dir[3], float head_len) {
    float ux, uy, uz;
    if (fabsf(dir[1]) < 0.9f) { ux = 0.0f; uy = 1.0f; uz = 0.0f; }
    else                      { ux = 1.0f; uy = 0.0f; uz = 0.0f; }
    float rx = dir[1]*uz - dir[2]*uy;
    float ry = dir[2]*ux - dir[0]*uz;
    float rz = dir[0]*uy - dir[1]*ux;
    float rlen = sqrtf(rx*rx + ry*ry + rz*rz);
    if (rlen > 1e-8f) { rx /= rlen; ry /= rlen; rz /= rlen; }
    float bx = ry*dir[2] - rz*dir[1];
    float by = rz*dir[0] - rx*dir[2];
    float bz = rx*dir[1] - ry*dir[0];
    float base[3] = {
        tip[0] - dir[0] * head_len,
        tip[1] - dir[1] * head_len,
        tip[2] - dir[2] * head_len
    };
    float fin = head_len * 0.45f;
    glBegin(GL_LINES);
    for (int k = 0; k < 4; k++) {
        float sx = (k == 0 ?  rx : k == 1 ? -rx : k == 2 ?  bx : -bx);
        float sy = (k == 0 ?  ry : k == 1 ? -ry : k == 2 ?  by : -by);
        float sz = (k == 0 ?  rz : k == 1 ? -rz : k == 2 ?  bz : -bz);
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
                             const GLCmd *cmd, const float p_start[3]) {
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
        glColor4f(0.9f, 0.9f, 0.9f, 0.9f);
        glBegin(GL_LINES);
        glVertex3f(p0[0]-tick, p0[1], p0[2]); glVertex3f(p0[0]+tick, p0[1], p0[2]);
        glVertex3f(p0[0], p0[1]-tick, p0[2]); glVertex3f(p0[0], p0[1]+tick, p0[2]);
        glVertex3f(p0[0], p0[1], p0[2]-tick); glVertex3f(p0[0], p0[1], p0[2]+tick);
        glEnd();
        glPointSize(6.0f);
        glBegin(GL_POINTS);
        glColor4f(0.95f, 0.95f, 0.95f, 1.0f);
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
            float head_len = dlen * 0.22f;
            if (head_len > 0.25f) head_len = 0.25f;
            if (head_len < 0.06f) head_len = (dlen < 0.06f ? dlen * 0.5f : 0.06f);
            float base[3] = {
                p1[0] - dir[0] * head_len,
                p1[1] - dir[1] * head_len,
                p1[2] - dir[2] * head_len
            };

            draw_pulse_segment(snapshot, p0, base, rgb);

            glLineWidth(3.0f);
            glColor4f(head_rgb[0], head_rgb[1], head_rgb[2], 0.95f);
            draw_arrow_head(p1, dir, head_len);
            glLineWidth(1.0f);

            glPointSize(9.0f);
            glBegin(GL_POINTS);
            glColor4f(head_rgb[0], head_rgb[1], head_rgb[2], 1.0f);
            glVertex3f(p1[0], p1[1], p1[2]);
            glEnd();
            glPointSize(1.0f);
        }
    } else {
        const float axes[3][3] = { {1,0,0}, {0,1,0}, {0,0,1} };
        const int perp_a[3] = { 1, 0, 0 };
        const int perp_b[3] = { 2, 2, 1 };
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
            glColor4f(0.55f, 0.55f, 0.55f, fminf(0.45f * snapshot->alpha_scale, 1.0f));
            glBegin(GL_LINES);
            glVertex3f(0.0f, 0.0f, 0.0f);
            glVertex3f(ax[0], ax[1], ax[2]);
            glEnd();

            glLineWidth(2.0f);
            glColor4f(0.9f, 0.9f, 0.9f, 0.9f);
            glBegin(GL_LINES);
            glVertex3f(ax[0] - pa[0]*tick, ax[1] - pa[1]*tick, ax[2] - pa[2]*tick);
            glVertex3f(ax[0] + pa[0]*tick, ax[1] + pa[1]*tick, ax[2] + pa[2]*tick);
            glVertex3f(ax[0] - pb[0]*tick, ax[1] - pb[1]*tick, ax[2] - pb[2]*tick);
            glVertex3f(ax[0] + pb[0]*tick, ax[1] + pb[1]*tick, ax[2] + pb[2]*tick);
            glEnd();
            glPointSize(5.0f);
            glBegin(GL_POINTS);
            glColor4f(0.95f, 0.95f, 0.95f, 1.0f);
            glVertex3f(ax[0], ax[1], ax[2]);
            glEnd();
            glPointSize(1.0f);

            if (fabsf(f - 1.0f) < 1e-4f) continue;

            float tip[3] = { ax[0]*f, ax[1]*f, ax[2]*f };
            float from[3] = { ax[0], ax[1], ax[2] };
            float delta[3] = { tip[0]-from[0], tip[1]-from[1], tip[2]-from[2] };
            float dlen = sqrtf(delta[0]*delta[0] + delta[1]*delta[1] + delta[2]*delta[2]);
            float dir[3] = { delta[0]/dlen, delta[1]/dlen, delta[2]/dlen };

            float head_len = dlen * 0.22f;
            if (head_len > 0.2f) head_len = 0.2f;
            if (head_len < 0.05f) head_len = (dlen < 0.05f ? dlen * 0.5f : 0.05f);
            float base[3] = {
                tip[0] - dir[0] * head_len,
                tip[1] - dir[1] * head_len,
                tip[2] - dir[2] * head_len
            };

            draw_pulse_segment(snapshot, from, base, axis_rgb[a]);

            glLineWidth(2.5f);
            glColor4f(axis_rgb[a][0]*0.6f + 0.4f,
                      axis_rgb[a][1]*0.6f + 0.4f,
                      axis_rgb[a][2]*0.6f + 0.4f, 0.95f);
            draw_arrow_head(tip, dir, head_len);
            glLineWidth(1.0f);

            glPointSize(7.0f);
            glBegin(GL_POINTS);
            glColor4f(axis_rgb[a][0]*0.6f + 0.4f,
                      axis_rgb[a][1]*0.6f + 0.4f,
                      axis_rgb[a][2]*0.6f + 0.4f, 1.0f);
            glVertex3f(tip[0], tip[1], tip[2]);
            glEnd();
            glPointSize(1.0f);
        }
    }

    glDisable(GL_BLEND);
    if (snapshot->user_lighting_enabled) glEnable(GL_LIGHTING);
    transform_guides_pop_state();
}

static void draw_rotate_guide(const SceneGuideSnapshot *snapshot,
                              const GLCmd *cmd, const float p_start[3]) {
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

    float axis_len = (plen > 0.5f ? plen : 0.5f) * 1.1f;
    float as = snapshot->alpha_scale;
    glLineWidth(2.0f);
    glColor4f(rgb[0], rgb[1], rgb[2], fminf(0.55f * as, 1.0f));
    glBegin(GL_LINES);
    glVertex3f(-ax*axis_len, -ay*axis_len, -az*axis_len);
    glVertex3f( ax*axis_len,  ay*axis_len,  az*axis_len);
    glEnd();

    const int segs = 48;
    float angle_rad = angle_deg * (float)(M_PI / 180.0);
    int use_helix = (plen < 0.05f);

    float arc[49][3];
    if (!use_helix) {
        for (int i = 0; i <= segs; i++) {
            float t = (float)i / (float)segs;
            float th = angle_rad * t;
            float c = cosf(th), s = sinf(th), k = 1.0f - c;
            float px = p_start[0], py = p_start[1], pz = p_start[2];
            arc[i][0] = (c + ax*ax*k)    * px + (ax*ay*k - az*s) * py + (ax*az*k + ay*s) * pz;
            arc[i][1] = (ay*ax*k + az*s) * px + (c + ay*ay*k)    * py + (ay*az*k - ax*s) * pz;
            arc[i][2] = (az*ax*k - ay*s) * px + (az*ay*k + ax*s) * py + (c + az*az*k)    * pz;
        }
    } else {
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

    glLineWidth(2.0f);
    glColor4f(rgb[0], rgb[1], rgb[2], fminf(0.30f * as, 1.0f));
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= segs; i++) glVertex3fv(arc[i]);
    glEnd();

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
    glColor4f(rgb[0], rgb[1], rgb[2], fminf(0.05f * as, 1.0f));
    glVertex3f(trail[0], trail[1], trail[2]);
    for (int i = i_tp + 1; i <= i_pos; i++) {
        float u = (float)(i - i_tp) / (float)(i_pos - i_tp + 1);
        glColor4f(rgb[0], rgb[1], rgb[2], fminf((0.05f + (glow * 0.7f) * u) * as, 1.0f));
        glVertex3fv(arc[i]);
    }
    glColor4f(rgb[0], rgb[1], rgb[2], fminf(glow * 0.75f * as, 1.0f));
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glLineWidth(1.0f);

    glPointSize(8.0f);
    glBegin(GL_POINTS);
    glColor4f(bright[0], bright[1], bright[2], glow);
    glVertex3f(pos[0], pos[1], pos[2]);
    glEnd();
    glPointSize(1.0f);

    glPointSize(6.0f);
    glBegin(GL_POINTS);
    glColor4f(rgb[0], rgb[1], rgb[2], 0.7f);
    glVertex3f(p_start[0], p_start[1], p_start[2]);
    glEnd();
    glPointSize(10.0f);
    glBegin(GL_POINTS);
    glColor4f(bright[0], bright[1], bright[2], 0.95f);
    glVertex3fv(arc[segs]);
    glEnd();
    glPointSize(1.0f);

    glDisable(GL_BLEND);
    if (snapshot->user_lighting_enabled) glEnable(GL_LIGHTING);
    transform_guides_pop_state();
}

static int transform_source_unmodified(const SceneGuideSnapshot *snapshot,
                                       const GLCmd *source_cmd) {
    (void)source_cmd;
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

int transform_guides_prepare(const SceneGuideSnapshot *snapshot,
                                   SceneTransformGuidePlan *plan) {
    if (!snapshot || !plan)
        return 0;

    *plan = (SceneTransformGuidePlan){
        .active = 0,
        .consumed = 0,
        .cursor_flat_idx = -1,
        .after_flat_idx = -1,
    };

    if (snapshot->replaying || !snapshot->show_guides)
        return 0;

    int edit_line_idx = snapshot->edit_line_idx;
    if (edit_line_idx < 0 || edit_line_idx >= snapshot->source_cmd_count)
        return 0;

    const GLCmd *source_cmd = &snapshot->source_cmds[edit_line_idx];
    if (!source_cmd->valid)
        return 0;
    if (!transform_source_unmodified(snapshot, source_cmd))
        return 0;
    if (!(source_cmd->type == CMD_TRANSLATE3F ||
          source_cmd->type == CMD_ROTATEF ||
          source_cmd->type == CMD_SCALEF)) {
        return 0;
    }

    const GLCmd *flat_cmds = snapshot->flat_program.cmds;
    int flat_cmd_count = snapshot->flat_program.cmd_count;
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

void transform_guides_render_if_due(const SceneGuideSnapshot *snapshot,
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

    glPushMatrix();
    if (snapshot->xform_guide_mode == 1) {
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

    if (live_cmd->type == CMD_TRANSLATE3F)
        draw_translate_guide(snapshot, live_cmd, guide_origin);
    else if (live_cmd->type == CMD_SCALEF)
        draw_scale_guide(snapshot, live_cmd, guide_origin);
    else
        draw_rotate_guide(snapshot, live_cmd, guide_origin);

    glPopMatrix();
    plan->consumed = 1;
}