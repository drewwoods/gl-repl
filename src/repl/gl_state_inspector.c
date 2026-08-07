/*
 * src/repl/gl_state_inspector.c - Pure OpenGL state fold for source checkpoints.
 *
 * Defaults below come from the OpenGL 2.1 state tables (chapter 6.2). The fold
 * starts with the generated init()/display() setup, then applies user display
 * commands up to the requested source checkpoint while preserving the latest
 * source.
 */
#include "repl/gl_state_inspector.h"

#include "repl/command_spec.h"
#include "repl/attrib_bits.h"   /* repl_attrib_bits_for_cmd (cap->bit membership) */
#include "repl/init_state.h"
#include "repl/state_views.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define REPL_GL_STATE_MAX_CAPS         32
#define REPL_GL_STATE_MATRIX_STACK_MAX 32
#define REPL_GL_STATE_MATERIAL_FACES   2
#define REPL_GL_STATE_MATERIAL_PROPS   5
#define REPL_GL_STATE_CLIP_PLANES      6

enum {
    REPL_GL_MAT_AMBIENT = 0,
    REPL_GL_MAT_DIFFUSE,
    REPL_GL_MAT_SPECULAR,
    REPL_GL_MAT_EMISSION,
    REPL_GL_MAT_SHININESS
};

typedef struct {
    GLenum cap;
    const char *name;
    int current;
    int default_value;
    int touched;
    ReplGlStateChangeSource source;
} ReplGlTrackedCap;

typedef struct {
    float value[4];
    int touched;
    ReplGlStateChangeSource source;
} ReplGlTrackedMaterialValue;

typedef struct {
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float position[4];
    float position_world[4];
    int ambient_touched;
    int diffuse_touched;
    int specular_touched;
    int position_touched;
    int position_world_valid;
    ReplGlStateChangeSource ambient_source;
    ReplGlStateChangeSource diffuse_source;
    ReplGlStateChangeSource specular_source;
    ReplGlStateChangeSource position_source;
} ReplGlTrackedLight;

/* Field order here is the module's canonical cell order: gl_state_apply_cmd()
 * and gl_state_append_report() both walk their cells in this sequence, so a
 * side-by-side read of the three is a straight zip. Where one command writes
 * several cells (glPushMatrix touches the stack depth but not the matrix), it
 * sits at the position of the primary cell it writes.
 *
 * gl_state_restore_attrib_groups() is the deliberate exception - it is ordered
 * by attribute group instead; see its comment. */
typedef struct {
    ReplGlTrackedCap caps[REPL_GL_STATE_MAX_CAPS];
    int cap_count;

    float current_color[4];
    float current_normal[3];
    int current_color_touched;
    int current_normal_touched;
    ReplGlStateChangeSource current_color_source;
    ReplGlStateChangeSource current_normal_source;

    GLenum shade_model;
    int shade_model_touched;
    ReplGlStateChangeSource shade_model_source;

    float matrix_stack[REPL_GL_STATE_MATRIX_STACK_MAX][16];
    int matrix_top;
    int matrix_touched;
    int matrix_depth_touched;
    ReplGlStateChangeSource matrix_source;
    ReplGlStateChangeSource matrix_depth_source;

    int attrib_stack_depth;
    int attrib_stack_depth_touched;
    ReplGlStateChangeSource attrib_stack_depth_source;

    GLenum color_material_face;
    GLenum color_material_mode;
    int color_material_touched;
    ReplGlStateChangeSource color_material_source;

    int light_model_local_viewer;
    int light_model_two_side;
    float light_model_ambient[4];
    int light_model_local_viewer_touched;
    int light_model_two_side_touched;
    int light_model_ambient_touched;
    ReplGlStateChangeSource light_model_local_viewer_source;
    ReplGlStateChangeSource light_model_two_side_source;
    ReplGlStateChangeSource light_model_ambient_source;

    ReplGlTrackedLight lights[REPL_LIGHT_SLOT_COUNT];
    float light_position_world_default[4];
    int light_position_world_default_valid;

    GLenum front_face;
    GLenum cull_face;
    GLenum depth_func;
    /* Front and back rasterization modes are separate GL state, so
     * glPolygonMode(GL_FRONT_AND_BACK, ...) writes both cells. */
    GLenum polygon_mode_front;
    GLenum polygon_mode_back;
    float polygon_offset_factor;
    float polygon_offset_units;
    int front_face_touched;
    int cull_face_touched;
    int depth_func_touched;
    int polygon_mode_touched;
    int polygon_offset_touched;
    ReplGlStateChangeSource front_face_source;
    ReplGlStateChangeSource cull_face_source;
    ReplGlStateChangeSource depth_func_source;
    ReplGlStateChangeSource polygon_mode_source;
    ReplGlStateChangeSource polygon_offset_source;

    ReplGlTrackedMaterialValue
        materials[REPL_GL_STATE_MATERIAL_FACES][REPL_GL_STATE_MATERIAL_PROPS];

    float point_size;
    float line_width;
    int line_stipple_repeat;
    unsigned int line_stipple_pattern;
    float point_attenuation[3];
    int point_size_touched;
    int line_width_touched;
    int line_stipple_touched;
    int point_attenuation_touched;
    ReplGlStateChangeSource point_size_source;
    ReplGlStateChangeSource line_width_source;
    ReplGlStateChangeSource line_stipple_source;
    ReplGlStateChangeSource point_attenuation_source;

    GLenum blend_src;
    GLenum blend_dst;
    int blend_func_touched;
    ReplGlStateChangeSource blend_func_source;

    float clear_color[4];
    int clear_color_touched;
    ReplGlStateChangeSource clear_color_source;

    float clear_depth;
    int clear_depth_touched;
    ReplGlStateChangeSource clear_depth_source;

    /* Stencil test + write state. func/ref/mask move together (one command
     * writes all three), the op slots move together, and the write mask and
     * clear value are independent - matching the attrib_bits cells. */
    GLenum stencil_func;
    int stencil_ref;
    unsigned int stencil_value_mask;
    GLenum stencil_fail_op;
    GLenum stencil_depth_fail_op;
    GLenum stencil_depth_pass_op;
    unsigned int stencil_write_mask;
    int clear_stencil;
    int stencil_func_touched;
    int stencil_op_touched;
    int stencil_write_mask_touched;
    int clear_stencil_touched;
    ReplGlStateChangeSource stencil_func_source;
    ReplGlStateChangeSource stencil_op_source;
    ReplGlStateChangeSource stencil_write_mask_source;
    ReplGlStateChangeSource clear_stencil_source;

    int depth_mask;
    int color_mask[4];
    int edge_flag;
    int depth_mask_touched;
    int color_mask_touched;
    int edge_flag_touched;
    ReplGlStateChangeSource depth_mask_source;
    ReplGlStateChangeSource color_mask_source;
    ReplGlStateChangeSource edge_flag_source;

    /* glRasterPos latches GL_CURRENT_RASTER_COLOR as well as the position, so
     * both cells ride raster_pos_touched/_source: one command writes them
     * together, and nothing else in the REPL command set moves either (a later
     * glColor* changes GL_CURRENT_COLOR only). The latched value is the current
     * color, or the lit color when GL_LIGHTING is on - see
     * gl_state_lit_color(). */
    float raster_pos[4];
    float raster_color[4];
    int raster_pos_touched;
    ReplGlStateChangeSource raster_pos_source;

    float clip_plane[REPL_GL_STATE_CLIP_PLANES][4];
    int clip_plane_touched[REPL_GL_STATE_CLIP_PLANES];
    ReplGlStateChangeSource clip_plane_source[REPL_GL_STATE_CLIP_PLANES];

    GLenum fog_mode;
    float fog_density;
    float fog_start;
    float fog_end;
    float fog_color[4];
    int fog_mode_touched;
    int fog_density_touched;
    int fog_start_touched;
    int fog_end_touched;
    int fog_color_touched;
    ReplGlStateChangeSource fog_mode_source;
    ReplGlStateChangeSource fog_density_source;
    ReplGlStateChangeSource fog_start_source;
    ReplGlStateChangeSource fog_end_source;
    ReplGlStateChangeSource fog_color_source;
} ReplGlTrackedState;

static const float gl_state_light_position_default[4] = { 0, 0, 1, 0 };

static const ReplEnumCommandSpec *gl_state_enum_spec(CmdType type) {
    const ReplEnumCommandSpec *spec = repl_enum_command_specs();
    while (spec && spec->name) {
        if (spec->type == type)
            return spec;
        spec++;
    }
    return NULL;
}

static const char *gl_state_enum_name(CmdType type, int slot, GLenum value) {
    const ReplEnumCommandSpec *spec = gl_state_enum_spec(type);
    const ReplEnumEntry *entry;
    int i;

    if (!spec || slot < 0 || slot >= MAX_ENUM_ARGS)
        return NULL;
    entry = spec->args[slot].enums;
    if (!entry)
        return NULL;
    for (i = 0; entry[i].name; i++)
        if (entry[i].value == value)
            return entry[i].name;
    return NULL;
}

static void gl_state_format_enum(char *buf, size_t n, CmdType type,
                                 int slot, GLenum value) {
    const char *name = gl_state_enum_name(type, slot, value);
    if (name)
        snprintf(buf, n, "%s", name);
    else
        snprintf(buf, n, "0x%X", (unsigned)value);
}

static int gl_state_cap_default(GLenum cap) {
    /* OpenGL 2.1 table 6.15 makes GL_MULTISAMPLE the sole initially-enabled
     * capability in the REPL's glEnable/glDisable token set. */
    return cap == GL_MULTISAMPLE;
}

static void gl_state_mat_identity(float m[16]) {
    int i;
    for (i = 0; i < 16; i++)
        m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void gl_state_mat_mul(const float a[16], const float b[16],
                             float out[16]) {
    float tmp[16];
    int row, col, k;
    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (k = 0; k < 4; k++)
                sum += a[k * 4 + row] * b[col * 4 + k];
            tmp[col * 4 + row] = sum;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void gl_state_mat_vec_mul(const float m[16], const float v[4],
                                 float out[4]) {
    float tmp[4];
    int row, col;
    for (row = 0; row < 4; row++) {
        float sum = 0.0f;
        for (col = 0; col < 4; col++)
            sum += m[col * 4 + row] * v[col];
        tmp[row] = sum;
    }
    memcpy(out, tmp, sizeof(tmp));
}

/* General 4x4 inverse in row-augmented form. The generated camera currently
 * contains only rigid transforms, but keeping this general makes the derived
 * world-space light position correct if the camera bridge later adds scale. */
static int gl_state_mat_inverse(const float m[16], float out[16]) {
    double aug[4][8];
    int pivot, row, col;

    for (row = 0; row < 4; row++) {
        for (col = 0; col < 4; col++) {
            aug[row][col] = (double)m[col * 4 + row];
            aug[row][col + 4] = row == col ? 1.0 : 0.0;
        }
    }

    for (pivot = 0; pivot < 4; pivot++) {
        int best_row = pivot;
        double best = fabs(aug[pivot][pivot]);
        double divisor;
        for (row = pivot + 1; row < 4; row++) {
            double candidate = fabs(aug[row][pivot]);
            if (candidate > best) {
                best = candidate;
                best_row = row;
            }
        }
        if (best <= 1e-12)
            return 0;
        if (best_row != pivot) {
            for (col = 0; col < 8; col++) {
                double tmp = aug[pivot][col];
                aug[pivot][col] = aug[best_row][col];
                aug[best_row][col] = tmp;
            }
        }

        divisor = aug[pivot][pivot];
        for (col = 0; col < 8; col++)
            aug[pivot][col] /= divisor;
        for (row = 0; row < 4; row++) {
            double factor;
            if (row == pivot)
                continue;
            factor = aug[row][pivot];
            for (col = 0; col < 8; col++)
                aug[row][col] -= factor * aug[pivot][col];
        }
    }

    for (row = 0; row < 4; row++)
        for (col = 0; col < 4; col++)
            out[col * 4 + row] = (float)aug[row][col + 4];
    return 1;
}

/* Generated display setup finishes with the camera modelview active. Convert
 * OpenGL's eye-coordinate light state back through that camera once, before
 * user model transforms are folded, so the companion world row is stable at
 * the coordinate in which the light illuminates the scene. This also maps an
 * eye-space/headlight slot to its per-frame world position. */
static void gl_state_capture_light_world_positions(ReplGlTrackedState *s) {
    float inverse[16];
    int i;

    if (!s || !gl_state_mat_inverse(s->matrix_stack[s->matrix_top], inverse))
        return;
    gl_state_mat_vec_mul(inverse, gl_state_light_position_default,
                         s->light_position_world_default);
    s->light_position_world_default_valid = 1;
    for (i = 0; i < REPL_LIGHT_SLOT_COUNT; i++) {
        ReplGlTrackedLight *light = &s->lights[i];
        if (!light->position_touched)
            continue;
        gl_state_mat_vec_mul(inverse, light->position,
                             light->position_world);
        light->position_world_valid = 1;
    }
}

static void gl_state_mat_translate(float m[16], float x, float y, float z) {
    float rhs[16];
    gl_state_mat_identity(rhs);
    rhs[12] = x;
    rhs[13] = y;
    rhs[14] = z;
    gl_state_mat_mul(m, rhs, m);
}

static void gl_state_mat_scale(float m[16], float x, float y, float z) {
    float rhs[16];
    gl_state_mat_identity(rhs);
    rhs[0] = x;
    rhs[5] = y;
    rhs[10] = z;
    gl_state_mat_mul(m, rhs, m);
}

static void gl_state_mat_rotate(float m[16], float angle_deg,
                                float x, float y, float z) {
    float rhs[16];
    float len = sqrtf(x * x + y * y + z * z);
    float radians, c, s, one_c;

    if (len <= 0.0f)
        return;
    x /= len;
    y /= len;
    z /= len;
    radians = angle_deg * (float)(3.14159265358979323846 / 180.0);
    c = cosf(radians);
    s = sinf(radians);
    one_c = 1.0f - c;

    gl_state_mat_identity(rhs);
    rhs[0] = x * x * one_c + c;
    rhs[4] = x * y * one_c - z * s;
    rhs[8] = x * z * one_c + y * s;
    rhs[1] = y * x * one_c + z * s;
    rhs[5] = y * y * one_c + c;
    rhs[9] = y * z * one_c - x * s;
    rhs[2] = z * x * one_c - y * s;
    rhs[6] = z * y * one_c + x * s;
    rhs[10] = z * z * one_c + c;
    gl_state_mat_mul(m, rhs, m);
}

static void gl_state_material_defaults(ReplGlTrackedState *s) {
    int face;
    for (face = 0; face < REPL_GL_STATE_MATERIAL_FACES; face++) {
        ReplGlTrackedMaterialValue *ambient =
            &s->materials[face][REPL_GL_MAT_AMBIENT];
        ReplGlTrackedMaterialValue *diffuse =
            &s->materials[face][REPL_GL_MAT_DIFFUSE];
        ReplGlTrackedMaterialValue *specular =
            &s->materials[face][REPL_GL_MAT_SPECULAR];
        ReplGlTrackedMaterialValue *emission =
            &s->materials[face][REPL_GL_MAT_EMISSION];
        ambient->value[0] = ambient->value[1] = ambient->value[2] = 0.2f;
        ambient->value[3] = 1.0f;
        diffuse->value[0] = diffuse->value[1] = diffuse->value[2] = 0.8f;
        diffuse->value[3] = 1.0f;
        specular->value[3] = 1.0f;
        emission->value[3] = 1.0f;
    }
}

static void gl_state_init(ReplGlTrackedState *s) {
    const ReplEnumCommandSpec *enable_spec;
    const ReplEnumEntry *caps;
    int i;

    memset(s, 0, sizeof(*s));
    enable_spec = gl_state_enum_spec(CMD_ENABLE);
    caps = enable_spec ? enable_spec->args[0].enums : NULL;
    for (i = 0; caps && caps[i].name && i < REPL_GL_STATE_MAX_CAPS; i++) {
        s->caps[i].cap = caps[i].value;
        s->caps[i].name = caps[i].name;
        s->caps[i].default_value = gl_state_cap_default(caps[i].value);
        s->caps[i].current = s->caps[i].default_value;
    }
    s->cap_count = i;

    s->current_color[0] = 1.0f;
    s->current_color[1] = 1.0f;
    s->current_color[2] = 1.0f;
    s->current_color[3] = 1.0f;
    s->current_normal[2] = 1.0f;
    s->shade_model = GL_SMOOTH;
    gl_state_mat_identity(s->matrix_stack[0]);
    s->color_material_face = GL_FRONT_AND_BACK;
    s->color_material_mode = GL_AMBIENT_AND_DIFFUSE;
    s->light_model_ambient[0] = 0.2f;
    s->light_model_ambient[1] = 0.2f;
    s->light_model_ambient[2] = 0.2f;
    s->light_model_ambient[3] = 1.0f;
    for (i = 0; i < REPL_LIGHT_SLOT_COUNT; i++) {
        s->lights[i].ambient[3] = 1.0f;
        s->lights[i].diffuse[3] = 1.0f;
        s->lights[i].specular[3] = 1.0f;
        s->lights[i].position[2] = 1.0f;
        if (i == 0) {
            s->lights[i].diffuse[0] = 1.0f;
            s->lights[i].diffuse[1] = 1.0f;
            s->lights[i].diffuse[2] = 1.0f;
            s->lights[i].specular[0] = 1.0f;
            s->lights[i].specular[1] = 1.0f;
            s->lights[i].specular[2] = 1.0f;
        }
    }
    s->front_face = GL_CCW;
    s->cull_face = GL_BACK;
    s->depth_func = GL_LESS;
    s->polygon_mode_front = GL_FILL;
    s->polygon_mode_back = GL_FILL;
    s->polygon_offset_factor = 0.0f;
    s->polygon_offset_units = 0.0f;
    gl_state_material_defaults(s);
    s->point_size = 1.0f;
    s->line_width = 1.0f;
    s->line_stipple_repeat = 1;
    s->line_stipple_pattern = 0xFFFFu;
    s->point_attenuation[0] = 1.0f;
    s->blend_src = GL_ONE;
    s->blend_dst = GL_ZERO;
    /* GL defaults: always-pass comparison against 0, all bits readable and
     * writable, keep on every outcome, clear to 0. The masks are reported as
     * 0xFF rather than GL's all-ones because the REPL surface is 8-bit
     * (see repl/stencil_limits.h). */
    s->stencil_func = GL_ALWAYS;
    s->stencil_ref = 0;
    s->stencil_value_mask = 0xFFu;
    s->stencil_fail_op = GL_KEEP;
    s->stencil_depth_fail_op = GL_KEEP;
    s->stencil_depth_pass_op = GL_KEEP;
    s->stencil_write_mask = 0xFFu;
    s->clear_stencil = 0;
    s->depth_mask = 1;
    s->color_mask[0] = s->color_mask[1] = 1;
    s->color_mask[2] = s->color_mask[3] = 1;
    s->edge_flag = 1;
    s->raster_pos[3] = 1.0f;
    s->raster_color[0] = s->raster_color[1] = 1.0f;
    s->raster_color[2] = s->raster_color[3] = 1.0f;
    s->fog_mode = GL_EXP;
    s->fog_density = 1.0f;
    s->fog_start = 0.0f;
    s->fog_end = 1.0f;
}

static ReplGlTrackedCap *gl_state_find_cap(ReplGlTrackedState *s, GLenum cap) {
    int i;
    for (i = 0; i < s->cap_count; i++)
        if (s->caps[i].cap == cap)
            return &s->caps[i];
    return NULL;
}

static int gl_state_cap_enabled(ReplGlTrackedState *s, GLenum cap) {
    ReplGlTrackedCap *tracked = gl_state_find_cap(s, cap);
    return tracked ? tracked->current : 0;
}

/* const twin for the report pass, which only reads. */
static int gl_state_cap_enabled_const(const ReplGlTrackedState *s, GLenum cap) {
    int i;
    for (i = 0; i < s->cap_count; i++)
        if (s->caps[i].cap == cap)
            return s->caps[i].current;
    return 0;
}

/* Should light `slot`'s parameter rows appear in the report?
 *
 * The generated display setup writes ambient/diffuse/specular/position for all
 * REPL_LIGHT_SLOT_COUNT slots every frame, so without this gate a report is
 * dominated by up to five rows per light for lights that are switched off -
 * measured at 20 of 37 rows for a three-line program, all reading (0, 0, 0, 0).
 * A disabled light's parameters cannot affect the frame, so they are noise.
 *
 * Enabled-ness alone is the wrong test, though: a program that configures a
 * light above the cursor and calls glEnable(GL_LIGHTn) below it would lose the
 * rows describing exactly the setup being read. So any parameter carrying a
 * user source line keeps the whole light visible regardless of its switch. */
static int gl_state_light_is_interesting(const ReplGlTrackedState *s, int slot) {
    const ReplGlTrackedLight *light = &s->lights[slot];
    if (gl_state_cap_enabled_const(s, (GLenum)(GL_LIGHT0 + slot)))
        return 1;
    return (light->ambient_source.source_line_idx  >= 0 ||
            light->diffuse_source.source_line_idx  >= 0 ||
            light->specular_source.source_line_idx >= 0 ||
            light->position_source.source_line_idx >= 0);
}

static int gl_state_material_face_lo(GLenum face) {
    return face == GL_BACK ? 1 : 0;
}

static int gl_state_material_face_hi(GLenum face) {
    return face == GL_FRONT ? 0 : 1;
}

static int gl_state_material_prop(GLenum pname) {
    switch (pname) {
    case GL_AMBIENT:  return REPL_GL_MAT_AMBIENT;
    case GL_DIFFUSE:  return REPL_GL_MAT_DIFFUSE;
    case GL_SPECULAR: return REPL_GL_MAT_SPECULAR;
    case GL_EMISSION: return REPL_GL_MAT_EMISSION;
    case GL_SHININESS:return REPL_GL_MAT_SHININESS;
    default:          return -1;
    }
}

static void gl_state_set_material(ReplGlTrackedState *s, GLenum face,
                                  GLenum pname, const float *value,
                                  int value_count,
                                  ReplGlStateChangeSource source) {
    int lo = gl_state_material_face_lo(face);
    int hi = gl_state_material_face_hi(face);
    int props[2];
    int prop_count = 1;
    int face_idx, prop_ord;

    if (pname == GL_AMBIENT_AND_DIFFUSE) {
        props[0] = REPL_GL_MAT_AMBIENT;
        props[1] = REPL_GL_MAT_DIFFUSE;
        prop_count = 2;
    } else {
        props[0] = gl_state_material_prop(pname);
        if (props[0] < 0)
            return;
    }

    for (face_idx = lo; face_idx <= hi; face_idx++) {
        for (prop_ord = 0; prop_ord < prop_count; prop_ord++) {
            ReplGlTrackedMaterialValue *dst =
                &s->materials[face_idx][props[prop_ord]];
            int count = props[prop_ord] == REPL_GL_MAT_SHININESS ? 1 : 4;
            int i;
            if (value_count < count)
                continue;
            for (i = 0; i < count; i++)
                dst->value[i] = value[i];
            dst->touched = 1;
            dst->source = source;
        }
    }
}

static void gl_state_apply_color_material(
    ReplGlTrackedState *s, ReplGlStateChangeSource source) {
    if (!gl_state_cap_enabled(s, GL_COLOR_MATERIAL))
        return;
    gl_state_set_material(s, s->color_material_face,
                          s->color_material_mode,
                          s->current_color, 4, source);
}

/* --- Fixed-function lighting (OpenGL 2.1 section 2.14.1) -------------------
 *
 * Evaluated for exactly one reported cell: GL_CURRENT_RASTER_COLOR, which
 * glRasterPos latches from the *lit* color when GL_LIGHTING is on. Vertex
 * colors need no equivalent - they are not state, so the report never quotes
 * one.
 *
 * What the REPL and the generated setup can reach bounds what has to be
 * modelled here. No command writes GL_CONSTANT / LINEAR / QUADRATIC_ATTENUATION
 * or GL_SPOT_CUTOFF / _DIRECTION / _EXPONENT, so the distance-attenuation and
 * spotlight factors keep GL's defaults of 1; and a raster position is a point,
 * where two-sided lighting does not apply (it distinguishes the faces of
 * polygons), so the front material is the one that lights it. Should a
 * spotlight or attenuation setter ever join the command set, this is where it
 * lands - the two factors are the multiplicand of the per-light sum below.
 *
 * The rest is the equation as specified: emission plus scene ambient, then per
 * enabled light an ambient, a diffuse and a Blinn specular term, honouring the
 * local-viewer half vector, GL_NORMALIZE, and glColorMaterial-tracked materials
 * (already folded into materials[] at glColor time). Verified against a real
 * driver in tests/test_gl_state_inspector_gl.c - the only reason this is worth
 * having rather than guessing. */
static float gl_state_vec3_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void gl_state_vec3_normalize(float v[3]) {
    float len = sqrtf(gl_state_vec3_dot(v, v));
    if (len <= 1e-20f)
        return;
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
}

/* Current normal into eye space: GL multiplies by the inverse transpose of the
 * modelview, then normalizes only if GL_NORMALIZE is enabled. A scaled
 * modelview with the switch off skews lighting in GL too, so normalizing here
 * on our own initiative would model a driver nobody has. A singular modelview
 * leaves the normal alone; GL's own result is undefined there. */
static void gl_state_normal_to_eye(const ReplGlTrackedState *s, float out[3]) {
    float inverse[16];
    int row, col;

    if (!gl_state_mat_inverse(s->matrix_stack[s->matrix_top], inverse)) {
        memcpy(out, s->current_normal, 3 * sizeof(float));
    } else {
        /* Column-major: element (r, c) of the inverse is inverse[c * 4 + r],
         * so its transpose reads inverse[r * 4 + c]. */
        for (row = 0; row < 3; row++) {
            float sum = 0.0f;
            for (col = 0; col < 3; col++)
                sum += inverse[row * 4 + col] * s->current_normal[col];
            out[row] = sum;
        }
    }
    if (gl_state_cap_enabled_const(s, GL_NORMALIZE))
        gl_state_vec3_normalize(out);
}

static float gl_state_clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static void gl_state_lit_color(const ReplGlTrackedState *s,
                               const float eye_pos[4], float out[4]) {
    const ReplGlTrackedMaterialValue *mat = s->materials[0];  /* front face */
    const float *m_ambient  = mat[REPL_GL_MAT_AMBIENT].value;
    const float *m_diffuse  = mat[REPL_GL_MAT_DIFFUSE].value;
    const float *m_specular = mat[REPL_GL_MAT_SPECULAR].value;
    const float *m_emission = mat[REPL_GL_MAT_EMISSION].value;
    float shininess = mat[REPL_GL_MAT_SHININESS].value[0];
    float normal[3];
    float vertex[3];
    float to_eye[3];
    int slot, ch;

    gl_state_normal_to_eye(s, normal);
    vertex[0] = eye_pos[0];
    vertex[1] = eye_pos[1];
    vertex[2] = eye_pos[2];
    if (eye_pos[3] != 0.0f && eye_pos[3] != 1.0f) {
        vertex[0] /= eye_pos[3];
        vertex[1] /= eye_pos[3];
        vertex[2] /= eye_pos[3];
    }

    /* Infinite viewer (the default) puts the eye direction at +z; a local
     * viewer aims it at the eye, which sits at the eye-space origin. */
    if (s->light_model_local_viewer) {
        to_eye[0] = -vertex[0];
        to_eye[1] = -vertex[1];
        to_eye[2] = -vertex[2];
        gl_state_vec3_normalize(to_eye);
    } else {
        to_eye[0] = to_eye[1] = 0.0f;
        to_eye[2] = 1.0f;
    }

    for (ch = 0; ch < 3; ch++)
        out[ch] = m_emission[ch] + m_ambient[ch] * s->light_model_ambient[ch];
    /* GL takes the vertex alpha from the diffuse material, not from the sum. */
    out[3] = m_diffuse[3];

    for (slot = 0; slot < REPL_LIGHT_SLOT_COUNT; slot++) {
        const ReplGlTrackedLight *light = &s->lights[slot];
        float to_light[3];
        float n_dot_l;

        if (!gl_state_cap_enabled_const(s, (GLenum)(GL_LIGHT0 + slot)))
            continue;
        /* Light positions are tracked in eye coordinates (GL transforms them
         * at glLight* time), so no further transform here. w == 0 is the
         * directional case: the position *is* the direction to the light. */
        if (light->position[3] == 0.0f) {
            to_light[0] = light->position[0];
            to_light[1] = light->position[1];
            to_light[2] = light->position[2];
        } else {
            to_light[0] = light->position[0] / light->position[3] - vertex[0];
            to_light[1] = light->position[1] / light->position[3] - vertex[1];
            to_light[2] = light->position[2] / light->position[3] - vertex[2];
        }
        gl_state_vec3_normalize(to_light);
        n_dot_l = gl_state_vec3_dot(normal, to_light);

        for (ch = 0; ch < 3; ch++)
            out[ch] += m_ambient[ch] * light->ambient[ch];
        if (n_dot_l <= 0.0f)
            continue;  /* facing away: no diffuse, and specular is gated on it */
        for (ch = 0; ch < 3; ch++)
            out[ch] += n_dot_l * m_diffuse[ch] * light->diffuse[ch];
        {
            float half[3];
            float n_dot_h;
            half[0] = to_light[0] + to_eye[0];
            half[1] = to_light[1] + to_eye[1];
            half[2] = to_light[2] + to_eye[2];
            gl_state_vec3_normalize(half);
            n_dot_h = gl_state_vec3_dot(normal, half);
            if (n_dot_h > 0.0f) {
                float spec = powf(n_dot_h, shininess);
                for (ch = 0; ch < 3; ch++)
                    out[ch] += spec * m_specular[ch] * light->specular[ch];
            }
        }
    }

    for (ch = 0; ch < 4; ch++)
        out[ch] = gl_state_clamp01(out[ch]);
}

static int gl_state_execution_anchor(const GLCmd *cmd) {
    if (cmd->root_call_src_cmd_idx >= 0)
        return cmd->root_call_src_cmd_idx;
    if (cmd->call_src_cmd_idx >= 0)
        return cmd->call_src_cmd_idx;
    return cmd->src_cmd_idx;
}

static int gl_state_command_precedes(const GLCmd *cmd, int source_line_idx) {
    int anchor;
    if (!cmd || !cmd->valid)
        return 0;
    anchor = gl_state_execution_anchor(cmd);
    return anchor >= 0 && anchor < source_line_idx;
}

/* One saved user-attribute frame: the push mask plus a full state snapshot.
 * Restore reads back only the groups the mask covers. */
typedef struct {
    unsigned           mask;
    ReplGlTrackedState snap;
} GlStateAttribFrame;

/* Does `mask` cover the state a command of `type` sets? All bit membership is
 * routed through attrib_bits (the single cell->bit source) so the inspector's
 * restore cannot drift from the analyzer / executor / overlays. */
static int gl_state_mask_covers(unsigned mask, CmdType type) {
    return (mask & repl_attrib_bits_for_type(type, 0)) != 0;
}

/* Restore the tracked state a matching glPushAttrib saved, but only the fields
 * whose covering bit is in `mask`, stamping each restored row's latest change
 * source to the pop line (the policy CMD_POP_MATRIX uses). Each field's bit
 * membership comes from attrib_bits via its setter command type; the only
 * fields with no REPL setter command (the per-light parameters, set solely by
 * generated writes) use the literal GL_LIGHTING_BIT the GL spec assigns them.
 *
 * Ordering: this is the one function in the module that does NOT follow
 * ReplGlTrackedState field order. It is grouped by attribute bit, in
 * k_attrib_bits[] order (command_spec.c) - the question a reader brings here is
 * "what does GL_LINE_BIT restore?", so a bit's cells must sit together. Keep
 * new cells in their group and keep the groups in table order.
 *
 * GL_ENABLE_BIT is the exception to the exception: it cross-cuts every cap
 * rather than owning cells of its own, so it runs as a prologue instead of
 * taking its table slot between GL_TRANSFORM_BIT and GL_COLOR_BUFFER_BIT. */
static void gl_state_restore_attrib_groups(ReplGlTrackedState *s,
                                           const ReplGlTrackedState *snap,
                                           unsigned mask,
                                           ReplGlStateChangeSource source) {
    int i, j;

    /* --- GL_ENABLE_BIT (cross-cutting prologue) ---
     * Enable/disable state of every tracked cap (GL_ENABLE_BIT plus the cap's
     * own group bit). */
    for (i = 0; i < s->cap_count; i++) {
        if (mask & repl_attrib_bits_for_type(CMD_ENABLE, s->caps[i].cap)) {
            s->caps[i].current = snap->caps[i].current;
            s->caps[i].touched = 1;
            s->caps[i].source = source;
        }
    }

    /* --- GL_CURRENT_BIT --- */
    if (gl_state_mask_covers(mask, CMD_COLOR3F)) {
        memcpy(s->current_color, snap->current_color, sizeof(s->current_color));
        s->current_color_touched = 1;
        s->current_color_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_NORMAL3F)) {
        memcpy(s->current_normal, snap->current_normal, sizeof(s->current_normal));
        s->current_normal_touched = 1;
        s->current_normal_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_EDGE_FLAG)) {
        s->edge_flag = snap->edge_flag;
        s->edge_flag_touched = 1;
        s->edge_flag_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_RASTER_POS3F)) {
        memcpy(s->raster_pos, snap->raster_pos, sizeof(s->raster_pos));
        memcpy(s->raster_color, snap->raster_color, sizeof(s->raster_color));
        s->raster_pos_touched = 1;
        s->raster_pos_source = source;
    }

    /* --- GL_POINT_BIT --- */
    if (gl_state_mask_covers(mask, CMD_POINT_SIZE)) {
        s->point_size = snap->point_size;
        s->point_size_touched = 1;
        s->point_size_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_POINT_PARAMETER_FV)) {
        memcpy(s->point_attenuation, snap->point_attenuation,
               sizeof(s->point_attenuation));
        s->point_attenuation_touched = 1;
        s->point_attenuation_source = source;
    }

    /* --- GL_LINE_BIT --- */
    if (gl_state_mask_covers(mask, CMD_LINE_WIDTH)) {
        s->line_width = snap->line_width;
        s->line_width_touched = 1;
        s->line_width_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_LINE_STIPPLE)) {
        s->line_stipple_repeat = snap->line_stipple_repeat;
        s->line_stipple_pattern = snap->line_stipple_pattern;
        s->line_stipple_touched = 1;
        s->line_stipple_source = source;
    }

    /* --- GL_POLYGON_BIT --- */
    if (gl_state_mask_covers(mask, CMD_FRONT_FACE)) {
        s->front_face = snap->front_face;
        s->front_face_touched = 1;
        s->front_face_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_CULL_FACE)) {
        s->cull_face = snap->cull_face;
        s->cull_face_touched = 1;
        s->cull_face_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_POLYGON_MODE)) {
        s->polygon_mode_front = snap->polygon_mode_front;
        s->polygon_mode_back = snap->polygon_mode_back;
        s->polygon_mode_touched = 1;
        s->polygon_mode_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_POLYGON_OFFSET)) {
        s->polygon_offset_factor = snap->polygon_offset_factor;
        s->polygon_offset_units = snap->polygon_offset_units;
        s->polygon_offset_touched = 1;
        s->polygon_offset_source = source;
    }

    /* --- GL_LIGHTING_BIT --- */
    if (gl_state_mask_covers(mask, CMD_SHADE_MODEL)) {
        s->shade_model = snap->shade_model;
        s->shade_model_touched = 1;
        s->shade_model_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_COLOR_MATERIAL)) {
        s->color_material_face = snap->color_material_face;
        s->color_material_mode = snap->color_material_mode;
        s->color_material_touched = 1;
        s->color_material_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_LIGHT_MODEL_I)) {
        s->light_model_local_viewer = snap->light_model_local_viewer;
        s->light_model_local_viewer_touched = 1;
        s->light_model_local_viewer_source = source;
        s->light_model_two_side = snap->light_model_two_side;
        s->light_model_two_side_touched = 1;
        s->light_model_two_side_source = source;
        memcpy(s->light_model_ambient, snap->light_model_ambient,
               sizeof(s->light_model_ambient));
        s->light_model_ambient_touched = 1;
        s->light_model_ambient_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_MATERIALFV)) {
        for (i = 0; i < REPL_GL_STATE_MATERIAL_FACES; i++) {
            for (j = 0; j < REPL_GL_STATE_MATERIAL_PROPS; j++) {
                memcpy(s->materials[i][j].value, snap->materials[i][j].value,
                       sizeof(s->materials[i][j].value));
                s->materials[i][j].touched = 1;
                s->materials[i][j].source = source;
            }
        }
    }
    if (mask & GL_LIGHTING_BIT) {
        /* Per-light parameters have no REPL setter command to key on; the GL
         * spec puts glLight* state under GL_LIGHTING_BIT. */
        for (i = 0; i < REPL_LIGHT_SLOT_COUNT; i++) {
            ReplGlTrackedLight *ld = &s->lights[i];
            const ReplGlTrackedLight *ls = &snap->lights[i];
            memcpy(ld->ambient, ls->ambient, sizeof(ld->ambient));
            memcpy(ld->diffuse, ls->diffuse, sizeof(ld->diffuse));
            memcpy(ld->specular, ls->specular, sizeof(ld->specular));
            memcpy(ld->position, ls->position, sizeof(ld->position));
            ld->ambient_touched = ld->diffuse_touched = 1;
            ld->specular_touched = ld->position_touched = 1;
            ld->ambient_source = ld->diffuse_source = source;
            ld->specular_source = ld->position_source = source;
        }
    }

    /* --- GL_FOG_BIT ---
     * Fog parameters (mode/density/start/end/color) all ride GL_FOG_BIT; the
     * GL_FOG enable flag rides GL_ENABLE_BIT|GL_FOG_BIT and is restored above
     * with the other caps. */
    if (gl_state_mask_covers(mask, CMD_FOG_I)) {
        s->fog_mode = snap->fog_mode;
        s->fog_mode_touched = 1;
        s->fog_mode_source = source;
        s->fog_density = snap->fog_density;
        s->fog_density_touched = 1;
        s->fog_density_source = source;
        s->fog_start = snap->fog_start;
        s->fog_start_touched = 1;
        s->fog_start_source = source;
        s->fog_end = snap->fog_end;
        s->fog_end_touched = 1;
        s->fog_end_source = source;
        memcpy(s->fog_color, snap->fog_color, sizeof(s->fog_color));
        s->fog_color_touched = 1;
        s->fog_color_source = source;
    }

    /* --- GL_DEPTH_BUFFER_BIT --- */
    if (gl_state_mask_covers(mask, CMD_DEPTH_FUNC)) {
        s->depth_func = snap->depth_func;
        s->depth_func_touched = 1;
        s->depth_func_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_DEPTH_MASK)) {
        s->depth_mask = snap->depth_mask;
        s->depth_mask_touched = 1;
        s->depth_mask_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_CLEAR_DEPTH)) {
        s->clear_depth = snap->clear_depth;
        s->clear_depth_touched = 1;
        s->clear_depth_source = source;
    }

    /* --- GL_STENCIL_BUFFER_BIT ---
     * The GL_STENCIL_TEST enable flag rides GL_ENABLE_BIT|GL_STENCIL_BUFFER_BIT
     * and is restored above with the other caps. */
    if (gl_state_mask_covers(mask, CMD_STENCIL_FUNC)) {
        s->stencil_func = snap->stencil_func;
        s->stencil_ref = snap->stencil_ref;
        s->stencil_value_mask = snap->stencil_value_mask;
        s->stencil_func_touched = 1;
        s->stencil_func_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_STENCIL_OP)) {
        s->stencil_fail_op = snap->stencil_fail_op;
        s->stencil_depth_fail_op = snap->stencil_depth_fail_op;
        s->stencil_depth_pass_op = snap->stencil_depth_pass_op;
        s->stencil_op_touched = 1;
        s->stencil_op_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_STENCIL_MASK)) {
        s->stencil_write_mask = snap->stencil_write_mask;
        s->stencil_write_mask_touched = 1;
        s->stencil_write_mask_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_CLEAR_STENCIL)) {
        s->clear_stencil = snap->clear_stencil;
        s->clear_stencil_touched = 1;
        s->clear_stencil_source = source;
    }

    /* --- GL_TRANSFORM_BIT --- */
    if (gl_state_mask_covers(mask, CMD_CLIP_PLANE)) {
        for (i = 0; i < REPL_GL_STATE_CLIP_PLANES; i++) {
            memcpy(s->clip_plane[i], snap->clip_plane[i],
                   sizeof(s->clip_plane[i]));
            s->clip_plane_touched[i] = 1;
            s->clip_plane_source[i] = source;
        }
    }

    /* --- GL_COLOR_BUFFER_BIT --- */
    if (gl_state_mask_covers(mask, CMD_CLEAR_COLOR)) {
        memcpy(s->clear_color, snap->clear_color, sizeof(s->clear_color));
        s->clear_color_touched = 1;
        s->clear_color_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_BLEND_FUNC)) {
        s->blend_src = snap->blend_src;
        s->blend_dst = snap->blend_dst;
        s->blend_func_touched = 1;
        s->blend_func_source = source;
    }
    if (gl_state_mask_covers(mask, CMD_COLOR_MASK)) {
        memcpy(s->color_mask, snap->color_mask, sizeof(s->color_mask));
        s->color_mask_touched = 1;
        s->color_mask_source = source;
    }
}

static void gl_state_apply_cmd(ReplGlTrackedState *s, const GLCmd *cmd,
                               ReplGlStateChangeSource source) {
    ReplGlTrackedCap *cap;
    int i;

    switch (cmd->type) {
    case CMD_ENABLE:
    case CMD_DISABLE:
        cap = gl_state_find_cap(s, (GLenum)cmd->args[0]);
        if (cap) {
            cap->current = cmd->type == CMD_ENABLE;
            cap->touched = 1;
            cap->source = source;
            if (cap->cap == GL_COLOR_MATERIAL && cap->current)
                gl_state_apply_color_material(s, source);
        }
        break;
    case CMD_COLOR3F:
    case CMD_COLOR4F:
        s->current_color[0] = cmd->args[0];
        s->current_color[1] = cmd->args[1];
        s->current_color[2] = cmd->args[2];
        s->current_color[3] = cmd->type == CMD_COLOR4F ? cmd->args[3] : 1.0f;
        s->current_color_touched = 1;
        s->current_color_source = source;
        gl_state_apply_color_material(s, source);
        break;
    case CMD_NORMAL3F:
        memcpy(s->current_normal, cmd->args, 3 * sizeof(float));
        s->current_normal_touched = 1;
        s->current_normal_source = source;
        break;
    case CMD_SHADE_MODEL:
        s->shade_model = (GLenum)cmd->args[0];
        s->shade_model_touched = 1;
        s->shade_model_source = source;
        break;
    case CMD_PUSH_MATRIX:
        if (s->matrix_top + 1 < REPL_GL_STATE_MATRIX_STACK_MAX) {
            memcpy(s->matrix_stack[s->matrix_top + 1],
                   s->matrix_stack[s->matrix_top], 16 * sizeof(float));
            s->matrix_top++;
        }
        s->matrix_depth_touched = 1;
        s->matrix_depth_source = source;
        break;
    case CMD_POP_MATRIX:
        if (s->matrix_top > 0)
            s->matrix_top--;
        s->matrix_touched = 1;
        s->matrix_depth_touched = 1;
        s->matrix_source = source;
        s->matrix_depth_source = source;
        break;
    case CMD_LOAD_IDENTITY:
        gl_state_mat_identity(s->matrix_stack[s->matrix_top]);
        s->matrix_touched = 1;
        s->matrix_source = source;
        break;
    case CMD_TRANSLATE3F:
        gl_state_mat_translate(s->matrix_stack[s->matrix_top],
                               cmd->args[0], cmd->args[1], cmd->args[2]);
        s->matrix_touched = 1;
        s->matrix_source = source;
        break;
    case CMD_SCALEF:
        gl_state_mat_scale(s->matrix_stack[s->matrix_top],
                           cmd->args[0], cmd->args[1], cmd->args[2]);
        s->matrix_touched = 1;
        s->matrix_source = source;
        break;
    case CMD_ROTATEF:
        gl_state_mat_rotate(s->matrix_stack[s->matrix_top],
                            cmd->args[0], cmd->args[1],
                            cmd->args[2], cmd->args[3]);
        s->matrix_touched = 1;
        s->matrix_source = source;
        break;
    case CMD_MULT_MATRIXF:
        /* Both spellings arrive with values on the command: the inline
         * 16-expression form is baked by the parser, the scratch-array form
         * (glMultMatrixf(A)) is snapshotted into payload.matrix by flatten,
         * and this fold walks the flat program. gl_state_mat_mul is
         * post-multiply in the same column-major layout glMultMatrixf takes,
         * and it writes through a temporary, so aliasing out with a is safe
         * (the translate/scale/rotate wrappers above rely on that too). */
        gl_state_mat_mul(s->matrix_stack[s->matrix_top], cmd->payload.matrix.m,
                         s->matrix_stack[s->matrix_top]);
        s->matrix_touched = 1;
        s->matrix_source = source;
        break;
    case CMD_COLOR_MATERIAL:
        s->color_material_face = (GLenum)cmd->args[0];
        s->color_material_mode = (GLenum)cmd->args[1];
        s->color_material_touched = 1;
        s->color_material_source = source;
        gl_state_apply_color_material(s, source);
        break;
    case CMD_LIGHT_MODEL_I:
        if ((GLenum)cmd->args[0] == GL_LIGHT_MODEL_LOCAL_VIEWER) {
            s->light_model_local_viewer = (int)cmd->args[1];
            s->light_model_local_viewer_touched = 1;
            s->light_model_local_viewer_source = source;
        } else if ((GLenum)cmd->args[0] == GL_LIGHT_MODEL_TWO_SIDE) {
            s->light_model_two_side = (int)cmd->args[1];
            s->light_model_two_side_touched = 1;
            s->light_model_two_side_source = source;
        }
        break;
    case CMD_FRONT_FACE:
        s->front_face = (GLenum)cmd->args[0];
        s->front_face_touched = 1;
        s->front_face_source = source;
        break;
    case CMD_CULL_FACE:
        s->cull_face = (GLenum)cmd->args[0];
        s->cull_face_touched = 1;
        s->cull_face_source = source;
        break;
    case CMD_POLYGON_MODE: {
        GLenum face = (GLenum)cmd->args[0];
        GLenum mode = (GLenum)cmd->args[1];
        if (face == GL_FRONT || face == GL_FRONT_AND_BACK)
            s->polygon_mode_front = mode;
        if (face == GL_BACK || face == GL_FRONT_AND_BACK)
            s->polygon_mode_back = mode;
        s->polygon_mode_touched = 1;
        s->polygon_mode_source = source;
        break;
    }
    case CMD_POLYGON_OFFSET:
        s->polygon_offset_factor = cmd->args[0];
        s->polygon_offset_units = cmd->args[1];
        s->polygon_offset_touched = 1;
        s->polygon_offset_source = source;
        break;
    case CMD_DEPTH_FUNC:
        s->depth_func = (GLenum)cmd->args[0];
        s->depth_func_touched = 1;
        s->depth_func_source = source;
        break;
    case CMD_MATERIALFV:
        gl_state_set_material(s, (GLenum)cmd->args[0],
                              (GLenum)cmd->args[1], &cmd->args[2],
                              cmd->num_args - 2, source);
        break;
    case CMD_MATERIALF:
        gl_state_set_material(s, (GLenum)cmd->args[0],
                              (GLenum)cmd->args[1], &cmd->args[2], 1,
                              source);
        break;
    case CMD_POINT_SIZE:
        s->point_size = cmd->args[0];
        s->point_size_touched = 1;
        s->point_size_source = source;
        break;
    case CMD_LINE_WIDTH:
        s->line_width = cmd->args[0];
        s->line_width_touched = 1;
        s->line_width_source = source;
        break;
    case CMD_LINE_STIPPLE:
        s->line_stipple_repeat = (int)cmd->args[0];
        s->line_stipple_pattern = (unsigned int)(GLushort)cmd->args[1];
        s->line_stipple_touched = 1;
        s->line_stipple_source = source;
        break;
    case CMD_POINT_PARAMETER_FV:
        if ((GLenum)cmd->args[0] == GL_POINT_DISTANCE_ATTENUATION) {
            memcpy(s->point_attenuation, &cmd->args[1], 3 * sizeof(float));
            s->point_attenuation_touched = 1;
            s->point_attenuation_source = source;
        }
        break;
    case CMD_BLEND_FUNC:
        s->blend_src = (GLenum)cmd->args[0];
        s->blend_dst = (GLenum)cmd->args[1];
        s->blend_func_touched = 1;
        s->blend_func_source = source;
        break;
    case CMD_CLEAR_COLOR:
        memcpy(s->clear_color, cmd->args, 4 * sizeof(float));
        s->clear_color_touched = 1;
        s->clear_color_source = source;
        break;
    case CMD_CLEAR_DEPTH:
        s->clear_depth = cmd->args[0];
        s->clear_depth_touched = 1;
        s->clear_depth_source = source;
        break;
    case CMD_CLEAR_STENCIL:
        s->clear_stencil = (int)cmd->args[0];
        s->clear_stencil_touched = 1;
        s->clear_stencil_source = source;
        break;
    case CMD_STENCIL_FUNC:
        s->stencil_func = (GLenum)cmd->args[0];
        s->stencil_ref = (int)cmd->args[1];
        s->stencil_value_mask = (unsigned int)cmd->args[2];
        s->stencil_func_touched = 1;
        s->stencil_func_source = source;
        break;
    case CMD_STENCIL_OP:
        s->stencil_fail_op = (GLenum)cmd->args[0];
        s->stencil_depth_fail_op = (GLenum)cmd->args[1];
        s->stencil_depth_pass_op = (GLenum)cmd->args[2];
        s->stencil_op_touched = 1;
        s->stencil_op_source = source;
        break;
    case CMD_STENCIL_MASK:
        s->stencil_write_mask = (unsigned int)cmd->args[0];
        s->stencil_write_mask_touched = 1;
        s->stencil_write_mask_source = source;
        break;
    case CMD_DEPTH_MASK:
        s->depth_mask = cmd->args[0] != 0.0f;
        s->depth_mask_touched = 1;
        s->depth_mask_source = source;
        break;
    case CMD_COLOR_MASK:
        for (i = 0; i < 4; i++)
            s->color_mask[i] = cmd->args[i] != 0.0f;
        s->color_mask_touched = 1;
        s->color_mask_source = source;
        break;
    case CMD_EDGE_FLAG:
        s->edge_flag = cmd->args[0] != 0.0f;
        s->edge_flag_touched = 1;
        s->edge_flag_source = source;
        break;
    case CMD_RASTER_POS3F:
        memcpy(s->raster_pos, cmd->args, 3 * sizeof(float));
        s->raster_pos[3] = 1.0f;
        /* GL 2.1 2.13: the raster position is processed like a vertex, so the
         * data associated with it - here GL_CURRENT_RASTER_COLOR - is latched
         * now and no longer follows GL_CURRENT_COLOR. Lighting applies exactly
         * as it would to a vertex, which is why the fold evaluates it
         * (gl_state_lit_color) instead of quoting the current color and hoping:
         * with GL_LIGHTING on those two are rarely the same number. */
        if (gl_state_cap_enabled(s, GL_LIGHTING)) {
            float eye_pos[4];
            gl_state_mat_vec_mul(s->matrix_stack[s->matrix_top], s->raster_pos,
                                 eye_pos);
            gl_state_lit_color(s, eye_pos, s->raster_color);
        } else {
            /* Clamped like the lit path: the raster color is a vertex's
             * associated color, and RGBA vertex colors are clamped to [0,1]
             * before use (GL 2.1 2.7; the compatibility CLAMP_VERTEX_COLOR
             * defaults to TRUE). Measured on three drivers - Apple M2 and
             * NVIDIA 595.84 store the clamped value, Mesa 25.2.8 keeps the raw
             * one; the panel follows the majority and the spec. GL_CURRENT_COLOR
             * stays unclamped, which is right: nothing has used it yet. */
            int ch;
            for (ch = 0; ch < 4; ch++)
                s->raster_color[ch] = gl_state_clamp01(s->current_color[ch]);
        }
        s->raster_pos_touched = 1;
        s->raster_pos_source = source;
        break;
    case CMD_CLIP_PLANE: {
        int plane = (int)((GLenum)cmd->args[0] - GL_CLIP_PLANE0);
        if (plane >= 0 && plane < REPL_GL_STATE_CLIP_PLANES) {
            memcpy(s->clip_plane[plane], &cmd->args[1], 4 * sizeof(float));
            s->clip_plane_touched[plane] = 1;
            s->clip_plane_source[plane] = source;
        }
        break;
    }
    case CMD_FOG_I:
        if ((GLenum)cmd->args[0] == GL_FOG_MODE) {
            s->fog_mode = (GLenum)cmd->args[1];
            s->fog_mode_touched = 1;
            s->fog_mode_source = source;
        }
        break;
    case CMD_FOG_F:
        if ((GLenum)cmd->args[0] == GL_FOG_DENSITY) {
            s->fog_density = cmd->args[1];
            s->fog_density_touched = 1;
            s->fog_density_source = source;
        } else if ((GLenum)cmd->args[0] == GL_FOG_START) {
            s->fog_start = cmd->args[1];
            s->fog_start_touched = 1;
            s->fog_start_source = source;
        } else if ((GLenum)cmd->args[0] == GL_FOG_END) {
            s->fog_end = cmd->args[1];
            s->fog_end_touched = 1;
            s->fog_end_source = source;
        }
        break;
    case CMD_FOG_FV:
        if ((GLenum)cmd->args[0] == GL_FOG_COLOR) {
            memcpy(s->fog_color, &cmd->args[1], 4 * sizeof(float));
            s->fog_color_touched = 1;
            s->fog_color_source = source;
        }
        break;

    /* Deliberately no `default:` - -Werror=switch then makes a new CmdType a
     * compile error here rather than a silently unmodelled state change (which
     * is exactly how CMD_MULT_MATRIXF went missing from the matrix fold). Every
     * type below is an explicit "does not move any tracked cell"; a new command
     * that does belongs in a case above, not in this list.
     *
     * Geometry: consumes current color/normal/matrix, never writes them. */
    case CMD_BEGIN:
    case CMD_END:
    case CMD_VERTEX3F:
    case CMD_VERTEX2F:
    case CMD_GLUT_TORUS:
    case CMD_GLUT_CUBE:
    case CMD_GLUT_SPHERE:
    case CMD_GLUT_TEAPOT:
    case CMD_GLUT_CONE:
    case CMD_LABEL:
    /* Tessellation: the vertex data rides the tessellator's own cursor and
     * reaches GL through its callbacks, not as a state write at this row. */
    case CMD_TESS_BEGIN_POLYGON:
    case CMD_TESS_BEGIN_CONTOUR:
    case CMD_TESS_END:
    case CMD_TESS_NORMAL:
    case CMD_TESS_COLOR:
    case CMD_TESS_VERTEX:
    /* Framebuffer write; the clear values themselves are tracked via
     * CMD_CLEAR_COLOR / _DEPTH / _STENCIL above. */
    case CMD_CLEAR:
    /* Control flow and variables: resolved by flatten before this walk, so the
     * fold only ever sees their effect baked into the commands it does read. */
    case CMD_FOR_BEGIN:
    case CMD_FOR_END:
    case CMD_BREAK:
    case CMD_CONTINUE:
    case CMD_FUNC_DEF:
    case CMD_FUNC_END:
    case CMD_CALL:
    case CMD_IF_BEGIN:
    case CMD_IF_END:
    case CMD_ELSE_IF:
    case CMD_ELSE:
    case CMD_VAR_ASSIGN:
    case CMD_SCRATCH_ASSIGN:
    case CMD_SCRATCH_BLOCK_ASSIGN:
    case CMD_VAR_DECLARE:
    case CMD_COMMENT:
    case CMD_EMPTY:
    /* Attribute stack: intercepted by the caller (repl_gl_state_report_at_line)
     * so it can snapshot/restore whole groups; never reaches this switch. */
    case CMD_PUSH_ATTRIB:
    case CMD_POP_ATTRIB:
    case CMD_TYPE_COUNT:
        break;
    }
}

static void gl_state_apply_generated_write(
    ReplGlTrackedState *s, const ReplGeneratedStateWrite *write,
    ReplGlStateChangeSource source) {
    ReplGlTrackedLight *light;
    int slot;

    if (!s || !write)
        return;
    if (write->kind == REPL_GENERATED_STATE_COMMAND) {
        gl_state_apply_cmd(s, &write->command, source);
        return;
    }
    if (write->kind == REPL_GENERATED_STATE_PUSH_ATTRIB) {
        s->attrib_stack_depth++;
        s->attrib_stack_depth_touched = 1;
        s->attrib_stack_depth_source = source;
        return;
    }
    if (write->kind == REPL_GENERATED_STATE_LIGHT_MODEL_FV) {
        if (write->pname == GL_LIGHT_MODEL_AMBIENT &&
            write->value_count >= 4) {
            memcpy(s->light_model_ambient, write->value,
                   sizeof(s->light_model_ambient));
            s->light_model_ambient_touched = 1;
            s->light_model_ambient_source = source;
        }
        return;
    }
    if (write->kind != REPL_GENERATED_STATE_LIGHT_FV ||
        write->value_count < 4)
        return;

    if (write->object < GL_LIGHT0 ||
        write->object >= (GLenum)(GL_LIGHT0 + REPL_LIGHT_SLOT_COUNT))
        return;
    slot = (int)(write->object - GL_LIGHT0);
    light = &s->lights[slot];
    switch (write->pname) {
    case GL_AMBIENT:
        memcpy(light->ambient, write->value, sizeof(light->ambient));
        light->ambient_touched = 1;
        light->ambient_source = source;
        break;
    case GL_DIFFUSE:
        memcpy(light->diffuse, write->value, sizeof(light->diffuse));
        light->diffuse_touched = 1;
        light->diffuse_source = source;
        break;
    case GL_SPECULAR:
        memcpy(light->specular, write->value, sizeof(light->specular));
        light->specular_touched = 1;
        light->specular_source = source;
        break;
    case GL_POSITION:
        /* OpenGL stores the position after multiplying it by the current
         * modelview, so show the actual eye-coordinate state-not merely the
         * object-space argument printed in display(). */
        gl_state_mat_vec_mul(s->matrix_stack[s->matrix_top], write->value,
                             light->position);
        light->position_touched = 1;
        light->position_source = source;
        break;
    default:
        break;
    }
}

static int gl_state_float_eq(float a, float b) {
    return fabsf(a - b) <= 1e-6f;
}

static int gl_state_float_array_eq(const float *a, const float *b, int n) {
    int i;
    for (i = 0; i < n; i++)
        if (!gl_state_float_eq(a[i], b[i]))
            return 0;
    return 1;
}

static ReplGlStateReportRow *gl_state_report_row(ReplGlStateReport *out,
                                                  const char *name) {
    ReplGlStateReportRow *row;
    if (out->count >= REPL_GL_STATE_REPORT_MAX_ROWS)
        return NULL;
    row = &out->rows[out->count++];
    memset(row, 0, sizeof(*row));
    snprintf(row->name, sizeof(row->name), "%s", name ? name : "");
    return row;
}

static void gl_state_report_set_last_source(
    ReplGlStateReport *out, ReplGlStateChangeSource source) {
    if (out && out->count > 0)
        out->rows[out->count - 1].source = source;
}

static void gl_state_report_bool(ReplGlStateReport *out, const char *name,
                                 int current, int default_value) {
    ReplGlStateReportRow *row = gl_state_report_row(out, name);
    if (!row)
        return;
    snprintf(row->current, sizeof(row->current), "%s",
             current ? "GL_TRUE" : "GL_FALSE");
    snprintf(row->basis_value, sizeof(row->basis_value), "%s",
             default_value ? "GL_TRUE" : "GL_FALSE");
    row->differs_from_basis = !!current != !!default_value;
}

#define GL_STATE_NUM_FIELD_W 8

static void gl_state_report_int(ReplGlStateReport *out, const char *name,
                                int current, int default_value) {
    ReplGlStateReportRow *row = gl_state_report_row(out, name);
    if (!row)
        return;
    /* Same field as the float cells below, so integer and float rows line up
     * in one column and an animated count (a stencil ref, a stack depth)
     * cannot resize the table under the reader either. */
    snprintf(row->current, sizeof(row->current), "%*d",
             GL_STATE_NUM_FIELD_W, current);
    snprintf(row->basis_value, sizeof(row->basis_value), "%*d",
             GL_STATE_NUM_FIELD_W, default_value);
    row->differs_from_basis = current != default_value;
}

/* Stencil masks read as bit patterns, not counts, so they print the way the
 * source spells them (0xNN) rather than in decimal. */
static void gl_state_report_hex_mask(ReplGlStateReport *out, const char *name,
                                     unsigned int current,
                                     unsigned int default_value) {
    ReplGlStateReportRow *row = gl_state_report_row(out, name);
    if (!row)
        return;
    snprintf(row->current, sizeof(row->current), "0x%02X", current);
    snprintf(row->basis_value, sizeof(row->basis_value), "0x%02X",
             default_value);
    row->differs_from_basis = current != default_value;
}

/* Every float the report prints goes through here, in one fixed-width field.
 *
 * The width is the point. These cells are re-read every frame while `t`
 * advances, and the popup sizes its columns from the widest value it holds -
 * so a free-running "%g" (2 characters for "0.5", 11 for "0.523598776")
 * re-solved the whole table each frame: the value column breathed, the
 * columns to its right slid, and a right-clamped popup walked sideways with
 * them. Nothing about the state had changed; the reader could not hold their
 * place. A constant field makes the layout a function of *which* rows are
 * shown, never of the numbers in them, and aligns the decimal points down the
 * column for free.
 *
 * Four decimals right-aligned in eight cells is the matrix convention (a full
 * 4x4 row still fits the panel's 44-character value cap). Magnitudes that
 * would overflow that fall back to "%.2g", which is at most eight characters
 * for any finite float (sign + "1.2" + "e+38"), so the field width is an
 * invariant rather than a usual case - see test_gl_state_value_field_width. */
static void gl_state_fmt_float(char *buf, size_t n, float v) {
    char num[32];

    if (!buf || n == 0)
        return;
    /* Values that round to zero at this precision print as a clean 0.0000
     * rather than "-0.0000". */
    if (fabsf(v) < 0.00005f)
        v = 0.0f;
    snprintf(num, sizeof(num), "%.4f", (double)v);
    if (strlen(num) > (size_t)GL_STATE_NUM_FIELD_W)
        snprintf(num, sizeof(num), "%.2g", (double)v);
    snprintf(buf, n, "%*s", GL_STATE_NUM_FIELD_W, num);
}

static void gl_state_report_float(ReplGlStateReport *out, const char *name,
                                  float current, float default_value) {
    ReplGlStateReportRow *row = gl_state_report_row(out, name);
    if (!row)
        return;
    gl_state_fmt_float(row->current, sizeof(row->current), current);
    gl_state_fmt_float(row->basis_value, sizeof(row->basis_value),
                       default_value);
    row->differs_from_basis = !gl_state_float_eq(current, default_value);
}

static void gl_state_format_vec(char *buf, size_t n, const float *v, int count) {
    size_t used = 0;
    int i;
    if (!buf || n == 0)
        return;
    buf[0] = '\0';
    used += (size_t)snprintf(buf + used, n - used, "(");
    for (i = 0; i < count && used < n; i++) {
        char cell[GL_STATE_NUM_FIELD_W + 1];
        gl_state_fmt_float(cell, sizeof(cell), v[i]);
        used += (size_t)snprintf(buf + used, n - used, "%s%s",
                                i ? ", " : "", cell);
    }
    if (used < n)
        snprintf(buf + used, n - used, ")");
    else
        buf[n - 1] = '\0';
}

static void gl_state_report_vec(ReplGlStateReport *out, const char *name,
                                const float *current, const float *default_value,
                                int count) {
    ReplGlStateReportRow *row = gl_state_report_row(out, name);
    if (!row)
        return;
    gl_state_format_vec(row->current, sizeof(row->current), current, count);
    gl_state_format_vec(row->basis_value, sizeof(row->basis_value),
                        default_value, count);
    row->differs_from_basis =
        !gl_state_float_array_eq(current, default_value, count);
}

static void gl_state_format_matrix(char *buf, size_t n, const float m[16]) {
    size_t used = 0;
    int row, col;
    if (!buf || n == 0)
        return;
    buf[0] = '\0';
    used += (size_t)snprintf(buf + used, n - used, "[");
    for (row = 0; row < 4 && used < n; row++) {
        for (col = 0; col < 4 && used < n; col++) {
            /* Same fixed field as every other value cell, which is what keeps
             * the four columns aligned and a complete visual row below the
             * state panel's 44-character value cap. */
            char cell[GL_STATE_NUM_FIELD_W + 1];
            gl_state_fmt_float(cell, sizeof(cell), m[col * 4 + row]);
            used += (size_t)snprintf(buf + used, n - used, "%s%s",
                                    col ? " " : "", cell);
        }
        if (row < 3 && used < n)
            used += (size_t)snprintf(buf + used, n - used, "; ");
    }
    if (used < n)
        snprintf(buf + used, n - used, "]");
    else
        buf[n - 1] = '\0';
}

static void gl_state_report_matrix(ReplGlStateReport *out, const float current[16]) {
    float identity[16];
    ReplGlStateReportRow *row =
        gl_state_report_row(out, "GL_MODELVIEW_MATRIX");
    if (!row)
        return;
    gl_state_mat_identity(identity);
    gl_state_format_matrix(row->current, sizeof(row->current), current);
    gl_state_format_matrix(row->basis_value, sizeof(row->basis_value), identity);
    row->differs_from_basis = !gl_state_float_array_eq(current, identity, 16);
}

static void gl_state_report_enum(ReplGlStateReport *out, const char *name,
                                 CmdType type, int slot,
                                 GLenum current, GLenum default_value) {
    ReplGlStateReportRow *row = gl_state_report_row(out, name);
    if (!row)
        return;
    gl_state_format_enum(row->current, sizeof(row->current), type, slot, current);
    gl_state_format_enum(row->basis_value, sizeof(row->basis_value),
                         type, slot, default_value);
    row->differs_from_basis = current != default_value;
}

static const char *const gl_state_material_face_names[2] = {
    "GL_FRONT_MATERIAL", "GL_BACK_MATERIAL"
};

static const char *const gl_state_material_prop_names[5] = {
    "AMBIENT", "DIFFUSE", "SPECULAR", "EMISSION", "SHININESS"
};

static void gl_state_append_report(const ReplGlTrackedState *s,
                                   ReplGlStateReport *out,
                                   int include_inert_lights) {
    static const float color_default[4] = { 1, 1, 1, 1 };
    static const float normal_default[3] = { 0, 0, 1 };
    static const float point_atten_default[3] = { 1, 0, 0 };
    static const float clear_default[4] = { 0, 0, 0, 0 };
    static const float raster_default[4] = { 0, 0, 0, 1 };
    static const float clip_default[4] = { 0, 0, 0, 0 };
    static const float fog_color_default[4] = { 0, 0, 0, 0 };
    static const float light_model_ambient_default[4] = {
        0.2f, 0.2f, 0.2f, 1.0f
    };
    static const float light_ambient_default[4] = { 0, 0, 0, 1 };
    static const float light_black_default[4] = { 0, 0, 0, 1 };
    static const float light_white_default[4] = { 1, 1, 1, 1 };
    int i, face, prop;

    for (i = 0; i < s->cap_count; i++) {
        if (s->caps[i].touched) {
            gl_state_report_bool(out, s->caps[i].name,
                                 s->caps[i].current,
                                 s->caps[i].default_value);
            gl_state_report_set_last_source(out, s->caps[i].source);
        }
    }

    if (s->current_color_touched) {
        gl_state_report_vec(out, "GL_CURRENT_COLOR", s->current_color,
                            color_default, 4);
        gl_state_report_set_last_source(out, s->current_color_source);
    }
    if (s->current_normal_touched) {
        gl_state_report_vec(out, "GL_CURRENT_NORMAL", s->current_normal,
                            normal_default, 3);
        gl_state_report_set_last_source(out, s->current_normal_source);
    }
    if (s->shade_model_touched) {
        gl_state_report_enum(out, "GL_SHADE_MODEL", CMD_SHADE_MODEL, 0,
                             s->shade_model, GL_SMOOTH);
        gl_state_report_set_last_source(out, s->shade_model_source);
    }
    if (s->matrix_touched) {
        gl_state_report_matrix(out, s->matrix_stack[s->matrix_top]);
        gl_state_report_set_last_source(out, s->matrix_source);
    }
    if (s->matrix_depth_touched) {
        gl_state_report_int(out, "GL_MODELVIEW_STACK_DEPTH",
                            s->matrix_top + 1, 1);
        gl_state_report_set_last_source(out, s->matrix_depth_source);
    }
    if (s->attrib_stack_depth_touched) {
        gl_state_report_int(out, "GL_ATTRIB_STACK_DEPTH",
                            s->attrib_stack_depth, 0);
        gl_state_report_set_last_source(out, s->attrib_stack_depth_source);
    }
    if (s->color_material_touched) {
        gl_state_report_enum(out, "GL_COLOR_MATERIAL_FACE",
                             CMD_COLOR_MATERIAL, 0,
                             s->color_material_face, GL_FRONT_AND_BACK);
        gl_state_report_set_last_source(out, s->color_material_source);
        gl_state_report_enum(out, "GL_COLOR_MATERIAL_PARAMETER",
                             CMD_COLOR_MATERIAL, 1,
                             s->color_material_mode, GL_AMBIENT_AND_DIFFUSE);
        gl_state_report_set_last_source(out, s->color_material_source);
    }
    if (s->light_model_local_viewer_touched) {
        gl_state_report_bool(out, "GL_LIGHT_MODEL_LOCAL_VIEWER",
                             s->light_model_local_viewer, 0);
        gl_state_report_set_last_source(out,
                                        s->light_model_local_viewer_source);
    }
    if (s->light_model_two_side_touched) {
        gl_state_report_bool(out, "GL_LIGHT_MODEL_TWO_SIDE",
                             s->light_model_two_side, 0);
        gl_state_report_set_last_source(out,
                                        s->light_model_two_side_source);
    }
    if (s->light_model_ambient_touched) {
        gl_state_report_vec(out, "GL_LIGHT_MODEL_AMBIENT",
                            s->light_model_ambient,
                            light_model_ambient_default, 4);
        gl_state_report_set_last_source(out,
                                        s->light_model_ambient_source);
    }
    for (i = 0; i < REPL_LIGHT_SLOT_COUNT; i++) {
        const ReplGlTrackedLight *light = &s->lights[i];
        const float *light_color_default = i == 0
            ? light_white_default : light_black_default;
        char name[REPL_GL_STATE_NAME_MAX];
        if (!include_inert_lights && !gl_state_light_is_interesting(s, i))
            continue;
        if (light->ambient_touched) {
            snprintf(name, sizeof(name), "GL_LIGHT%d_AMBIENT", i);
            gl_state_report_vec(out, name, light->ambient,
                                light_ambient_default, 4);
            gl_state_report_set_last_source(out, light->ambient_source);
        }
        if (light->diffuse_touched) {
            snprintf(name, sizeof(name), "GL_LIGHT%d_DIFFUSE", i);
            gl_state_report_vec(out, name, light->diffuse,
                                light_color_default, 4);
            gl_state_report_set_last_source(out, light->diffuse_source);
        }
        if (light->specular_touched) {
            snprintf(name, sizeof(name), "GL_LIGHT%d_SPECULAR", i);
            gl_state_report_vec(out, name, light->specular,
                                light_color_default, 4);
            gl_state_report_set_last_source(out, light->specular_source);
        }
        if (light->position_touched) {
            if (light->position_world_valid &&
                s->light_position_world_default_valid) {
                snprintf(name, sizeof(name),
                         "GL_LIGHT%d_POSITION (world)", i);
                gl_state_report_vec(out, name, light->position_world,
                                    s->light_position_world_default, 4);
                gl_state_report_set_last_source(out,
                                                light->position_source);
            }
            snprintf(name, sizeof(name), "GL_LIGHT%d_POSITION (eye)", i);
            gl_state_report_vec(out, name, light->position,
                                gl_state_light_position_default, 4);
            gl_state_report_set_last_source(out, light->position_source);
        }
    }
    if (s->front_face_touched) {
        gl_state_report_enum(out, "GL_FRONT_FACE", CMD_FRONT_FACE, 0,
                             s->front_face, GL_CCW);
        gl_state_report_set_last_source(out, s->front_face_source);
    }
    if (s->cull_face_touched) {
        gl_state_report_enum(out, "GL_CULL_FACE_MODE", CMD_CULL_FACE, 0,
                             s->cull_face, GL_BACK);
        gl_state_report_set_last_source(out, s->cull_face_source);
    }
    if (s->polygon_mode_touched) {
        gl_state_report_enum(out, "GL_POLYGON_MODE (front)", CMD_POLYGON_MODE, 1,
                             s->polygon_mode_front, GL_FILL);
        gl_state_report_set_last_source(out, s->polygon_mode_source);
        gl_state_report_enum(out, "GL_POLYGON_MODE (back)", CMD_POLYGON_MODE, 1,
                             s->polygon_mode_back, GL_FILL);
        gl_state_report_set_last_source(out, s->polygon_mode_source);
    }
    if (s->polygon_offset_touched) {
        gl_state_report_float(out, "GL_POLYGON_OFFSET_FACTOR",
                              s->polygon_offset_factor, 0.0f);
        gl_state_report_set_last_source(out, s->polygon_offset_source);
        gl_state_report_float(out, "GL_POLYGON_OFFSET_UNITS",
                              s->polygon_offset_units, 0.0f);
        gl_state_report_set_last_source(out, s->polygon_offset_source);
    }
    if (s->depth_func_touched) {
        gl_state_report_enum(out, "GL_DEPTH_FUNC", CMD_DEPTH_FUNC, 0,
                             s->depth_func, GL_LESS);
        gl_state_report_set_last_source(out, s->depth_func_source);
    }

    for (face = 0; face < REPL_GL_STATE_MATERIAL_FACES; face++) {
        for (prop = 0; prop < REPL_GL_STATE_MATERIAL_PROPS; prop++) {
            const ReplGlTrackedMaterialValue *mat = &s->materials[face][prop];
            char name[REPL_GL_STATE_NAME_MAX];
            float defaults[4] = { 0, 0, 0, 0 };
            if (!mat->touched)
                continue;
            snprintf(name, sizeof(name), "%s_%s",
                     gl_state_material_face_names[face],
                     gl_state_material_prop_names[prop]);
            if (prop == REPL_GL_MAT_AMBIENT) {
                defaults[0] = defaults[1] = defaults[2] = 0.2f;
                defaults[3] = 1.0f;
            } else if (prop == REPL_GL_MAT_DIFFUSE) {
                defaults[0] = defaults[1] = defaults[2] = 0.8f;
                defaults[3] = 1.0f;
            } else if (prop == REPL_GL_MAT_SPECULAR ||
                       prop == REPL_GL_MAT_EMISSION) {
                defaults[3] = 1.0f;
            }
            if (prop == REPL_GL_MAT_SHININESS)
                gl_state_report_float(out, name, mat->value[0], 0.0f);
            else
                gl_state_report_vec(out, name, mat->value, defaults, 4);
            gl_state_report_set_last_source(out, mat->source);
        }
    }

    if (s->point_size_touched) {
        gl_state_report_float(out, "GL_POINT_SIZE", s->point_size, 1.0f);
        gl_state_report_set_last_source(out, s->point_size_source);
    }
    if (s->line_width_touched) {
        gl_state_report_float(out, "GL_LINE_WIDTH", s->line_width, 1.0f);
        gl_state_report_set_last_source(out, s->line_width_source);
    }
    if (s->line_stipple_touched) {
        ReplGlStateReportRow *row;
        gl_state_report_int(out, "GL_LINE_STIPPLE_REPEAT",
                            s->line_stipple_repeat, 1);
        gl_state_report_set_last_source(out, s->line_stipple_source);
        row = gl_state_report_row(out, "GL_LINE_STIPPLE_PATTERN");
        if (row) {
            snprintf(row->current, sizeof(row->current), "0x%04X",
                     s->line_stipple_pattern & 0xFFFFu);
            snprintf(row->basis_value, sizeof(row->basis_value), "0xFFFF");
            row->differs_from_basis =
                (s->line_stipple_pattern & 0xFFFFu) != 0xFFFFu;
            row->source = s->line_stipple_source;
        }
    }
    if (s->point_attenuation_touched) {
        gl_state_report_vec(out, "GL_POINT_DISTANCE_ATTENUATION",
                            s->point_attenuation, point_atten_default, 3);
        gl_state_report_set_last_source(out, s->point_attenuation_source);
    }
    if (s->blend_func_touched) {
        gl_state_report_enum(out, "GL_BLEND_SRC", CMD_BLEND_FUNC, 0,
                             s->blend_src, GL_ONE);
        gl_state_report_set_last_source(out, s->blend_func_source);
        gl_state_report_enum(out, "GL_BLEND_DST", CMD_BLEND_FUNC, 1,
                             s->blend_dst, GL_ZERO);
        gl_state_report_set_last_source(out, s->blend_func_source);
    }
    if (s->clear_color_touched) {
        gl_state_report_vec(out, "GL_COLOR_CLEAR_VALUE", s->clear_color,
                            clear_default, 4);
        gl_state_report_set_last_source(out, s->clear_color_source);
    }
    if (s->clear_depth_touched) {
        gl_state_report_float(out, "GL_DEPTH_CLEAR_VALUE", s->clear_depth, 1.0f);
        gl_state_report_set_last_source(out, s->clear_depth_source);
    }
    if (s->depth_mask_touched) {
        gl_state_report_bool(out, "GL_DEPTH_WRITEMASK", s->depth_mask, 1);
        gl_state_report_set_last_source(out, s->depth_mask_source);
    }
    if (s->stencil_func_touched) {
        gl_state_report_enum(out, "GL_STENCIL_FUNC", CMD_STENCIL_FUNC, 0,
                             s->stencil_func, GL_ALWAYS);
        gl_state_report_set_last_source(out, s->stencil_func_source);
        gl_state_report_int(out, "GL_STENCIL_REF", s->stencil_ref, 0);
        gl_state_report_set_last_source(out, s->stencil_func_source);
        gl_state_report_hex_mask(out, "GL_STENCIL_VALUE_MASK",
                                 s->stencil_value_mask, 0xFFu);
        gl_state_report_set_last_source(out, s->stencil_func_source);
    }
    if (s->stencil_op_touched) {
        gl_state_report_enum(out, "GL_STENCIL_FAIL", CMD_STENCIL_OP, 0,
                             s->stencil_fail_op, GL_KEEP);
        gl_state_report_set_last_source(out, s->stencil_op_source);
        gl_state_report_enum(out, "GL_STENCIL_PASS_DEPTH_FAIL", CMD_STENCIL_OP, 1,
                             s->stencil_depth_fail_op, GL_KEEP);
        gl_state_report_set_last_source(out, s->stencil_op_source);
        gl_state_report_enum(out, "GL_STENCIL_PASS_DEPTH_PASS", CMD_STENCIL_OP, 2,
                             s->stencil_depth_pass_op, GL_KEEP);
        gl_state_report_set_last_source(out, s->stencil_op_source);
    }
    if (s->stencil_write_mask_touched) {
        gl_state_report_hex_mask(out, "GL_STENCIL_WRITEMASK",
                                 s->stencil_write_mask, 0xFFu);
        gl_state_report_set_last_source(out, s->stencil_write_mask_source);
    }
    if (s->clear_stencil_touched) {
        gl_state_report_int(out, "GL_STENCIL_CLEAR_VALUE", s->clear_stencil, 0);
        gl_state_report_set_last_source(out, s->clear_stencil_source);
    }
    if (s->color_mask_touched) {
        ReplGlStateReportRow *row = gl_state_report_row(out, "GL_COLOR_WRITEMASK");
        if (row) {
            snprintf(row->current, sizeof(row->current), "(%s, %s, %s, %s)",
                     s->color_mask[0] ? "T" : "F",
                     s->color_mask[1] ? "T" : "F",
                     s->color_mask[2] ? "T" : "F",
                     s->color_mask[3] ? "T" : "F");
            snprintf(row->basis_value, sizeof(row->basis_value),
                     "(T, T, T, T)");
            row->differs_from_basis =
                !(s->color_mask[0] && s->color_mask[1] &&
                  s->color_mask[2] && s->color_mask[3]);
            row->source = s->color_mask_source;
        }
    }
    if (s->edge_flag_touched) {
        gl_state_report_bool(out, "GL_EDGE_FLAG", s->edge_flag, 1);
        gl_state_report_set_last_source(out, s->edge_flag_source);
    }
    if (s->raster_pos_touched) {
        gl_state_report_vec(out, "GL_CURRENT_RASTER_POSITION (object input)",
                            s->raster_pos, raster_default, 4);
        gl_state_report_set_last_source(out, s->raster_pos_source);
        /* Gated on the raster *position* latch, not on the current color:
         * glRasterPos3f is the only command that writes this cell, so before
         * the first one the initial (1,1,1,1) says nothing, and after one the
         * row is what `label(...)` actually draws with - the lit color when
         * lighting is on, which is the number a program's own glColor3f will
         * not tell you. */
        gl_state_report_vec(out, "GL_CURRENT_RASTER_COLOR",
                            s->raster_color, color_default, 4);
        gl_state_report_set_last_source(out, s->raster_pos_source);
    }
    for (i = 0; i < REPL_GL_STATE_CLIP_PLANES; i++) {
        if (s->clip_plane_touched[i]) {
            char name[REPL_GL_STATE_NAME_MAX];
            snprintf(name, sizeof(name), "GL_CLIP_PLANE%d_EQUATION (object)", i);
            gl_state_report_vec(out, name, s->clip_plane[i], clip_default, 4);
            gl_state_report_set_last_source(out, s->clip_plane_source[i]);
        }
    }
    if (s->fog_mode_touched) {
        gl_state_report_enum(out, "GL_FOG_MODE", CMD_FOG_I, 1,
                             s->fog_mode, GL_EXP);
        gl_state_report_set_last_source(out, s->fog_mode_source);
    }
    if (s->fog_density_touched) {
        gl_state_report_float(out, "GL_FOG_DENSITY", s->fog_density, 1.0f);
        gl_state_report_set_last_source(out, s->fog_density_source);
    }
    if (s->fog_start_touched) {
        gl_state_report_float(out, "GL_FOG_START", s->fog_start, 0.0f);
        gl_state_report_set_last_source(out, s->fog_start_source);
    }
    if (s->fog_end_touched) {
        gl_state_report_float(out, "GL_FOG_END", s->fog_end, 1.0f);
        gl_state_report_set_last_source(out, s->fog_end_source);
    }
    if (s->fog_color_touched) {
        gl_state_report_vec(out, "GL_FOG_COLOR", s->fog_color,
                            fog_color_default, 4);
        gl_state_report_set_last_source(out, s->fog_color_source);
    }
}

/* Stable-partition the emitted rows so user-authored ones lead, and record the
 * boundary. Insertion-sort-style rotation rather than a comparison sort: the
 * row array is small (<= REPL_GL_STATE_REPORT_MAX_ROWS) and stability is the
 * point - within each group rows must keep gl_state_append_report()'s
 * emission order, which is the module's canonical cell order. */
static void gl_state_partition_report_by_author(ReplGlStateReport *out) {
    int i, boundary = 0;
    for (i = 0; i < out->count; i++) {
        if (out->rows[i].source.source_line_idx < 0)
            continue;
        if (i != boundary) {
            ReplGlStateReportRow moved = out->rows[i];
            int j;
            for (j = i; j > boundary; j--)
                out->rows[j] = out->rows[j - 1];
            out->rows[boundary] = moved;
        }
        boundary++;
    }
    out->user_row_count = boundary;
}

/* Append + partition. Every exit from repl_gl_state_report_at_line() goes
 * through this, so user_row_count is never left stale at 0. */
static void gl_state_finish_report(const ReplGlTrackedState *s,
                                   ReplGlStateReport *out,
                                   int include_inert_lights) {
    gl_state_append_report(s, out, include_inert_lights);
    gl_state_partition_report_by_author(out);
}

static void gl_state_report_at_line(FlatProgramView program,
                                    int source_line_idx,
                                    int include_inert_lights,
                                    ReplGlStateReport *out) {
    ReplGlTrackedState state;
    ReplGlStateChangeSource source;
    ReplGeneratedStateWrite write;
    int write_count;
    int i;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->source_line_idx = source_line_idx;
    out->basis_line_idx = -1;
    gl_state_init(&state);

    source.kind = REPL_GL_STATE_SOURCE_INIT;
    source.source_line_idx = -1;
    write_count = repl_generated_init_state_write_count();
    for (i = 0; i < write_count; i++) {
        if (repl_generated_init_state_write_at(i, &write))
            gl_state_apply_generated_write(&state, &write, source);
    }

    if (source_line_idx < 0) {
        gl_state_finish_report(&state, out, include_inert_lights);
        return;
    }
    source.kind = REPL_GL_STATE_SOURCE_DISPLAY;
    source.source_line_idx = -1;
    write_count = repl_generated_display_state_write_count();
    for (i = 0; i < write_count; i++) {
        if (repl_generated_display_state_write_at(i, &write))
            gl_state_apply_generated_write(&state, &write, source);
    }
    gl_state_capture_light_world_positions(&state);

    if (!program.cmds) {
        gl_state_finish_report(&state, out, include_inert_lights);
        return;
    }

    if (program.cmd_count < 0)
        program.cmd_count = 0;
    /* User glPushAttrib/glPopAttrib scope the fold with a separate virtual
     * depth, kept distinct from the generated GL_ALL_ATTRIB_BITS display
     * bracket (attrib_stack_depth) so an orphan user pop can't consume it.
     * Function-static frame storage (reset via user_attrib_depth = 0 each
     * call) avoids a multi-KB stack snapshot array; not reentrant. */
    static GlStateAttribFrame attrib_frames[REPL_ATTRIB_STACK_CAP];
    int user_attrib_depth = 0;
    ReplGlStateChangeSource user_attrib_source;
    user_attrib_source.kind = REPL_GL_STATE_SOURCE_DISPLAY;
    user_attrib_source.source_line_idx = -1;
    for (i = 0; i < program.cmd_count; i++) {
        const GLCmd *cmd = &program.cmds[i];
        if (!gl_state_command_precedes(cmd, source_line_idx))
            continue;
        source.kind = REPL_GL_STATE_SOURCE_DISPLAY;
        source.source_line_idx = cmd->src_cmd_idx >= 0
            ? cmd->src_cmd_idx : gl_state_execution_anchor(cmd);
        if (cmd->type == CMD_PUSH_ATTRIB) {
            if (user_attrib_depth < REPL_ATTRIB_STACK_CAP) {
                attrib_frames[user_attrib_depth].mask = (unsigned)cmd->args[0];
                attrib_frames[user_attrib_depth].snap = state;
            }
            user_attrib_depth++;
            user_attrib_source = source;
            continue;
        }
        if (cmd->type == CMD_POP_ATTRIB) {
            if (user_attrib_depth > 0) {
                user_attrib_depth--;
                user_attrib_source = source;
                if (user_attrib_depth < REPL_ATTRIB_STACK_CAP)
                    gl_state_restore_attrib_groups(
                        &state, &attrib_frames[user_attrib_depth].snap,
                        attrib_frames[user_attrib_depth].mask, source);
            }
            continue;
        }
        gl_state_apply_cmd(&state, cmd, source);
    }

    /* Reported GL_ATTRIB_STACK_DEPTH = generated bracket depth + the real user
     * push depth (min(virtual, CAP)) still open at the cursor. While user
     * pushes are open, the row's latest-change source is the last user
     * push/pop that moved the depth, not the generated display bracket. */
    if (user_attrib_depth > 0) {
        int eff = user_attrib_depth < REPL_ATTRIB_STACK_CAP
                      ? user_attrib_depth : REPL_ATTRIB_STACK_CAP;
        state.attrib_stack_depth += eff;
        state.attrib_stack_depth_touched = 1;
        state.attrib_stack_depth_source = user_attrib_source;
    }

    gl_state_finish_report(&state, out, include_inert_lights);
}

/* The reported fold: presentation filters applied, which today means a light
 * whose parameters cannot reach the frame contributes no rows. */
void repl_gl_state_report_at_line(FlatProgramView program,
                                  int source_line_idx,
                                  ReplGlStateReport *out) {
    gl_state_report_at_line(program, source_line_idx, 0, out);
}

void repl_gl_state_report_rebase(FlatProgramView program,
                                 int basis_line_idx,
                                 ReplGlStateReport *out) {
    /* The basis fold, built here rather than taken from the caller. It is a
     * value source, never a display, so it is built with the inert-light rows
     * included - and that is load-bearing, not incidental. Rebasing reads a
     * row's absence from the basis as "the fold had not written that state
     * there", which holds for touched-ness because touched-ness only goes
     * from 0 to 1 along a fold. The disabled-light gate is a second reason a
     * row can be absent and it is not monotone: the generated setup writes
     * all four slots and disables them, so a scene that enables one halfway
     * down hides those parameters at a basis above the glEnable and shows
     * them below it. Compared against a gated basis they came back as
     * differences, quoting a GL default the state never held, when nothing
     * about them had changed. Owning the basis build is what makes that
     * unrepresentable - no caller can hand in a filtered one.
     *
     * Static because a ReplGlStateReport is ~50 KB and this runs from the
     * frame path; the popup is single-threaded and consumes the values before
     * returning. */
    static ReplGlStateReport basis;
    int i, j;

    if (!out)
        return;
    gl_state_report_at_line(program, basis_line_idx, 1, &basis);
    out->basis_line_idx = basis.source_line_idx;
    for (i = 0; i < out->count; i++) {
        ReplGlStateReportRow *row = &out->rows[i];
        for (j = 0; j < basis.count; j++) {
            if (strcmp(row->name, basis.rows[j].name) != 0)
                continue;
            snprintf(row->basis_value, sizeof(row->basis_value), "%s",
                     basis.rows[j].current);
            break;
        }
        /* No counterpart: keep the GL default the row was built with - see
         * the header. Either way the comparison is over the formatted text,
         * which is what the popup shows, so a difference the reader cannot
         * see is never reported. */
        row->differs_from_basis = strcmp(row->current, row->basis_value) != 0;
    }
}
