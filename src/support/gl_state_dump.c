/*
 * gl_state_dump.c - live OpenGL 1.1 state dump (contract in gl_state_dump.h).
 *
 * Everything is table-driven: a row names a state variable, the pname to
 * query it with, how to print it, and - for enum-valued state - which value
 * domain its name lives in. Sections are arrays of rows plus a heading, and
 * the walk is one pass over the section list. State that does not fit the
 * row shape (per-light and per-material parameters, clip-plane equations,
 * texgen planes, the polygon stipple, the context strings) gets a small
 * dedicated emitter called at the point in the order where it belongs.
 *
 * Enum names are resolved *within a domain*, never globally: GL_ZERO,
 * GL_POINTS and GL_FALSE are all 0, so a global number-to-name table would
 * print confident nonsense. A value the domain does not list prints as
 * 0x%04X, which is the honest answer.
 *
 * The whole querying implementation is compiled only against a real desktop
 * GL. Under the no-op stub headers every glGet* answers zero, and the
 * Emscripten/gl4es build has no accumulation buffer, no color index mode and
 * no evaluators to report - so both emit a single explanatory row instead of
 * a page of zeros presented as measurements.
 */

#include "support/gl_state_dump.h"

#include <stdio.h>
#include <string.h>

#include "gl_includes.h"

#if defined(GL_STUBS) || defined(__EMSCRIPTEN__)
#define GL_STATE_DUMP_QUERIES 0
#else
#define GL_STATE_DUMP_QUERIES 1
#endif

/* ========================================================================= */
/* Row sink plumbing shared by both builds                                    */
/* ========================================================================= */

typedef struct {
    FILE *out;
} DumpFileSink;

static void dump_file_emit(void *user_data, const char *name, const char *value) {
    DumpFileSink *sink = (DumpFileSink *)user_data;
    if (!sink || !sink->out) return;
    if (!name)
        fprintf(sink->out, "# %s\n", value ? value : "");
    else
        fprintf(sink->out, "%s=%s\n", name, value ? value : "");
}

void gl_state_dump_write(FILE *out, const char *label) {
    DumpFileSink sink;
    if (!out) return;
    sink.out = out;
    fprintf(out, "# gl-state-dump v1 label=%s\n", label ? label : "(none)");
    gl_state_dump_walk(dump_file_emit, &sink);
}

int gl_state_dump_write_path(const char *path, const char *label) {
    FILE *out;
    if (!path) return 0;
    out = fopen(path, "w");
    if (!out) return 0;
    gl_state_dump_write(out, label);
    fclose(out);
    return 1;
}

#if !GL_STATE_DUMP_QUERIES

void gl_state_dump_walk(GlStateDumpEmitFn emit, void *user_data) {
    if (!emit) return;
    emit(user_data, NULL, "--- context ---");
    emit(user_data, "GL_STATE_DUMP_AVAILABLE", "0");
    emit(user_data, "GL_STATE_DUMP_REASON",
#ifdef GL_STUBS
         "built against the no-op GL stub headers"
#else
         "built for Emscripten/gl4es; desktop GL 1.1 state is not queryable"
#endif
        );
}

#else /* GL_STATE_DUMP_QUERIES */

/* ========================================================================= */
/* Enum value domains                                                         */
/* ========================================================================= */

typedef struct {
    GLenum      value;
    const char *name;
} GlEnumName;

#define ENUM_ROW(sym) { sym, #sym }
#define ENUM_END      { 0, NULL }

static const GlEnumName k_dom_face[] = {
    ENUM_ROW(GL_FRONT), ENUM_ROW(GL_BACK), ENUM_ROW(GL_FRONT_AND_BACK), ENUM_END
};

static const GlEnumName k_dom_compare[] = {
    ENUM_ROW(GL_NEVER), ENUM_ROW(GL_LESS), ENUM_ROW(GL_EQUAL), ENUM_ROW(GL_LEQUAL),
    ENUM_ROW(GL_GREATER), ENUM_ROW(GL_NOTEQUAL), ENUM_ROW(GL_GEQUAL),
    ENUM_ROW(GL_ALWAYS), ENUM_END
};

static const GlEnumName k_dom_blend_factor[] = {
    ENUM_ROW(GL_ZERO), ENUM_ROW(GL_ONE),
    ENUM_ROW(GL_SRC_COLOR), ENUM_ROW(GL_ONE_MINUS_SRC_COLOR),
    ENUM_ROW(GL_DST_COLOR), ENUM_ROW(GL_ONE_MINUS_DST_COLOR),
    ENUM_ROW(GL_SRC_ALPHA), ENUM_ROW(GL_ONE_MINUS_SRC_ALPHA),
    ENUM_ROW(GL_DST_ALPHA), ENUM_ROW(GL_ONE_MINUS_DST_ALPHA),
    ENUM_ROW(GL_SRC_ALPHA_SATURATE), ENUM_END
};

static const GlEnumName k_dom_stencil_op[] = {
    ENUM_ROW(GL_KEEP), ENUM_ROW(GL_ZERO), ENUM_ROW(GL_REPLACE),
    ENUM_ROW(GL_INCR), ENUM_ROW(GL_DECR), ENUM_ROW(GL_INVERT), ENUM_END
};

static const GlEnumName k_dom_matrix_mode[] = {
    ENUM_ROW(GL_MODELVIEW), ENUM_ROW(GL_PROJECTION), ENUM_ROW(GL_TEXTURE), ENUM_END
};

static const GlEnumName k_dom_shade_model[] = {
    ENUM_ROW(GL_FLAT), ENUM_ROW(GL_SMOOTH), ENUM_END
};

static const GlEnumName k_dom_polygon_mode[] = {
    ENUM_ROW(GL_POINT), ENUM_ROW(GL_LINE), ENUM_ROW(GL_FILL), ENUM_END
};

static const GlEnumName k_dom_front_face[] = {
    ENUM_ROW(GL_CW), ENUM_ROW(GL_CCW), ENUM_END
};

static const GlEnumName k_dom_hint[] = {
    ENUM_ROW(GL_FASTEST), ENUM_ROW(GL_NICEST), ENUM_ROW(GL_DONT_CARE), ENUM_END
};

static const GlEnumName k_dom_fog_mode[] = {
    ENUM_ROW(GL_LINEAR), ENUM_ROW(GL_EXP), ENUM_ROW(GL_EXP2), ENUM_END
};

static const GlEnumName k_dom_logic_op[] = {
    ENUM_ROW(GL_CLEAR), ENUM_ROW(GL_AND), ENUM_ROW(GL_AND_REVERSE), ENUM_ROW(GL_COPY),
    ENUM_ROW(GL_AND_INVERTED), ENUM_ROW(GL_NOOP), ENUM_ROW(GL_XOR), ENUM_ROW(GL_OR),
    ENUM_ROW(GL_NOR), ENUM_ROW(GL_EQUIV), ENUM_ROW(GL_INVERT), ENUM_ROW(GL_OR_REVERSE),
    ENUM_ROW(GL_COPY_INVERTED), ENUM_ROW(GL_OR_INVERTED), ENUM_ROW(GL_NAND),
    ENUM_ROW(GL_SET), ENUM_END
};

static const GlEnumName k_dom_buffer[] = {
    ENUM_ROW(GL_NONE), ENUM_ROW(GL_FRONT_LEFT), ENUM_ROW(GL_FRONT_RIGHT),
    ENUM_ROW(GL_BACK_LEFT), ENUM_ROW(GL_BACK_RIGHT), ENUM_ROW(GL_FRONT),
    ENUM_ROW(GL_BACK), ENUM_ROW(GL_LEFT), ENUM_ROW(GL_RIGHT),
    ENUM_ROW(GL_FRONT_AND_BACK), ENUM_ROW(GL_AUX0), ENUM_ROW(GL_AUX1),
    ENUM_ROW(GL_AUX2), ENUM_ROW(GL_AUX3), ENUM_END
};

static const GlEnumName k_dom_tex_env_mode[] = {
    ENUM_ROW(GL_MODULATE), ENUM_ROW(GL_DECAL), ENUM_ROW(GL_BLEND),
    ENUM_ROW(GL_REPLACE), ENUM_END
};

static const GlEnumName k_dom_tex_filter[] = {
    ENUM_ROW(GL_NEAREST), ENUM_ROW(GL_LINEAR),
    ENUM_ROW(GL_NEAREST_MIPMAP_NEAREST), ENUM_ROW(GL_LINEAR_MIPMAP_NEAREST),
    ENUM_ROW(GL_NEAREST_MIPMAP_LINEAR), ENUM_ROW(GL_LINEAR_MIPMAP_LINEAR), ENUM_END
};

static const GlEnumName k_dom_tex_wrap[] = {
    ENUM_ROW(GL_CLAMP), ENUM_ROW(GL_REPEAT), ENUM_END
};

static const GlEnumName k_dom_texgen_mode[] = {
    ENUM_ROW(GL_OBJECT_LINEAR), ENUM_ROW(GL_EYE_LINEAR), ENUM_ROW(GL_SPHERE_MAP), ENUM_END
};

static const GlEnumName k_dom_color_material[] = {
    ENUM_ROW(GL_EMISSION), ENUM_ROW(GL_AMBIENT), ENUM_ROW(GL_DIFFUSE),
    ENUM_ROW(GL_SPECULAR), ENUM_ROW(GL_AMBIENT_AND_DIFFUSE), ENUM_END
};

static const GlEnumName k_dom_render_mode[] = {
    ENUM_ROW(GL_RENDER), ENUM_ROW(GL_FEEDBACK), ENUM_ROW(GL_SELECT), ENUM_END
};

static const GlEnumName k_dom_list_mode[] = {
    ENUM_ROW(GL_COMPILE), ENUM_ROW(GL_COMPILE_AND_EXECUTE), ENUM_END
};

static const char *enum_name(const GlEnumName *domain, GLint value, char *scratch,
                             size_t scratch_cap) {
    int i;
    for (i = 0; domain && domain[i].name; i++) {
        if ((GLint)domain[i].value == value)
            return domain[i].name;
    }
    snprintf(scratch, scratch_cap, "0x%04X", (unsigned)value);
    return scratch;
}

/* ========================================================================= */
/* Row tables                                                                 */
/* ========================================================================= */

typedef enum {
    ROW_ENABLE,  /* glIsEnabled(pname)                          */
    ROW_BOOL,    /* glGetBooleanv, `count` flags as 0/1         */
    ROW_INT,     /* glGetIntegerv, `count` decimals             */
    ROW_HEX,     /* glGetIntegerv, one mask as 0x%08X           */
    ROW_ENUM,    /* glGetIntegerv, `count` names from `domain`  */
    ROW_FLOAT    /* glGetFloatv, `count` fixed-point decimals   */
} RowKind;

typedef struct {
    const char       *name;
    GLenum            pname;
    RowKind           kind;
    int               count;
    const GlEnumName *domain;
} StateRow;

#define ROW_CAP(sym)             { #sym, sym, ROW_ENABLE, 1, NULL }
#define ROW_B(sym, n)            { #sym, sym, ROW_BOOL,   n, NULL }
#define ROW_I(sym, n)            { #sym, sym, ROW_INT,    n, NULL }
#define ROW_X(sym)               { #sym, sym, ROW_HEX,    1, NULL }
#define ROW_E(sym, n, dom)       { #sym, sym, ROW_ENUM,   n, dom  }
#define ROW_F(sym, n)            { #sym, sym, ROW_FLOAT,  n, NULL }

static const StateRow k_rows_capabilities[] = {
    ROW_CAP(GL_ALPHA_TEST), ROW_CAP(GL_AUTO_NORMAL), ROW_CAP(GL_BLEND),
    ROW_CAP(GL_CLIP_PLANE0), ROW_CAP(GL_CLIP_PLANE1), ROW_CAP(GL_CLIP_PLANE2),
    ROW_CAP(GL_CLIP_PLANE3), ROW_CAP(GL_CLIP_PLANE4), ROW_CAP(GL_CLIP_PLANE5),
    ROW_CAP(GL_COLOR_ARRAY), ROW_CAP(GL_COLOR_LOGIC_OP), ROW_CAP(GL_COLOR_MATERIAL),
    ROW_CAP(GL_CULL_FACE), ROW_CAP(GL_DEPTH_TEST), ROW_CAP(GL_DITHER),
    ROW_CAP(GL_EDGE_FLAG_ARRAY), ROW_CAP(GL_FOG), ROW_CAP(GL_INDEX_ARRAY),
    ROW_CAP(GL_INDEX_LOGIC_OP), ROW_CAP(GL_LIGHTING),
    ROW_CAP(GL_LIGHT0), ROW_CAP(GL_LIGHT1), ROW_CAP(GL_LIGHT2), ROW_CAP(GL_LIGHT3),
    ROW_CAP(GL_LIGHT4), ROW_CAP(GL_LIGHT5), ROW_CAP(GL_LIGHT6), ROW_CAP(GL_LIGHT7),
    ROW_CAP(GL_LINE_SMOOTH), ROW_CAP(GL_LINE_STIPPLE),
    ROW_CAP(GL_MAP1_COLOR_4), ROW_CAP(GL_MAP1_INDEX), ROW_CAP(GL_MAP1_NORMAL),
    ROW_CAP(GL_MAP1_TEXTURE_COORD_1), ROW_CAP(GL_MAP1_TEXTURE_COORD_2),
    ROW_CAP(GL_MAP1_TEXTURE_COORD_3), ROW_CAP(GL_MAP1_TEXTURE_COORD_4),
    ROW_CAP(GL_MAP1_VERTEX_3), ROW_CAP(GL_MAP1_VERTEX_4),
    ROW_CAP(GL_MAP2_COLOR_4), ROW_CAP(GL_MAP2_INDEX), ROW_CAP(GL_MAP2_NORMAL),
    ROW_CAP(GL_MAP2_TEXTURE_COORD_1), ROW_CAP(GL_MAP2_TEXTURE_COORD_2),
    ROW_CAP(GL_MAP2_TEXTURE_COORD_3), ROW_CAP(GL_MAP2_TEXTURE_COORD_4),
    ROW_CAP(GL_MAP2_VERTEX_3), ROW_CAP(GL_MAP2_VERTEX_4),
    ROW_CAP(GL_NORMALIZE), ROW_CAP(GL_NORMAL_ARRAY), ROW_CAP(GL_POINT_SMOOTH),
    ROW_CAP(GL_POLYGON_OFFSET_FILL), ROW_CAP(GL_POLYGON_OFFSET_LINE),
    ROW_CAP(GL_POLYGON_OFFSET_POINT), ROW_CAP(GL_POLYGON_SMOOTH),
    ROW_CAP(GL_POLYGON_STIPPLE), ROW_CAP(GL_SCISSOR_TEST), ROW_CAP(GL_STENCIL_TEST),
    ROW_CAP(GL_TEXTURE_1D), ROW_CAP(GL_TEXTURE_2D), ROW_CAP(GL_TEXTURE_COORD_ARRAY),
    ROW_CAP(GL_TEXTURE_GEN_Q), ROW_CAP(GL_TEXTURE_GEN_R), ROW_CAP(GL_TEXTURE_GEN_S),
    ROW_CAP(GL_TEXTURE_GEN_T), ROW_CAP(GL_VERTEX_ARRAY)
};

static const StateRow k_rows_current[] = {
    ROW_F(GL_CURRENT_COLOR, 4),
    ROW_F(GL_CURRENT_INDEX, 1),
    ROW_F(GL_CURRENT_NORMAL, 3),
    ROW_F(GL_CURRENT_RASTER_COLOR, 4),
    ROW_F(GL_CURRENT_RASTER_DISTANCE, 1),
    ROW_F(GL_CURRENT_RASTER_INDEX, 1),
    ROW_F(GL_CURRENT_RASTER_POSITION, 4),
    ROW_B(GL_CURRENT_RASTER_POSITION_VALID, 1),
    ROW_F(GL_CURRENT_RASTER_TEXTURE_COORDS, 4),
    ROW_F(GL_CURRENT_TEXTURE_COORDS, 4),
    ROW_B(GL_EDGE_FLAG, 1)
};

static const StateRow k_rows_transform[] = {
    ROW_E(GL_MATRIX_MODE, 1, k_dom_matrix_mode),
    ROW_F(GL_MODELVIEW_MATRIX, 16),
    ROW_I(GL_MODELVIEW_STACK_DEPTH, 1),
    ROW_F(GL_PROJECTION_MATRIX, 16),
    ROW_I(GL_PROJECTION_STACK_DEPTH, 1),
    ROW_F(GL_TEXTURE_MATRIX, 16),
    ROW_I(GL_TEXTURE_STACK_DEPTH, 1),
    ROW_F(GL_DEPTH_RANGE, 2),
    ROW_I(GL_VIEWPORT, 4)
};

static const StateRow k_rows_lighting[] = {
    ROW_E(GL_COLOR_MATERIAL_FACE, 1, k_dom_face),
    ROW_E(GL_COLOR_MATERIAL_PARAMETER, 1, k_dom_color_material),
    ROW_F(GL_LIGHT_MODEL_AMBIENT, 4),
    ROW_B(GL_LIGHT_MODEL_LOCAL_VIEWER, 1),
    ROW_B(GL_LIGHT_MODEL_TWO_SIDE, 1),
    ROW_E(GL_SHADE_MODEL, 1, k_dom_shade_model)
};

static const StateRow k_rows_rasterization[] = {
    ROW_E(GL_CULL_FACE_MODE, 1, k_dom_face),
    ROW_E(GL_FRONT_FACE, 1, k_dom_front_face),
    ROW_F(GL_LINE_WIDTH, 1),
    ROW_X(GL_LINE_STIPPLE_PATTERN),
    ROW_I(GL_LINE_STIPPLE_REPEAT, 1),
    ROW_F(GL_POINT_SIZE, 1),
    ROW_E(GL_POLYGON_MODE, 2, k_dom_polygon_mode),
    ROW_F(GL_POLYGON_OFFSET_FACTOR, 1),
    ROW_F(GL_POLYGON_OFFSET_UNITS, 1)
};

static const StateRow k_rows_fog[] = {
    ROW_F(GL_FOG_COLOR, 4),
    ROW_F(GL_FOG_DENSITY, 1),
    ROW_F(GL_FOG_END, 1),
    ROW_F(GL_FOG_INDEX, 1),
    ROW_E(GL_FOG_MODE, 1, k_dom_fog_mode),
    ROW_F(GL_FOG_START, 1)
};

static const StateRow k_rows_texture_env[] = {
    ROW_I(GL_TEXTURE_BINDING_1D, 1),
    ROW_I(GL_TEXTURE_BINDING_2D, 1)
};

static const StateRow k_rows_pixel_ops[] = {
    ROW_E(GL_ALPHA_TEST_FUNC, 1, k_dom_compare),
    ROW_F(GL_ALPHA_TEST_REF, 1),
    ROW_E(GL_BLEND_DST, 1, k_dom_blend_factor),
    ROW_E(GL_BLEND_SRC, 1, k_dom_blend_factor),
    ROW_E(GL_DEPTH_FUNC, 1, k_dom_compare),
    ROW_E(GL_LOGIC_OP_MODE, 1, k_dom_logic_op),
    ROW_I(GL_SCISSOR_BOX, 4),
    ROW_E(GL_STENCIL_FAIL, 1, k_dom_stencil_op),
    ROW_E(GL_STENCIL_FUNC, 1, k_dom_compare),
    ROW_E(GL_STENCIL_PASS_DEPTH_FAIL, 1, k_dom_stencil_op),
    ROW_E(GL_STENCIL_PASS_DEPTH_PASS, 1, k_dom_stencil_op),
    ROW_I(GL_STENCIL_REF, 1),
    ROW_X(GL_STENCIL_VALUE_MASK)
};

static const StateRow k_rows_framebuffer[] = {
    ROW_F(GL_ACCUM_CLEAR_VALUE, 4),
    ROW_F(GL_COLOR_CLEAR_VALUE, 4),
    ROW_B(GL_COLOR_WRITEMASK, 4),
    ROW_F(GL_DEPTH_CLEAR_VALUE, 1),
    ROW_B(GL_DEPTH_WRITEMASK, 1),
    ROW_E(GL_DRAW_BUFFER, 1, k_dom_buffer),
    ROW_X(GL_INDEX_WRITEMASK),
    ROW_E(GL_READ_BUFFER, 1, k_dom_buffer),
    ROW_I(GL_STENCIL_CLEAR_VALUE, 1),
    ROW_X(GL_STENCIL_WRITEMASK)
};

static const StateRow k_rows_pixel_store[] = {
    ROW_F(GL_ALPHA_BIAS, 1),
    ROW_F(GL_ALPHA_SCALE, 1),
    ROW_F(GL_BLUE_BIAS, 1),
    ROW_F(GL_BLUE_SCALE, 1),
    ROW_F(GL_DEPTH_BIAS, 1),
    ROW_F(GL_DEPTH_SCALE, 1),
    ROW_F(GL_GREEN_BIAS, 1),
    ROW_F(GL_GREEN_SCALE, 1),
    ROW_I(GL_INDEX_OFFSET, 1),
    ROW_I(GL_INDEX_SHIFT, 1),
    ROW_B(GL_MAP_COLOR, 1),
    ROW_B(GL_MAP_STENCIL, 1),
    ROW_I(GL_PACK_ALIGNMENT, 1),
    ROW_B(GL_PACK_LSB_FIRST, 1),
    ROW_I(GL_PACK_ROW_LENGTH, 1),
    ROW_I(GL_PACK_SKIP_PIXELS, 1),
    ROW_I(GL_PACK_SKIP_ROWS, 1),
    ROW_B(GL_PACK_SWAP_BYTES, 1),
    ROW_F(GL_RED_BIAS, 1),
    ROW_F(GL_RED_SCALE, 1),
    ROW_I(GL_UNPACK_ALIGNMENT, 1),
    ROW_B(GL_UNPACK_LSB_FIRST, 1),
    ROW_I(GL_UNPACK_ROW_LENGTH, 1),
    ROW_I(GL_UNPACK_SKIP_PIXELS, 1),
    ROW_I(GL_UNPACK_SKIP_ROWS, 1),
    ROW_B(GL_UNPACK_SWAP_BYTES, 1),
    ROW_F(GL_ZOOM_X, 1),
    ROW_F(GL_ZOOM_Y, 1)
};

static const StateRow k_rows_hints[] = {
    ROW_E(GL_FOG_HINT, 1, k_dom_hint),
    ROW_E(GL_LINE_SMOOTH_HINT, 1, k_dom_hint),
    ROW_E(GL_PERSPECTIVE_CORRECTION_HINT, 1, k_dom_hint),
    ROW_E(GL_POINT_SMOOTH_HINT, 1, k_dom_hint),
    ROW_E(GL_POLYGON_SMOOTH_HINT, 1, k_dom_hint)
};

static const StateRow k_rows_misc[] = {
    ROW_I(GL_ATTRIB_STACK_DEPTH, 1),
    ROW_I(GL_CLIENT_ATTRIB_STACK_DEPTH, 1),
    ROW_I(GL_LIST_BASE, 1),
    ROW_I(GL_LIST_INDEX, 1),
    ROW_E(GL_LIST_MODE, 1, k_dom_list_mode),
    ROW_I(GL_NAME_STACK_DEPTH, 1),
    ROW_E(GL_RENDER_MODE, 1, k_dom_render_mode)
};

/* Not GL 1.1 state: the drawable's configuration and the driver's identity.
 * Kept because the first question a surprising diff raises is "same context?"
 * - and because multisampling silently changes what smoothing does. */
static const StateRow k_rows_context[] = {
    ROW_I(GL_ALPHA_BITS, 1),
    ROW_I(GL_BLUE_BITS, 1),
    ROW_I(GL_DEPTH_BITS, 1),
    ROW_I(GL_GREEN_BITS, 1),
    ROW_I(GL_RED_BITS, 1),
    ROW_I(GL_STENCIL_BITS, 1),
    ROW_B(GL_DOUBLEBUFFER, 1),
    ROW_B(GL_STEREO, 1)
};

/* ========================================================================= */
/* Value formatting                                                           */
/* ========================================================================= */

#define VALUE_CAP GL_STATE_DUMP_VALUE_MAX

/* Append one field to `buf`, comma-separated. Truncation is silent but
 * marked: a value that did not fit ends in "...", so a diff shows a
 * too-narrow buffer rather than two rows that quietly agree. */
static void append_field(char *buf, size_t *used, const char *text) {
    int wrote;
    size_t left = VALUE_CAP - *used;
    if (left <= 1) return;
    wrote = snprintf(buf + *used, left, "%s%s", *used ? "," : "", text);
    if (wrote < 0 || (size_t)wrote >= left) {
        size_t tail = VALUE_CAP > 4 ? VALUE_CAP - 4 : 0;
        memcpy(buf + tail, "...", 4);
        *used = VALUE_CAP - 1;
        return;
    }
    *used += (size_t)wrote;
}

static void format_row(const StateRow *row, char *buf) {
    char field[64];
    char scratch[32];
    size_t used = 0;
    int i;

    buf[0] = '\0';

    switch (row->kind) {
    case ROW_ENABLE:
        snprintf(buf, VALUE_CAP, "%d", glIsEnabled(row->pname) ? 1 : 0);
        return;
    case ROW_BOOL: {
        GLboolean values[16];
        memset(values, 0, sizeof values);
        glGetBooleanv(row->pname, values);
        for (i = 0; i < row->count; i++) {
            snprintf(field, sizeof field, "%d", values[i] ? 1 : 0);
            append_field(buf, &used, field);
        }
        return;
    }
    case ROW_INT: {
        GLint values[16];
        memset(values, 0, sizeof values);
        glGetIntegerv(row->pname, values);
        for (i = 0; i < row->count; i++) {
            snprintf(field, sizeof field, "%d", (int)values[i]);
            append_field(buf, &used, field);
        }
        return;
    }
    case ROW_HEX: {
        GLint value = 0;
        glGetIntegerv(row->pname, &value);
        snprintf(buf, VALUE_CAP, "0x%08X", (unsigned)value);
        return;
    }
    case ROW_ENUM: {
        GLint values[16];
        memset(values, 0, sizeof values);
        glGetIntegerv(row->pname, values);
        for (i = 0; i < row->count; i++)
            append_field(buf, &used,
                         enum_name(row->domain, values[i], scratch, sizeof scratch));
        return;
    }
    case ROW_FLOAT: {
        GLfloat values[16];
        memset(values, 0, sizeof values);
        glGetFloatv(row->pname, values);
        for (i = 0; i < row->count; i++) {
            snprintf(field, sizeof field, "%.4f", (double)values[i]);
            append_field(buf, &used, field);
        }
        return;
    }
    }
}

/* ========================================================================= */
/* Walk                                                                       */
/* ========================================================================= */

typedef struct {
    GlStateDumpEmitFn emit;
    void             *user_data;
} Sink;

static void emit_section(const Sink *sink, const char *heading) {
    sink->emit(sink->user_data, NULL, heading);
}

static void emit_row(const Sink *sink, const char *name, const char *value) {
    sink->emit(sink->user_data, name, value);
}

static void emit_rows(const Sink *sink, const StateRow *rows, int count) {
    char value[VALUE_CAP];
    int i;
    for (i = 0; i < count; i++) {
        format_row(&rows[i], value);
        emit_row(sink, rows[i].name, value);
    }
}

#define ROW_COUNT(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

/* Scoped name: "GL_LIGHT0.GL_DIFFUSE". The dot keeps the owning object and
 * the parameter both greppable without inventing a second syntax. */
static void emit_scoped_floats(const Sink *sink, const char *scope,
                               const char *param, const GLfloat *values, int count) {
    char name[GL_STATE_DUMP_NAME_MAX];
    char value[VALUE_CAP];
    char field[64];
    size_t used = 0;
    int i;

    value[0] = '\0';
    for (i = 0; i < count; i++) {
        snprintf(field, sizeof field, "%.4f", (double)values[i]);
        append_field(value, &used, field);
    }
    snprintf(name, sizeof name, "%s.%s", scope, param);
    emit_row(sink, name, value);
}

static void emit_lights(const Sink *sink) {
    static const struct { GLenum pname; const char *name; int count; } k_params[] = {
        { GL_AMBIENT,               "GL_AMBIENT",               4 },
        { GL_CONSTANT_ATTENUATION,  "GL_CONSTANT_ATTENUATION",  1 },
        { GL_DIFFUSE,               "GL_DIFFUSE",               4 },
        { GL_LINEAR_ATTENUATION,    "GL_LINEAR_ATTENUATION",    1 },
        { GL_POSITION,              "GL_POSITION",              4 },
        { GL_QUADRATIC_ATTENUATION, "GL_QUADRATIC_ATTENUATION", 1 },
        { GL_SPECULAR,              "GL_SPECULAR",              4 },
        { GL_SPOT_CUTOFF,           "GL_SPOT_CUTOFF",           1 },
        { GL_SPOT_DIRECTION,        "GL_SPOT_DIRECTION",        3 },
        { GL_SPOT_EXPONENT,         "GL_SPOT_EXPONENT",         1 }
    };
    char scope[GL_STATE_DUMP_NAME_MAX];
    int light, i;

    for (light = 0; light < 8; light++) {
        snprintf(scope, sizeof scope, "GL_LIGHT%d", light);
        for (i = 0; i < ROW_COUNT(k_params); i++) {
            GLfloat values[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            glGetLightfv((GLenum)(GL_LIGHT0 + light), k_params[i].pname, values);
            emit_scoped_floats(sink, scope, k_params[i].name, values,
                               k_params[i].count);
        }
    }
}

static void emit_materials(const Sink *sink) {
    static const struct { GLenum pname; const char *name; int count; } k_params[] = {
        { GL_AMBIENT,   "GL_AMBIENT",   4 },
        { GL_DIFFUSE,   "GL_DIFFUSE",   4 },
        { GL_EMISSION,  "GL_EMISSION",  4 },
        { GL_SHININESS, "GL_SHININESS", 1 },
        { GL_SPECULAR,  "GL_SPECULAR",  4 }
    };
    static const struct { GLenum face; const char *name; } k_faces[] = {
        { GL_FRONT, "GL_FRONT" },
        { GL_BACK,  "GL_BACK"  }
    };
    int face, i;

    for (face = 0; face < ROW_COUNT(k_faces); face++) {
        for (i = 0; i < ROW_COUNT(k_params); i++) {
            GLfloat values[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            glGetMaterialfv(k_faces[face].face, k_params[i].pname, values);
            emit_scoped_floats(sink, k_faces[face].name, k_params[i].name, values,
                               k_params[i].count);
        }
    }
}

static void emit_clip_planes(const Sink *sink) {
    char name[GL_STATE_DUMP_NAME_MAX];
    char value[VALUE_CAP];
    int plane;

    for (plane = 0; plane < 6; plane++) {
        GLdouble eq[4] = { 0.0, 0.0, 0.0, 0.0 };
        char field[64];
        size_t used = 0;
        int i;
        glGetClipPlane((GLenum)(GL_CLIP_PLANE0 + plane), eq);
        value[0] = '\0';
        for (i = 0; i < 4; i++) {
            snprintf(field, sizeof field, "%.4f", eq[i]);
            append_field(value, &used, field);
        }
        snprintf(name, sizeof name, "GL_CLIP_PLANE%d.equation", plane);
        emit_row(sink, name, value);
    }
}

static void emit_texture_unit(const Sink *sink) {
    static const struct { GLenum pname; const char *name; const GlEnumName *domain; }
    k_int_params[] = {
        { GL_TEXTURE_MAG_FILTER, "GL_TEXTURE_MAG_FILTER", k_dom_tex_filter },
        { GL_TEXTURE_MIN_FILTER, "GL_TEXTURE_MIN_FILTER", k_dom_tex_filter },
        { GL_TEXTURE_WRAP_S,     "GL_TEXTURE_WRAP_S",     k_dom_tex_wrap   },
        { GL_TEXTURE_WRAP_T,     "GL_TEXTURE_WRAP_T",     k_dom_tex_wrap   }
    };
    static const struct { GLenum coord; const char *name; } k_coords[] = {
        { GL_S, "GL_TEXTURE_GEN_S" },
        { GL_T, "GL_TEXTURE_GEN_T" },
        { GL_R, "GL_TEXTURE_GEN_R" },
        { GL_Q, "GL_TEXTURE_GEN_Q" }
    };
    char name[GL_STATE_DUMP_NAME_MAX];
    char value[VALUE_CAP];
    char scratch[32];
    GLfloat colors[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    GLfloat priority = 0.0f;
    GLint mode = 0;
    int i;

    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &mode);
    snprintf(value, VALUE_CAP, "%s",
             enum_name(k_dom_tex_env_mode, mode, scratch, sizeof scratch));
    emit_row(sink, "GL_TEXTURE_ENV_MODE", value);
    glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, colors);
    emit_scoped_floats(sink, "GL_TEXTURE_ENV", "GL_TEXTURE_ENV_COLOR", colors, 4);

    /* Parameters of whatever is bound to GL_TEXTURE_2D - the only target the
     * sample ever binds. A 1D/3D twin would be the same four queries. */
    for (i = 0; i < ROW_COUNT(k_int_params); i++) {
        GLint param = 0;
        glGetTexParameteriv(GL_TEXTURE_2D, k_int_params[i].pname, &param);
        snprintf(name, sizeof name, "GL_TEXTURE_2D.%s", k_int_params[i].name);
        snprintf(value, VALUE_CAP, "%s",
                 enum_name(k_int_params[i].domain, param, scratch, sizeof scratch));
        emit_row(sink, name, value);
    }
    glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, colors);
    emit_scoped_floats(sink, "GL_TEXTURE_2D", "GL_TEXTURE_BORDER_COLOR", colors, 4);
    glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_PRIORITY, &priority);
    emit_scoped_floats(sink, "GL_TEXTURE_2D", "GL_TEXTURE_PRIORITY", &priority, 1);

    for (i = 0; i < ROW_COUNT(k_coords); i++) {
        GLfloat plane[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        GLint gen_mode = 0;
        glGetTexGeniv(k_coords[i].coord, GL_TEXTURE_GEN_MODE, &gen_mode);
        snprintf(name, sizeof name, "%s.GL_TEXTURE_GEN_MODE", k_coords[i].name);
        snprintf(value, VALUE_CAP, "%s",
                 enum_name(k_dom_texgen_mode, gen_mode, scratch, sizeof scratch));
        emit_row(sink, name, value);
        glGetTexGenfv(k_coords[i].coord, GL_OBJECT_PLANE, plane);
        emit_scoped_floats(sink, k_coords[i].name, "GL_OBJECT_PLANE", plane, 4);
        glGetTexGenfv(k_coords[i].coord, GL_EYE_PLANE, plane);
        emit_scoped_floats(sink, k_coords[i].name, "GL_EYE_PLANE", plane, 4);
    }
}

/* The 32x32 stipple is 128 bytes - too wide to read and too noisy to diff as
 * itself, but its identity still matters, so it is reported as a checksum
 * plus a set-bit count. Equal checksums mean equal patterns for any pattern
 * that matters; the bit count makes "all on" / "all off" readable at a
 * glance. */
static void emit_polygon_stipple(const Sink *sink) {
    GLubyte pattern[128];
    char value[VALUE_CAP];
    unsigned long checksum = 5381UL;
    int bits = 0;
    int i;

    memset(pattern, 0, sizeof pattern);
    glGetPolygonStipple(pattern);
    for (i = 0; i < (int)sizeof pattern; i++) {
        unsigned byte = pattern[i];
        int bit;
        checksum = ((checksum << 5) + checksum) + byte;
        for (bit = 0; bit < 8; bit++)
            if (byte & (1u << bit)) bits++;
    }
    snprintf(value, VALUE_CAP, "0x%08lX", checksum & 0xFFFFFFFFUL);
    emit_row(sink, "GL_POLYGON_STIPPLE.checksum", value);
    snprintf(value, VALUE_CAP, "%d", bits);
    emit_row(sink, "GL_POLYGON_STIPPLE.set_bits", value);
}

static void emit_strings(const Sink *sink) {
    static const struct { GLenum name; const char *label; } k_strings[] = {
        { GL_RENDERER, "GL_RENDERER" },
        { GL_VENDOR,   "GL_VENDOR"   },
        { GL_VERSION,  "GL_VERSION"  }
    };
    char value[VALUE_CAP];
    int i;

    for (i = 0; i < ROW_COUNT(k_strings); i++) {
        const GLubyte *text = glGetString(k_strings[i].name);
        snprintf(value, VALUE_CAP, "%s", text ? (const char *)text : "(null)");
        emit_row(sink, k_strings[i].label, value);
    }
}

/* Multisampling is GL 1.3, but it decides whether GL_LINE_SMOOTH /
 * GL_POLYGON_SMOOTH do anything at all, so a smoothing diff is unreadable
 * without it. Guarded: a header without these enums just omits the rows. */
static void emit_multisample(const Sink *sink) {
#if defined(GL_SAMPLE_BUFFERS) && defined(GL_SAMPLES) && defined(GL_MULTISAMPLE)
    char value[VALUE_CAP];
    GLint samples = 0, buffers = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    glGetIntegerv(GL_SAMPLE_BUFFERS, &buffers);
    snprintf(value, VALUE_CAP, "%d", glIsEnabled(GL_MULTISAMPLE) ? 1 : 0);
    emit_row(sink, "GL_MULTISAMPLE", value);
    snprintf(value, VALUE_CAP, "%d", (int)buffers);
    emit_row(sink, "GL_SAMPLE_BUFFERS", value);
    snprintf(value, VALUE_CAP, "%d", (int)samples);
    emit_row(sink, "GL_SAMPLES", value);
#else
    (void)sink;
#endif
}

void gl_state_dump_walk(GlStateDumpEmitFn emit, void *user_data) {
    Sink sink;
    char value[VALUE_CAP];
    GLenum entry_error;

    if (!emit) return;
    sink.emit = emit;
    sink.user_data = user_data;

    entry_error = glGetError();

    emit_section(&sink, "--- capabilities ---");
    emit_rows(&sink, k_rows_capabilities, ROW_COUNT(k_rows_capabilities));

    emit_section(&sink, "--- current values ---");
    emit_rows(&sink, k_rows_current, ROW_COUNT(k_rows_current));

    emit_section(&sink, "--- transformation ---");
    emit_rows(&sink, k_rows_transform, ROW_COUNT(k_rows_transform));
    emit_clip_planes(&sink);

    emit_section(&sink, "--- lighting ---");
    emit_rows(&sink, k_rows_lighting, ROW_COUNT(k_rows_lighting));
    emit_lights(&sink);
    emit_materials(&sink);

    emit_section(&sink, "--- rasterization ---");
    emit_rows(&sink, k_rows_rasterization, ROW_COUNT(k_rows_rasterization));
    emit_polygon_stipple(&sink);

    emit_section(&sink, "--- fog ---");
    emit_rows(&sink, k_rows_fog, ROW_COUNT(k_rows_fog));

    emit_section(&sink, "--- texturing ---");
    emit_rows(&sink, k_rows_texture_env, ROW_COUNT(k_rows_texture_env));
    emit_texture_unit(&sink);

    emit_section(&sink, "--- pixel operations ---");
    emit_rows(&sink, k_rows_pixel_ops, ROW_COUNT(k_rows_pixel_ops));

    emit_section(&sink, "--- framebuffer control ---");
    emit_rows(&sink, k_rows_framebuffer, ROW_COUNT(k_rows_framebuffer));

    emit_section(&sink, "--- pixel store / transfer ---");
    emit_rows(&sink, k_rows_pixel_store, ROW_COUNT(k_rows_pixel_store));

    emit_section(&sink, "--- hints ---");
    emit_rows(&sink, k_rows_hints, ROW_COUNT(k_rows_hints));

    emit_section(&sink, "--- misc ---");
    emit_rows(&sink, k_rows_misc, ROW_COUNT(k_rows_misc));

    emit_section(&sink, "--- context (not GL 1.1 state) ---");
    emit_rows(&sink, k_rows_context, ROW_COUNT(k_rows_context));
    emit_multisample(&sink);
    emit_strings(&sink);

    snprintf(value, VALUE_CAP, "0x%04X", (unsigned)entry_error);
    emit_row(&sink, "GL_ERROR_ON_ENTRY", value);
    /* Swallow anything the queries themselves raised: a dump must not change
     * the error state the caller is about to inspect. */
    (void)glGetError();
}

#endif /* GL_STATE_DUMP_QUERIES */
