/*
 * program_bounds.c - World-space AABB of the flat program's geometry.
 *
 * See program_bounds.h for why this is a software walk rather than a
 * GL_FEEDBACK capture.
 */

#include "repl/program_bounds.h"

#include "repl/command.h"
#include "repl/transform_utils.h"

#include <math.h>

/* Local half-extents of the GLUT solids, as freeglut actually generates
 * them (third_party/freeglut/src/fg_geometry.c, fg_teapot.c). Pinned here
 * rather than guessed because the flight paths scale off the result:
 *
 *   cube(size)            +-size/2 on every axis
 *   sphere(r)             +-r on every axis
 *   torus(inner, outer)   ring in the XY plane about Z:
 *                         x,y +-(outer + inner), z +-inner
 *   cone(base, height)    x,y +-base, z in [0, height]  (not centred)
 *   teapot(size)          from the Bezier control hull: the patch data is
 *                         emitted as (cp.x, cp.z - 1.575, -cp.y) * size/2
 *                         and reproduced by 4-fold rotation about Y, so the
 *                         horizontal extent is the largest |cp.x|,|cp.y| and
 *                         the vertical one comes from the offset cp.z range.
 */
#define TEAPOT_HALF_XZ 1.7625f
#define TEAPOT_HALF_Y  0.7875f

typedef struct BoundsAccum {
    float min[3];
    float max[3];
    int   any;
} BoundsAccum;

static void accum_point(BoundsAccum *acc, const float p[3]) {
    if (!acc->any) {
        for (int i = 0; i < 3; i++)
            acc->min[i] = acc->max[i] = p[i];
        acc->any = 1;
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (p[i] < acc->min[i])
            acc->min[i] = p[i];
        if (p[i] > acc->max[i])
            acc->max[i] = p[i];
    }
}

static void accum_local_point(BoundsAccum *acc, const float m[16],
                              float x, float y, float z) {
    float local[3];
    float world[3];
    local[0] = x;
    local[1] = y;
    local[2] = z;
    mat4_point_col_major(m, local, world);
    accum_point(acc, world);
}

/* Add the eight corners of a local-space box. A rotated box yields the AABB
 * of its corners, which is an over-estimate - see the header. */
static void accum_local_box(BoundsAccum *acc, const float m[16],
                            const float lo[3], const float hi[3]) {
    for (int i = 0; i < 8; i++) {
        accum_local_point(acc, m,
                          (i & 1) ? hi[0] : lo[0],
                          (i & 2) ? hi[1] : lo[1],
                          (i & 4) ? hi[2] : lo[2]);
    }
}

static void accum_symmetric_box(BoundsAccum *acc, const float m[16],
                                float hx, float hy, float hz) {
    float lo[3], hi[3];
    lo[0] = -hx; lo[1] = -hy; lo[2] = -hz;
    hi[0] =  hx; hi[1] =  hy; hi[2] =  hz;
    accum_local_box(acc, m, lo, hi);
}

/* Local box of one GLUT solid, or 0 if `cmd` is not one. */
static int glut_solid_box(const GLCmd *cmd, float lo[3], float hi[3]) {
    switch (cmd->type) {
    case CMD_GLUT_CUBE: {
        float h = (float)fabs((double)cmd->args[0]) * 0.5f;
        lo[0] = lo[1] = lo[2] = -h;
        hi[0] = hi[1] = hi[2] =  h;
        return 1;
    }
    case CMD_GLUT_SPHERE: {
        float r = (float)fabs((double)cmd->args[0]);
        lo[0] = lo[1] = lo[2] = -r;
        hi[0] = hi[1] = hi[2] =  r;
        return 1;
    }
    case CMD_GLUT_TORUS: {
        float inner = (float)fabs((double)cmd->args[0]);
        float outer = (float)fabs((double)cmd->args[1]);
        float ring  = outer + inner;
        lo[0] = lo[1] = -ring; hi[0] = hi[1] = ring;
        lo[2] = -inner;        hi[2] = inner;
        return 1;
    }
    case CMD_GLUT_CONE: {
        float base   = (float)fabs((double)cmd->args[0]);
        float height = cmd->args[1];
        lo[0] = lo[1] = -base; hi[0] = hi[1] = base;
        lo[2] = (height < 0.0f) ? height : 0.0f;
        hi[2] = (height < 0.0f) ? 0.0f   : height;
        return 1;
    }
    case CMD_GLUT_TEAPOT: {
        float s = (float)fabs((double)cmd->args[0]);
        lo[0] = lo[2] = -TEAPOT_HALF_XZ * s;
        hi[0] = hi[2] =  TEAPOT_HALF_XZ * s;
        lo[1] = -TEAPOT_HALF_Y * s;
        hi[1] =  TEAPOT_HALF_Y * s;
        return 1;
    }
    default:
        return 0;
    }
}

ReplSceneBounds repl_program_bounds(FlatProgramView program, int cmd_count) {
    ReplSceneBounds out;
    BoundsAccum acc;
    Mat4Stack st;
    int in_begin = 0;   /* inside glBegin/glEnd */
    int in_tess = 0;    /* inside a tess polygon */

    for (int i = 0; i < 3; i++)
        out.min[i] = out.max[i] = 0.0f;
    out.valid = 0;

    if (!program.cmds)
        return out;
    if (cmd_count > program.cmd_count)
        cmd_count = program.cmd_count;

    acc.any = 0;
    mat4_stack_init(&st);

    for (int i = 0; i < cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        float lo[3], hi[3];

        if (!cmd->valid)
            continue;

        /* Transforms first: a vertex is placed by the matrix in effect
         * before it, and a transform command emits no geometry itself. */
        mat4_stack_apply_cmd(&st, cmd);

        switch (cmd->type) {
        case CMD_BEGIN:
            in_begin = 1;
            break;
        case CMD_END:
            in_begin = 0;
            break;
        case CMD_TESS_BEGIN_POLYGON:
            in_tess = 1;
            break;
        case CMD_TESS_END:
            in_tess = 0;
            break;
        /* The executor drops vertices outside a block, so they contribute
         * nothing on screen and must not widen the box either. */
        case CMD_VERTEX2F:
            if (in_begin)
                accum_local_point(&acc, st.top, cmd->args[0], cmd->args[1], 0.0f);
            break;
        case CMD_VERTEX3F:
            if (in_begin)
                accum_local_point(&acc, st.top,
                                  cmd->args[0], cmd->args[1], cmd->args[2]);
            break;
        case CMD_VERTEX4F:
            if (in_begin) {
                /* glVertex4f divides through by w; w == 0 is a point at
                 * infinity, which no finite box can contain - skip it
                 * rather than poison the bounds with an infinity. */
                float w = cmd->args[3];
                if (w != 0.0f)
                    accum_local_point(&acc, st.top,
                                      cmd->args[0] / w,
                                      cmd->args[1] / w,
                                      cmd->args[2] / w);
            }
            break;
        case CMD_TESS_VERTEX:
            if (in_tess)
                accum_local_point(&acc, st.top,
                                  cmd->args[0], cmd->args[1], cmd->args[2]);
            break;
        default:
            if (glut_solid_box(cmd, lo, hi))
                accum_local_box(&acc, st.top, lo, hi);
            break;
        }
    }

    /* An overflowed matrix stack means every command after the overflow was
     * placed by the wrong matrix. Report nothing rather than something
     * wrong - a consumer that falls back to a default scale looks far
     * better than one that flies around a box in the wrong place. */
    if (!acc.any || st.overflow)
        return out;

    /* A NaN anywhere (a user expression divided by zero) makes every
     * comparison above false, so the box can come back malformed. Check
     * before publishing, and leave min/max zeroed if it did. */
    for (int i = 0; i < 3; i++) {
        if (!(acc.min[i] <= acc.max[i]))
            return out;
    }

    for (int i = 0; i < 3; i++) {
        out.min[i] = acc.min[i];
        out.max[i] = acc.max[i];
    }
    out.valid = 1;
    return out;
}

void repl_scene_bounds_center(const ReplSceneBounds *b, float out[3]) {
    for (int i = 0; i < 3; i++)
        out[i] = (b && b->valid) ? (b->min[i] + b->max[i]) * 0.5f : 0.0f;
}

float repl_scene_bounds_radius(const ReplSceneBounds *b) {
    float d[3];
    if (!b || !b->valid)
        return 0.0f;
    for (int i = 0; i < 3; i++)
        d[i] = (b->max[i] - b->min[i]) * 0.5f;
    return (float)sqrt((double)(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
}
