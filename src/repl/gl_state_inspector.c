/*
 * src/repl/gl_state_inspector.c - Pure OpenGL state fold for source checkpoints.
 *
 * Defaults below come from the OpenGL 2.1 state tables (chapter 6.2).  This
 * module intentionally models the state the REPL can author, not the app's
 * startup bootstrap (which enables blending and changes the clear color).
 */
#include "repl/gl_state_inspector.h"

#include "repl/command_spec.h"

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
} ReplGlTrackedCap;

typedef struct {
    float value[4];
    int touched;
} ReplGlTrackedMaterialValue;

typedef struct {
    ReplGlTrackedCap caps[REPL_GL_STATE_MAX_CAPS];
    int cap_count;

    float current_color[4];
    float current_normal[3];
    int current_color_touched;
    int current_normal_touched;

    GLenum shade_model;
    int shade_model_touched;

    float matrix_stack[REPL_GL_STATE_MATRIX_STACK_MAX][16];
    int matrix_top;
    int matrix_touched;
    int matrix_depth_touched;

    GLenum color_material_face;
    GLenum color_material_mode;
    int color_material_touched;

    int light_model_local_viewer;
    int light_model_two_side;
    int light_model_local_viewer_touched;
    int light_model_two_side_touched;

    GLenum front_face;
    GLenum cull_face;
    GLenum depth_func;
    int front_face_touched;
    int cull_face_touched;
    int depth_func_touched;

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

    GLenum blend_src;
    GLenum blend_dst;
    int blend_func_touched;

    float clear_color[4];
    int clear_color_touched;

    int depth_mask;
    int color_mask[4];
    int edge_flag;
    int depth_mask_touched;
    int color_mask_touched;
    int edge_flag_touched;

    float raster_pos[4];
    int raster_pos_touched;

    float clip_plane[REPL_GL_STATE_CLIP_PLANES][4];
    int clip_plane_touched[REPL_GL_STATE_CLIP_PLANES];
} ReplGlTrackedState;

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
                                  int value_count) {
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
        }
    }
}

static void gl_state_apply_color_material(ReplGlTrackedState *s) {
    if (!gl_state_cap_enabled(s, GL_COLOR_MATERIAL))
        return;
    gl_state_set_material(s, s->color_material_face,
                          s->color_material_mode,
                          s->current_color, 4);
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

static void gl_state_apply_cmd(ReplGlTrackedState *s, const GLCmd *cmd) {
    ReplGlTrackedCap *cap;
    int i;

    switch (cmd->type) {
    case CMD_ENABLE:
    case CMD_DISABLE:
        cap = gl_state_find_cap(s, (GLenum)cmd->args[0]);
        if (cap) {
            cap->current = cmd->type == CMD_ENABLE;
            cap->touched = 1;
            if (cap->cap == GL_COLOR_MATERIAL && cap->current)
                gl_state_apply_color_material(s);
        }
        break;
    case CMD_COLOR3F:
    case CMD_COLOR4F:
        s->current_color[0] = cmd->args[0];
        s->current_color[1] = cmd->args[1];
        s->current_color[2] = cmd->args[2];
        s->current_color[3] = cmd->type == CMD_COLOR4F ? cmd->args[3] : 1.0f;
        s->current_color_touched = 1;
        gl_state_apply_color_material(s);
        break;
    case CMD_NORMAL3F:
        memcpy(s->current_normal, cmd->args, 3 * sizeof(float));
        s->current_normal_touched = 1;
        break;
    case CMD_SHADE_MODEL:
        s->shade_model = (GLenum)cmd->args[0];
        s->shade_model_touched = 1;
        break;
    case CMD_PUSH_MATRIX:
        if (s->matrix_top + 1 < REPL_GL_STATE_MATRIX_STACK_MAX) {
            memcpy(s->matrix_stack[s->matrix_top + 1],
                   s->matrix_stack[s->matrix_top], 16 * sizeof(float));
            s->matrix_top++;
        }
        s->matrix_depth_touched = 1;
        break;
    case CMD_POP_MATRIX:
        if (s->matrix_top > 0)
            s->matrix_top--;
        s->matrix_touched = 1;
        s->matrix_depth_touched = 1;
        break;
    case CMD_LOAD_IDENTITY:
        gl_state_mat_identity(s->matrix_stack[s->matrix_top]);
        s->matrix_touched = 1;
        break;
    case CMD_TRANSLATE3F:
        gl_state_mat_translate(s->matrix_stack[s->matrix_top],
                               cmd->args[0], cmd->args[1], cmd->args[2]);
        s->matrix_touched = 1;
        break;
    case CMD_SCALEF:
        gl_state_mat_scale(s->matrix_stack[s->matrix_top],
                           cmd->args[0], cmd->args[1], cmd->args[2]);
        s->matrix_touched = 1;
        break;
    case CMD_ROTATEF:
        gl_state_mat_rotate(s->matrix_stack[s->matrix_top],
                            cmd->args[0], cmd->args[1],
                            cmd->args[2], cmd->args[3]);
        s->matrix_touched = 1;
        break;
    case CMD_COLOR_MATERIAL:
        s->color_material_face = (GLenum)cmd->args[0];
        s->color_material_mode = (GLenum)cmd->args[1];
        s->color_material_touched = 1;
        gl_state_apply_color_material(s);
        break;
    case CMD_LIGHT_MODEL_I:
        if ((GLenum)cmd->args[0] == GL_LIGHT_MODEL_LOCAL_VIEWER) {
            s->light_model_local_viewer = (int)cmd->args[1];
            s->light_model_local_viewer_touched = 1;
        } else if ((GLenum)cmd->args[0] == GL_LIGHT_MODEL_TWO_SIDE) {
            s->light_model_two_side = (int)cmd->args[1];
            s->light_model_two_side_touched = 1;
        }
        break;
    case CMD_FRONT_FACE:
        s->front_face = (GLenum)cmd->args[0];
        s->front_face_touched = 1;
        break;
    case CMD_CULL_FACE:
        s->cull_face = (GLenum)cmd->args[0];
        s->cull_face_touched = 1;
        break;
    case CMD_DEPTH_FUNC:
        s->depth_func = (GLenum)cmd->args[0];
        s->depth_func_touched = 1;
        break;
    case CMD_MATERIALFV:
        gl_state_set_material(s, (GLenum)cmd->args[0],
                              (GLenum)cmd->args[1], &cmd->args[2],
                              cmd->num_args - 2);
        break;
    case CMD_MATERIALF:
        gl_state_set_material(s, (GLenum)cmd->args[0],
                              (GLenum)cmd->args[1], &cmd->args[2], 1);
        break;
    case CMD_POINT_SIZE:
        s->point_size = cmd->args[0];
        s->point_size_touched = 1;
        break;
    case CMD_LINE_WIDTH:
        s->line_width = cmd->args[0];
        s->line_width_touched = 1;
        break;
    case CMD_LINE_STIPPLE:
        s->line_stipple_repeat = (int)cmd->args[0];
        s->line_stipple_pattern = (unsigned int)(GLushort)cmd->args[1];
        s->line_stipple_touched = 1;
        break;
    case CMD_POINT_PARAMETER_FV:
        if ((GLenum)cmd->args[0] == GL_POINT_DISTANCE_ATTENUATION) {
            memcpy(s->point_attenuation, &cmd->args[1], 3 * sizeof(float));
            s->point_attenuation_touched = 1;
        }
        break;
    case CMD_BLEND_FUNC:
        s->blend_src = (GLenum)cmd->args[0];
        s->blend_dst = (GLenum)cmd->args[1];
        s->blend_func_touched = 1;
        break;
    case CMD_CLEAR_COLOR:
        memcpy(s->clear_color, cmd->args, 4 * sizeof(float));
        s->clear_color_touched = 1;
        break;
    case CMD_DEPTH_MASK:
        s->depth_mask = cmd->args[0] != 0.0f;
        s->depth_mask_touched = 1;
        break;
    case CMD_COLOR_MASK:
        for (i = 0; i < 4; i++)
            s->color_mask[i] = cmd->args[i] != 0.0f;
        s->color_mask_touched = 1;
        break;
    case CMD_EDGE_FLAG:
        s->edge_flag = cmd->args[0] != 0.0f;
        s->edge_flag_touched = 1;
        break;
    case CMD_RASTER_POS3F:
        memcpy(s->raster_pos, cmd->args, 3 * sizeof(float));
        s->raster_pos[3] = 1.0f;
        s->raster_pos_touched = 1;
        break;
    case CMD_CLIP_PLANE: {
        int plane = (int)((GLenum)cmd->args[0] - GL_CLIP_PLANE0);
        if (plane >= 0 && plane < REPL_GL_STATE_CLIP_PLANES) {
            memcpy(s->clip_plane[plane], &cmd->args[1], 4 * sizeof(float));
            s->clip_plane_touched[plane] = 1;
        }
        break;
    }
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
            used += (size_t)snprintf(buf + used, n - used, "%s%g",
                                    col ? " " : "",
                                    (double)m[col * 4 + row]);
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
        gl_state_report_row(out, "GL_MODELVIEW_MATRIX (REPL relative)");
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
    int i, face, prop;

    for (i = 0; i < s->cap_count; i++)
        if (s->caps[i].touched)
            gl_state_report_bool(out, s->caps[i].name,
                                 s->caps[i].current,
                                 s->caps[i].default_value);

    if (s->current_color_touched)
        gl_state_report_vec(out, "GL_CURRENT_COLOR", s->current_color,
                            color_default, 4);
    if (s->current_normal_touched)
        gl_state_report_vec(out, "GL_CURRENT_NORMAL", s->current_normal,
                            normal_default, 3);
    if (s->shade_model_touched)
        gl_state_report_enum(out, "GL_SHADE_MODEL", CMD_SHADE_MODEL, 0,
                             s->shade_model, GL_SMOOTH);
    if (s->matrix_touched)
        gl_state_report_matrix(out, s->matrix_stack[s->matrix_top]);
    if (s->matrix_depth_touched)
        gl_state_report_int(out, "GL_MODELVIEW_STACK_DEPTH",
                            s->matrix_top + 1, 1);
    if (s->color_material_touched) {
        gl_state_report_enum(out, "GL_COLOR_MATERIAL_FACE",
                             CMD_COLOR_MATERIAL, 0,
                             s->color_material_face, GL_FRONT_AND_BACK);
        gl_state_report_enum(out, "GL_COLOR_MATERIAL_PARAMETER",
                             CMD_COLOR_MATERIAL, 1,
                             s->color_material_mode, GL_AMBIENT_AND_DIFFUSE);
    }
    if (s->light_model_local_viewer_touched)
        gl_state_report_bool(out, "GL_LIGHT_MODEL_LOCAL_VIEWER",
                             s->light_model_local_viewer, 0);
    if (s->light_model_two_side_touched)
        gl_state_report_bool(out, "GL_LIGHT_MODEL_TWO_SIDE",
                             s->light_model_two_side, 0);
    if (s->front_face_touched)
        gl_state_report_enum(out, "GL_FRONT_FACE", CMD_FRONT_FACE, 0,
                             s->front_face, GL_CCW);
    if (s->cull_face_touched)
        gl_state_report_enum(out, "GL_CULL_FACE_MODE", CMD_CULL_FACE, 0,
                             s->cull_face, GL_BACK);
    if (s->depth_func_touched)
        gl_state_report_enum(out, "GL_DEPTH_FUNC", CMD_DEPTH_FUNC, 0,
                             s->depth_func, GL_LESS);

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
        }
    }

    if (s->point_size_touched)
        gl_state_report_float(out, "GL_POINT_SIZE", s->point_size, 1.0f);
    if (s->line_width_touched)
        gl_state_report_float(out, "GL_LINE_WIDTH", s->line_width, 1.0f);
    if (s->line_stipple_touched) {
        ReplGlStateReportRow *row;
        gl_state_report_int(out, "GL_LINE_STIPPLE_REPEAT",
                            s->line_stipple_repeat, 1);
        row = gl_state_report_row(out, "GL_LINE_STIPPLE_PATTERN");
        if (row) {
            snprintf(row->current, sizeof(row->current), "0x%04X",
                     s->line_stipple_pattern & 0xFFFFu);
            snprintf(row->default_value, sizeof(row->default_value), "0xFFFF");
            row->differs_from_default =
                (s->line_stipple_pattern & 0xFFFFu) != 0xFFFFu;
        }
    }
    if (s->point_attenuation_touched)
        gl_state_report_vec(out, "GL_POINT_DISTANCE_ATTENUATION",
                            s->point_attenuation, point_atten_default, 3);
    if (s->blend_func_touched) {
        gl_state_report_enum(out, "GL_BLEND_SRC", CMD_BLEND_FUNC, 0,
                             s->blend_src, GL_ONE);
        gl_state_report_enum(out, "GL_BLEND_DST", CMD_BLEND_FUNC, 1,
                             s->blend_dst, GL_ZERO);
    }
    if (s->clear_color_touched)
        gl_state_report_vec(out, "GL_COLOR_CLEAR_VALUE", s->clear_color,
                            clear_default, 4);
    if (s->depth_mask_touched)
        gl_state_report_bool(out, "GL_DEPTH_WRITEMASK", s->depth_mask, 1);
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
        }
    }
    if (s->edge_flag_touched)
        gl_state_report_bool(out, "GL_EDGE_FLAG", s->edge_flag, 1);
    if (s->raster_pos_touched)
        gl_state_report_vec(out, "GL_CURRENT_RASTER_POSITION (object input)",
                            s->raster_pos, raster_default, 4);
    for (i = 0; i < REPL_GL_STATE_CLIP_PLANES; i++) {
        if (s->clip_plane_touched[i]) {
            char name[REPL_GL_STATE_NAME_MAX];
            snprintf(name, sizeof(name), "GL_CLIP_PLANE%d_EQUATION (object)", i);
            gl_state_report_vec(out, name, s->clip_plane[i], clip_default, 4);
        }
    }
}

void repl_gl_state_report_at_line(FlatProgramView program,
                                  int source_line_idx,
                                  ReplGlStateReport *out) {
    ReplGlTrackedState state;
    int i;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->source_line_idx = source_line_idx;
    gl_state_init(&state);

    if (!program.cmds || source_line_idx < 0)
        return;
    if (program.cmd_count < 0)
        program.cmd_count = 0;
    for (i = 0; i < program.cmd_count; i++)
        if (gl_state_command_precedes(&program.cmds[i], source_line_idx))
            gl_state_apply_cmd(&state, &program.cmds[i]);

    gl_state_append_report(&state, out);
}
