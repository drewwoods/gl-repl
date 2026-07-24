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
 * gl_state_restore_attrib_groups() is the deliberate exception — it is ordered
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
    int front_face_touched;
    int cull_face_touched;
    int depth_func_touched;
    ReplGlStateChangeSource front_face_source;
    ReplGlStateChangeSource cull_face_source;
    ReplGlStateChangeSource depth_func_source;

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

    int depth_mask;
    int color_mask[4];
    int edge_flag;
    int depth_mask_touched;
    int color_mask_touched;
    int edge_flag_touched;
    ReplGlStateChangeSource depth_mask_source;
    ReplGlStateChangeSource color_mask_source;
    ReplGlStateChangeSource edge_flag_source;

    float raster_pos[4];
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
    gl_state_material_defaults(s);
    s->point_size = 1.0f;
    s->line_width = 1.0f;
    s->line_stipple_repeat = 1;
    s->line_stipple_pattern = 0xFFFFu;
    s->point_attenuation[0] = 1.0f;
    s->blend_src = GL_ONE;
    s->blend_dst = GL_ZERO;
    s->depth_mask = 1;
    s->color_mask[0] = s->color_mask[1] = 1;
    s->color_mask[2] = s->color_mask[3] = 1;
    s->edge_flag = 1;
    s->raster_pos[3] = 1.0f;
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
 * k_attrib_bits[] order (command_spec.c) — the question a reader brings here is
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
    default:
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
         * modelview, so show the actual eye-coordinate state—not merely the
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
    snprintf(row->default_value, sizeof(row->default_value), "%s",
             default_value ? "GL_TRUE" : "GL_FALSE");
    row->differs_from_default = !!current != !!default_value;
}

static void gl_state_report_int(ReplGlStateReport *out, const char *name,
                                int current, int default_value) {
    ReplGlStateReportRow *row = gl_state_report_row(out, name);
    if (!row)
        return;
    snprintf(row->current, sizeof(row->current), "%d", current);
    snprintf(row->default_value, sizeof(row->default_value), "%d",
             default_value);
    row->differs_from_default = current != default_value;
}

static void gl_state_report_float(ReplGlStateReport *out, const char *name,
                                  float current, float default_value) {
    ReplGlStateReportRow *row = gl_state_report_row(out, name);
    if (!row)
        return;
    snprintf(row->current, sizeof(row->current), "%g", (double)current);
    snprintf(row->default_value, sizeof(row->default_value), "%g",
             (double)default_value);
    row->differs_from_default = !gl_state_float_eq(current, default_value);
}

static void gl_state_format_vec(char *buf, size_t n, const float *v, int count) {
    size_t used = 0;
    int i;
    if (!buf || n == 0)
        return;
    buf[0] = '\0';
    used += (size_t)snprintf(buf + used, n - used, "(");
    for (i = 0; i < count && used < n; i++)
        used += (size_t)snprintf(buf + used, n - used, "%s%g",
                                i ? ", " : "", (double)v[i]);
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
    gl_state_format_vec(row->default_value, sizeof(row->default_value),
                        default_value, count);
    row->differs_from_default =
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
            float value = m[col * 4 + row];
            /* Four fixed decimals in an eight-cell field keep all matrix
             * columns aligned while a complete visual row remains below the
             * state panel's 44-character value cap. Avoid negative zero for
             * values that round to 0.0000 at this precision. */
            if (fabsf(value) < 0.00005f)
                value = 0.0f;
            used += (size_t)snprintf(buf + used, n - used, "%s%8.4f",
                                    col ? " " : "", (double)value);
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
    gl_state_format_matrix(row->default_value, sizeof(row->default_value), identity);
    row->differs_from_default = !gl_state_float_array_eq(current, identity, 16);
}

static void gl_state_report_enum(ReplGlStateReport *out, const char *name,
                                 CmdType type, int slot,
                                 GLenum current, GLenum default_value) {
    ReplGlStateReportRow *row = gl_state_report_row(out, name);
    if (!row)
        return;
    gl_state_format_enum(row->current, sizeof(row->current), type, slot, current);
    gl_state_format_enum(row->default_value, sizeof(row->default_value),
                         type, slot, default_value);
    row->differs_from_default = current != default_value;
}

static const char *const gl_state_material_face_names[2] = {
    "GL_FRONT_MATERIAL", "GL_BACK_MATERIAL"
};

static const char *const gl_state_material_prop_names[5] = {
    "AMBIENT", "DIFFUSE", "SPECULAR", "EMISSION", "SHININESS"
};

static void gl_state_append_report(const ReplGlTrackedState *s,
                                   ReplGlStateReport *out) {
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
            snprintf(row->default_value, sizeof(row->default_value), "0xFFFF");
            row->differs_from_default =
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
    if (s->color_mask_touched) {
        ReplGlStateReportRow *row = gl_state_report_row(out, "GL_COLOR_WRITEMASK");
        if (row) {
            snprintf(row->current, sizeof(row->current), "(%s, %s, %s, %s)",
                     s->color_mask[0] ? "T" : "F",
                     s->color_mask[1] ? "T" : "F",
                     s->color_mask[2] ? "T" : "F",
                     s->color_mask[3] ? "T" : "F");
            snprintf(row->default_value, sizeof(row->default_value),
                     "(T, T, T, T)");
            row->differs_from_default =
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

void repl_gl_state_report_at_line(FlatProgramView program,
                                  int source_line_idx,
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
    gl_state_init(&state);

    source.kind = REPL_GL_STATE_SOURCE_INIT;
    source.source_line_idx = -1;
    write_count = repl_generated_init_state_write_count();
    for (i = 0; i < write_count; i++) {
        if (repl_generated_init_state_write_at(i, &write))
            gl_state_apply_generated_write(&state, &write, source);
    }

    if (source_line_idx < 0) {
        gl_state_append_report(&state, out);
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
        gl_state_append_report(&state, out);
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

    gl_state_append_report(&state, out);
}
