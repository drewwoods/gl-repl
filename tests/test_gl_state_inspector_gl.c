/*
 * test_gl_state_inspector_gl.c - differential: the state fold vs a real driver.
 *
 * Why this exists
 * ---------------
 * gl_state_inspector.c answers "what OpenGL state does this program hold at
 * this line?" without issuing a single GL call — it re-implements the parts of
 * the GL state machine the REPL can reach (including 4x4 matrix composition and
 * glPushAttrib group semantics). Everything else that tests it compares the
 * fold against *itself*: the pure sweep in test_repl_state.c drives apply and
 * report and checks they agree, and test_ui_gl_state.c checks a driver against
 * initial values hand-copied into the test. Neither can catch the fold
 * modelling GL wrongly.
 *
 * This test closes that: one GLCmd program drives two sides —
 *
 *   driver side: the real executor (repl_execute_program), the same walk the
 *                live frame uses, against a real GL context;
 *   fold side:   repl_gl_state_report_at_line() over the same commands;
 *
 * then each compared row is read back with glGet* and matched against what the
 * report claims. Both sides start from the generated init() + display() writes
 * enumerated by repl_generated_*_state_write_at(), so the baselines agree; that
 * the enumeration matches the setup the app actually runs is a separate
 * property, covered by the export-trace parity test.
 *
 * Rows deliberately NOT compared, because the report says in its own name that
 * it is not quoting a GL value:
 *   - "GL_CURRENT_RASTER_POSITION (object input)" — GL stores window
 *     coordinates; the fold reports the untransformed input.
 *   - "GL_LIGHTn_POSITION (world)" — derived by inverting the modelview.
 * Their exact-value twins (the "(eye)" light position) are compared.
 *
 * GL_CURRENT_RASTER_COLOR *is* compared, including under GL_LIGHTING: the fold
 * evaluates the lighting equation for that cell (gl_state_lit_color), so the
 * lighting cases are the only check that it evaluates it the way a driver does.
 *
 * "GL_CLIP_PLANEn_EQUATION (object)" is the third qualified row and the one
 * case where the difference itself is checkable: GL stores the equation
 * transformed into eye coordinates at call time, so the row is compared through
 * that transform rather than for equality. See diff_clip_plane_row.
 *
 * In GL_TEST_BINS / `make gl-tests` (needs a real context; a display, or
 * FREEGLUT_OSMESA=1 headless) — NOT in `make test` / `make test-stubs`.
 */
#include "gl_includes.h"
#include "repl/gl_state_inspector.h"
#include "repl/init_state.h"
#include "repl/executor.h"
#include "repl/command_spec.h"
#include "repl/export.h"   /* light bridge: the generated setup's light values */
#include "support/test_harness.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) && !defined(FREEGLUT_OSMESA)
#include <ApplicationServices/ApplicationServices.h>
#endif

static TestHarness g_harness = TEST_HARNESS_INIT;
#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)

/* The report prints matrices at four decimals, so matrix rows can only be
 * compared to that precision; other cells round-trip exactly. */
#define DIFF_EPSILON        1e-4f
#define DIFF_MATRIX_EPSILON 1e-3f

/* The fold under test, rebuilt per case. */
static ReplGlStateReport g_report;

static const ReplGlStateReportRow *diff_row(const char *name) {
    int i;
    for (i = 0; i < g_report.count; i++)
        if (strcmp(g_report.rows[i].name, name) == 0)
            return &g_report.rows[i];
    return NULL;
}

/* Every comparator funnels through this so a missing row fails loudly instead
 * of silently passing as "nothing to compare". */
static const ReplGlStateReportRow *diff_require_row(const char *name) {
    const ReplGlStateReportRow *row = diff_row(name);
    char label[192];
    snprintf(label, sizeof(label), "%s: reported at all", name);
    ASSERT_TRUE(label, row != NULL);
    return row;
}

static int feq(float a, float b, float epsilon) {
    float d = a - b;
    return (d < 0 ? -d : d) <= epsilon;
}

/* --- driver-specific known deviations ------------------------------------ */

/* Apple's legacy GL deviates on GL_CURRENT_RASTER_COLOR in two ways, both
 * measured against Mesa 25.2.8 (Intel ADL-N), which agrees with this fold:
 *
 *   1. When GL_COLOR_MATERIAL is enabled at the glRasterPos call, the material
 *      components it tracks are lit as ZERO. Isolated by sweeping the state
 *      around the call: with GL_AMBIENT_AND_DIFFUSE both terms vanish and the
 *      cell reads (0,0,0,1); with GL_DIFFUSE alone only the diffuse term goes,
 *      leaving exactly the ambient sum (0.060, 0.064, 0.068). It does not depend
 *      on the face, nor on whether a glColor was ever issued (setting the
 *      material with glMaterialfv while the cap is on zeroes it just the same),
 *      and ordinary *vertex* lighting under identical state is correct — a
 *      GL_3D_COLOR feedback vertex comes back at the value the equation calls
 *      for. So the fault is confined to the raster-position path, and
 *      glDisable(GL_COLOR_MATERIAL) immediately before glRasterPos is a
 *      complete workaround: the cell then matches the vertex exactly.
 *      There is no portable assertion to make, so that case is skipped here and
 *      says so out loud rather than being quietly dropped.
 *   2. The latched color is clamped to [0, 1]. Mesa stores what was latched,
 *      unclamped. GL 2.1 does not settle this one, so the fold follows Mesa
 *      (which also keeps the row consistent with the panel's GL_CURRENT_COLOR)
 *      and the case accepts either answer, reporting which arrived. */
static int diff_driver_is_apple_legacy(void) {
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    return vendor && strstr(vendor, "Apple") != NULL;
}

/* Mesa deviates on the same cell in two *different* places — measured on Mesa
 * 25.2.8 / Intel ADL-N against Apple's legacy GL, which agrees with this fold
 * and with the GL 2.1 text on both. Each was pinned by solving for the model
 * that reproduces the driver's numbers exactly, not by inspection:
 *
 *   1. The raster position is lit from its *object-space* position, so the
 *      vector to a positional light is wrong whenever the modelview at the call
 *      is not the identity (the normal is transformed correctly). Measured
 *      n·L = 0.897580, which is what the untransformed position gives; the
 *      eye-space position gives 0.743409, the value Apple stores.
 *   2. GL_NORMALIZE is ignored for that path: under a scale(4) modelview Mesa
 *      lights the raw quarter-length inverse-transposed normal and stores
 *      (0.212361, 0.180281, 0.158201) where normalizing first gives
 *      (0.699445, 0.559124, 0.428803).
 *
 * Cases resting on either are skipped there. Both deviations need a non-identity
 * modelview, which is why the directional-light and local-viewer cases below
 * pass on Mesa unchanged: with w = 0 the vertex position drops out of the light
 * vector, and those cases leave the modelview alone. */
static int diff_driver_is_mesa(void) {
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    return (renderer && strstr(renderer, "Mesa") != NULL) ||
           (version && strstr(version, "Mesa") != NULL);
}

/* Skips print, so a driver-specific gap can never pass for coverage. */
static void diff_skip(const char *what, const char *why) {
    printf("    (skipped: %s — %s)\n", what, why);
}

/* --- generated-setup light values ---------------------------------------- */

/* With no light bridge installed the generated setup emits zeroed lights, so
 * every diffuse and specular term below would multiply by zero and the lighting
 * cases would "pass" while testing nothing. These values give the terms weight.
 * Both sides read them through the same enumerator, so the two baselines shift
 * together. Slot 0 is positional (w = 1) and slot 1 directional (w = 0), which
 * are the two branches of the light-vector computation. Positions are flagged
 * eye-space so they are emitted at identity modelview and stored verbatim. */
static void diff_fill_light_slot(int slot, ReplExportLightInfo *out) {
    memset(out, 0, sizeof(*out));
    out->pos_is_eye_space = 1;
    if (slot == 0) {
        out->pos[0] = 0.4f; out->pos[1] = 0.8f;
        out->pos[2] = 1.2f; out->pos[3] = 1.0f;
        out->ambient[0] = 0.10f; out->ambient[1] = 0.12f;
        out->ambient[2] = 0.14f; out->ambient[3] = 1.0f;
        out->diffuse[0] = 0.90f; out->diffuse[1] = 0.70f;
        out->diffuse[2] = 0.50f; out->diffuse[3] = 1.0f;
        out->specular[0] = 0.80f; out->specular[1] = 0.85f;
        out->specular[2] = 0.90f; out->specular[3] = 1.0f;
    } else if (slot == 1) {
        out->pos[0] = 0.0f; out->pos[1] = 0.6f;
        out->pos[2] = 0.8f; out->pos[3] = 0.0f;
        out->ambient[0] = 0.05f; out->ambient[1] = 0.05f;
        out->ambient[2] = 0.05f; out->ambient[3] = 1.0f;
        out->diffuse[0] = 0.30f; out->diffuse[1] = 0.40f;
        out->diffuse[2] = 0.50f; out->diffuse[3] = 1.0f;
        out->specular[0] = 0.25f; out->specular[1] = 0.25f;
        out->specular[2] = 0.25f; out->specular[3] = 1.0f;
    }
}

static const ReplExportLightBridge k_diff_light_bridge = {
    diff_fill_light_slot
};

/* --- building test programs --------------------------------------------- */

static GLCmd diff_cmd(CmdType type, int source_line_idx, int num_args, ...) {
    GLCmd cmd;
    va_list ap;
    int i;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = type;
    cmd.valid = 1;
    cmd.src_cmd_idx = source_line_idx;
    cmd.call_src_cmd_idx = -1;
    cmd.root_call_src_cmd_idx = -1;
    cmd.num_args = num_args;
    va_start(ap, num_args);
    for (i = 0; i < num_args && i < 8; i++)
        cmd.args[i] = (float)va_arg(ap, double);
    va_end(ap);
    return cmd;
}

/* --- the two sides ------------------------------------------------------ */

/* Driver side: the production executor walk, not a second model of it. */
static void diff_exec(const GLCmd *cmds, int count) {
    ReplExecutionOptions options;
    memset(&options, 0, sizeof(options));
    options.program.cmds = cmds;
    options.program.cmd_count = count;
    options.flat_cmd_count = count;
    repl_execute_program(&options);
}

/* A case's program, copied so the deferred cursor below can never point at a
 * caller's expired local array. */
#define DIFF_MAX_CMDS 32
static GLCmd g_case_cmds[DIFF_MAX_CMDS];

/* The executor walk for the case under comparison, left open on purpose.
 * repl_exec_cursor_end() balances whatever the prefix left open — an unmatched
 * glPushMatrix or glPushAttrib — because a frame must never leak stack depth.
 * That balancing is the opposite of what a checkpoint *inside* a push is
 * asking about, so the walk is stepped to the checkpoint, the rows are
 * compared while it is still open, and the next case (or main) closes it. */
static ReplExecCursor g_cursor;
static int g_cursor_open;

static void diff_close_cursor(void) {
    if (!g_cursor_open)
        return;
    repl_exec_cursor_end(&g_cursor);
    g_cursor_open = 0;
}

static void diff_step_to_checkpoint(int count) {
    ReplExecutionOptions options;
    memset(&options, 0, sizeof(options));
    options.program.cmds = g_case_cmds;
    options.program.cmd_count = count;
    options.flat_cmd_count = count;
    g_cursor = repl_exec_cursor_begin(&options);
    g_cursor_open = 1;
    while (repl_exec_cursor_step(&g_cursor)) {
    }
}

/* Apply one generated setup write to GL. The three non-command kinds are the
 * fixed-function calls that have no REPL command form; the inspector folds them
 * in gl_state_apply_generated_write(), and this is their GL twin. */
static void diff_apply_generated_write(const ReplGeneratedStateWrite *write) {
    switch (write->kind) {
    case REPL_GENERATED_STATE_COMMAND:
        diff_exec(&write->command, 1);
        break;
    case REPL_GENERATED_STATE_LIGHT_FV:
        glLightfv(write->object, write->pname, write->value);
        break;
    case REPL_GENERATED_STATE_LIGHT_MODEL_FV:
        glLightModelfv(write->pname, write->value);
        break;
    case REPL_GENERATED_STATE_PUSH_ATTRIB:
        /* The generated display bracket saves everything (the exported C spells
         * it glPushAttrib(GL_ALL_ATTRIB_BITS)); the fold tracks only its
         * depth. */
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        break;
    }
}

static void diff_apply_generated_phase(int display_phase) {
    int count = display_phase ? repl_generated_display_state_write_count()
                              : repl_generated_init_state_write_count();
    int i;
    for (i = 0; i < count; i++) {
        ReplGeneratedStateWrite write;
        int ok = display_phase
                     ? repl_generated_display_state_write_at(i, &write)
                     : repl_generated_init_state_write_at(i, &write);
        if (ok)
            diff_apply_generated_write(&write);
    }
}

/* Return GL to the state the previous case started from: popping the generated
 * display bracket restores every attribute group the case touched inside it,
 * and the matrix stack is unwound by hand (matrices are not attribute state).
 * State the init() phase wrote survives, exactly as it does in the app — and
 * every case re-applies those same writes anyway. */
static void diff_reset_gl(void) {
    GLint attrib_depth = 0;
    GLint matrix_depth = 0;
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &attrib_depth);
    while (attrib_depth-- > 0)
        glPopAttrib();
    glMatrixMode(GL_MODELVIEW);
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &matrix_depth);
    while (matrix_depth-- > 1)
        glPopMatrix();
    glLoadIdentity();
}

/* Run one case: reset, replay the generated prologue into GL, execute the
 * program up to the checkpoint, and fold the same program to `checkpoint`.
 * Rows are compared by the caller.
 *
 * The driver must see exactly the commands the fold folds, or a mid-program
 * checkpoint compares two different program prefixes. Every case here puts one
 * command per source line in order, so the fold's "anchor < checkpoint" rule
 * (gl_state_command_precedes) is the leading `checkpoint` commands. */
static void diff_case(const char *name, const GLCmd *cmds, int count,
                      int checkpoint) {
    FlatProgramView program;
    int exec_count = 0;

    printf("--- %s ---\n", name);
    diff_close_cursor();
    diff_reset_gl();
    diff_apply_generated_phase(0);
    diff_apply_generated_phase(1);

    if (count > DIFF_MAX_CMDS)
        count = DIFF_MAX_CMDS;
    if (count > 0)
        memcpy(g_case_cmds, cmds, (size_t)count * sizeof(g_case_cmds[0]));
    while (exec_count < count && g_case_cmds[exec_count].src_cmd_idx < checkpoint)
        exec_count++;
    diff_step_to_checkpoint(exec_count);

    memset(&program, 0, sizeof(program));
    program.cmds = g_case_cmds;
    program.cmd_count = count;
    repl_gl_state_report_at_line(program, checkpoint, &g_report);
}

/* --- comparators -------------------------------------------------------- */

/* Resolve an enum token back to its value through the same spec tables the
 * report formatted it with, so the comparison is value against value: the
 * report's claim is "GL holds this enum", and the token is how it spells it.
 * Handles the report's "0x%X" fallback for a value with no token. */
static int diff_enum_text_value(const char *text, GLenum *out) {
    const ReplEnumCommandSpec *spec = repl_enum_command_specs();
    if (!text || !*text)
        return 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        *out = (GLenum)strtoul(text, NULL, 16);
        return 1;
    }
    for (; spec && spec->name; spec++) {
        int slot;
        for (slot = 0; slot < MAX_ENUM_ARGS; slot++) {
            const ReplEnumEntry *entry = spec->args[slot].enums;
            int i;
            for (i = 0; entry && entry[i].name; i++) {
                if (strcmp(entry[i].name, text) == 0) {
                    *out = entry[i].value;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* "(a, b, c, d)" -> floats. Returns the count parsed. */
static int diff_parse_vec(const char *text, float *out, int max_count) {
    const char *p = text;
    int n = 0;
    if (!p || *p != '(')
        return 0;
    p++;
    while (n < max_count) {
        char *end = NULL;
        double value = strtod(p, &end);
        if (end == p)
            break;
        out[n++] = (float)value;
        p = end;
        while (*p == ',' || *p == ' ')
            p++;
        if (*p == ')')
            break;
    }
    return n;
}

static void diff_vec_row(const char *name, GLenum pname, int count) {
    const ReplGlStateReportRow *row = diff_require_row(name);
    GLfloat driver[4];
    float reported[4];
    char label[192];
    int parsed, i;

    if (!row)
        return;
    glGetFloatv(pname, driver);
    parsed = diff_parse_vec(row->current, reported, count);
    snprintf(label, sizeof(label), "%s: %d components parsed", name, count);
    ASSERT_TRUE(label, parsed == count);
    for (i = 0; i < parsed && i < count; i++) {
        snprintf(label, sizeof(label), "%s[%d]: fold %g == driver %g", name, i,
                 (double)reported[i], (double)driver[i]);
        ASSERT_TRUE(label, feq(reported[i], driver[i], DIFF_EPSILON));
    }
}

static void diff_float_row(const char *name, GLenum pname) {
    const ReplGlStateReportRow *row = diff_require_row(name);
    GLfloat driver = 0.0f;
    char label[192];
    if (!row)
        return;
    glGetFloatv(pname, &driver);
    snprintf(label, sizeof(label), "%s: fold %s == driver %g", name,
             row->current, (double)driver);
    ASSERT_TRUE(label, feq((float)atof(row->current), driver, DIFF_EPSILON));
}

static void diff_int_row(const char *name, GLenum pname) {
    const ReplGlStateReportRow *row = diff_require_row(name);
    GLint driver = 0;
    char label[192];
    if (!row)
        return;
    glGetIntegerv(pname, &driver);
    snprintf(label, sizeof(label), "%s: fold %s == driver %d", name,
             row->current, (int)driver);
    ASSERT_TRUE(label, atoi(row->current) == (int)driver);
}

static void diff_enum_row(const char *name, GLenum pname) {
    const ReplGlStateReportRow *row = diff_require_row(name);
    GLenum reported = 0;
    GLint driver = 0;
    char label[192];
    if (!row)
        return;
    snprintf(label, sizeof(label), "%s: token \"%s\" resolves", name,
             row->current);
    ASSERT_TRUE(label, diff_enum_text_value(row->current, &reported));
    glGetIntegerv(pname, &driver);
    snprintf(label, sizeof(label), "%s: fold %s == driver 0x%X", name,
             row->current, (unsigned)driver);
    ASSERT_TRUE(label, reported == (GLenum)driver);
}

/* The report spells enable state GL_TRUE / GL_FALSE, so the same token
 * resolution works for caps read with glIsEnabled. */
static void diff_cap_row(const char *name, GLenum cap) {
    const ReplGlStateReportRow *row = diff_require_row(name);
    GLenum reported = 0;
    char label[192];
    if (!row)
        return;
    ASSERT_TRUE("cap token resolves", diff_enum_text_value(row->current,
                                                           &reported));
    snprintf(label, sizeof(label), "%s: fold %s == driver %s", name,
             row->current,
             glIsEnabled(cap) == GL_TRUE ? "GL_TRUE" : "GL_FALSE");
    ASSERT_TRUE(label, (reported == GL_TRUE) == (glIsEnabled(cap) == GL_TRUE));
}

static void diff_bool_row(const char *name, GLenum pname) {
    const ReplGlStateReportRow *row = diff_require_row(name);
    GLboolean driver[4];
    GLenum reported = 0;
    char label[192];
    if (!row)
        return;
    glGetBooleanv(pname, driver);
    ASSERT_TRUE("bool token resolves", diff_enum_text_value(row->current,
                                                            &reported));
    snprintf(label, sizeof(label), "%s: fold %s == driver %s", name,
             row->current, driver[0] == GL_TRUE ? "GL_TRUE" : "GL_FALSE");
    ASSERT_TRUE(label, (reported == GL_TRUE) == (driver[0] == GL_TRUE));
}

/* The report narrows stencil masks to the REPL's 8-bit surface
 * (repl/stencil_limits.h), so only the low byte is comparable. */
static void diff_stencil_mask_row(const char *name, GLenum pname) {
    const ReplGlStateReportRow *row = diff_require_row(name);
    GLint driver = 0;
    char label[192];
    if (!row)
        return;
    glGetIntegerv(pname, &driver);
    snprintf(label, sizeof(label), "%s: fold %s == driver 0x%02X (low byte)",
             name, row->current, (unsigned)driver & 0xFFu);
    ASSERT_TRUE(label, strtoul(row->current, NULL, 16) ==
                           ((unsigned long)driver & 0xFFul));
}

static void diff_color_mask_row(void) {
    const ReplGlStateReportRow *row =
        diff_require_row("GL_COLOR_WRITEMASK");
    GLboolean driver[4];
    char expected[32];
    char label[192];
    if (!row)
        return;
    glGetBooleanv(GL_COLOR_WRITEMASK, driver);
    snprintf(expected, sizeof(expected), "(%s, %s, %s, %s)",
             driver[0] ? "T" : "F", driver[1] ? "T" : "F",
             driver[2] ? "T" : "F", driver[3] ? "T" : "F");
    snprintf(label, sizeof(label), "GL_COLOR_WRITEMASK: fold %s == driver %s",
             row->current, expected);
    ASSERT_TRUE(label, strcmp(row->current, expected) == 0);
}

/* glPolygonMode reads back as one front/back pair, which the report splits
 * into two rows. */
static void diff_polygon_mode_rows(void) {
    static const char *const names[2] = {
        "GL_POLYGON_MODE (front)", "GL_POLYGON_MODE (back)"
    };
    GLint driver[2] = { 0, 0 };
    int i;
    glGetIntegerv(GL_POLYGON_MODE, driver);
    for (i = 0; i < 2; i++) {
        const ReplGlStateReportRow *row = diff_require_row(names[i]);
        GLenum reported = 0;
        char label[192];
        if (!row)
            continue;
        ASSERT_TRUE("polygon mode token resolves",
                    diff_enum_text_value(row->current, &reported));
        snprintf(label, sizeof(label), "%s: fold %s == driver 0x%X", names[i],
                 row->current, (unsigned)driver[i]);
        ASSERT_TRUE(label, reported == (GLenum)driver[i]);
    }
}

/* The matrix row prints four visual rows of "%8.4f", "; "-separated: visual
 * row r, column c is GL's column-major m[c * 4 + r]. */
static void diff_matrix_row(void) {
    const ReplGlStateReportRow *row = diff_require_row("GL_MODELVIEW_MATRIX");
    GLfloat driver[16];
    const char *p;
    int i;

    if (!row)
        return;
    glGetFloatv(GL_MODELVIEW_MATRIX, driver);
    p = row->current;
    ASSERT_TRUE("GL_MODELVIEW_MATRIX: opens with [", *p == '[');
    p++;
    for (i = 0; i < 16; i++) {
        int r = i / 4, c = i % 4;
        char *end = NULL;
        double value;
        char label[192];
        while (*p == ' ' || *p == ';')
            p++;
        value = strtod(p, &end);
        snprintf(label, sizeof(label), "GL_MODELVIEW_MATRIX[%d][%d]: parsed",
                 r, c);
        ASSERT_TRUE(label, end != p);
        if (end == p)
            return;
        p = end;
        snprintf(label, sizeof(label),
                 "GL_MODELVIEW_MATRIX[%d][%d]: fold %g == driver %g", r, c,
                 value, (double)driver[c * 4 + r]);
        ASSERT_TRUE(label, feq((float)value, driver[c * 4 + r],
                               DIFF_MATRIX_EPSILON));
    }
}

/* Clip planes are the one cell where the fold and GL deliberately hold
 * *different* numbers, so plain equality is the wrong comparison.
 *
 * glClipPlane transforms the equation into eye coordinates at call time: as a
 * row vector, p' = p M^-1 with M the modelview current at the call (GL 2.1
 * §2.12), and glGetClipPlane reads that stored eye-space plane back. The fold
 * keeps the object-space input it was handed — the row says "(object)" for
 * exactly this reason — because the modelview it would need is the one at the
 * call, which is the fold's own composed matrix rather than anything GL will
 * tell it.
 *
 * So the differential is the *relationship*: GL's stored plane must be the
 * fold's row carried through that transform. For a pure translation
 * M^-1 = T(-t), which collapses p M^-1 to
 *
 *     (a, b, c, d - (a*tx + b*ty + c*tz))
 *
 * — a closed form, so the test needs no general 4x4 inverse of its own to
 * check against (which would just be a second copy of the code under test).
 * An identity modelview reduces it to equality, which is the plain case. */
static void diff_clip_plane_row(int plane, float tx, float ty, float tz) {
    char name[REPL_GL_STATE_NAME_MAX];
    const ReplGlStateReportRow *row;
    GLdouble driver[4] = { 0, 0, 0, 0 };
    float object[4] = { 0, 0, 0, 0 };
    float expected[4];
    char label[192];
    int parsed, i;

    snprintf(name, sizeof(name), "GL_CLIP_PLANE%d_EQUATION (object)", plane);
    row = diff_require_row(name);
    if (!row)
        return;
    parsed = diff_parse_vec(row->current, object, 4);
    snprintf(label, sizeof(label), "%s: 4 components parsed", name);
    ASSERT_TRUE(label, parsed == 4);
    if (parsed != 4)
        return;

    glGetClipPlane((GLenum)(GL_CLIP_PLANE0 + plane), driver);
    expected[0] = object[0];
    expected[1] = object[1];
    expected[2] = object[2];
    expected[3] = object[3] -
                  (object[0] * tx + object[1] * ty + object[2] * tz);
    for (i = 0; i < 4; i++) {
        snprintf(label, sizeof(label),
                 "%s[%d]: fold %g through the call's modelview == driver %g",
                 name, i, (double)expected[i], driver[i]);
        ASSERT_TRUE(label, feq(expected[i], (float)driver[i], DIFF_EPSILON));
    }
}

/* GL leaves the current raster color and position *undefined* when the raster
 * position is clipped away (GL 2.1 2.13); both drivers tested leave the cell at
 * its (1,1,1,1) default instead of latching anything. The fold cannot know
 * that — it tracks the modelview but never the projection — so it reports the
 * value a valid position would have latched. Every lighting case therefore keeps
 * its point inside the clip volume, and this asserts it, so a case that later
 * drifts outside fails on the reason instead of on a baffling color mismatch. */
static void diff_require_valid_raster_pos(const char *case_name) {
    GLboolean valid = GL_FALSE;
    char label[192];
    glGetBooleanv(GL_CURRENT_RASTER_POSITION_VALID, &valid);
    snprintf(label, sizeof(label), "%s: raster position inside the clip volume",
             case_name);
    ASSERT_TRUE(label, valid == GL_TRUE);
}

/* The one row where the two drivers disagree (see diff_driver_is_apple_legacy):
 * accept the fold's value either verbatim or clamped, and report which the
 * driver chose. Pinning "one of exactly these two" still catches a third
 * behavior, which is what a regression here would look like. */
static void diff_raster_color_raw_or_clamped(void) {
    const ReplGlStateReportRow *row =
        diff_require_row("GL_CURRENT_RASTER_COLOR");
    GLfloat driver[4];
    float reported[4];
    int raw_match = 1, clamped_match = 1;
    int i;

    if (!row)
        return;
    if (diff_parse_vec(row->current, reported, 4) != 4) {
        ASSERT_TRUE("out-of-range raster color: 4 components parsed", 0);
        return;
    }
    glGetFloatv(GL_CURRENT_RASTER_COLOR, driver);
    for (i = 0; i < 4; i++) {
        float clamped = reported[i] < 0.0f ? 0.0f
                                           : (reported[i] > 1.0f ? 1.0f
                                                                 : reported[i]);
        if (!feq(reported[i], driver[i], DIFF_EPSILON))
            raw_match = 0;
        if (!feq(clamped, driver[i], DIFF_EPSILON))
            clamped_match = 0;
    }
    printf("    (out-of-range latch: driver stores it %s)\n",
           raw_match ? "unclamped, like the fold"
                     : (clamped_match ? "clamped to [0,1]" : "some third way"));
    ASSERT_TRUE("out-of-range raster color is stored raw or clamped, "
                "nothing else", raw_match || clamped_match);
}

/* Material rows, read back with glGetMaterialfv. Both drivers agree here, so
 * these need no gating: what the fold has to get right is that the material
 * tracks the current color from the moment GL_COLOR_MATERIAL is enabled — not
 * only on the glColor calls that follow it. */
static void diff_material_row(const char *name, GLenum face, GLenum pname,
                              int count) {
    const ReplGlStateReportRow *row = diff_require_row(name);
    GLfloat driver[4];
    float reported[4];
    char label[192];
    int i;

    if (!row)
        return;
    glGetMaterialfv(face, pname, driver);
    if (count == 1) {
        snprintf(label, sizeof(label), "%s: fold %s == driver %g", name,
                 row->current, (double)driver[0]);
        ASSERT_TRUE(label, feq((float)atof(row->current), driver[0],
                               DIFF_EPSILON));
        return;
    }
    if (diff_parse_vec(row->current, reported, count) != count) {
        snprintf(label, sizeof(label), "%s: %d components parsed", name, count);
        ASSERT_TRUE(label, 0);
        return;
    }
    for (i = 0; i < count; i++) {
        snprintf(label, sizeof(label), "%s[%d]: fold %g == driver %g", name, i,
                 (double)reported[i], (double)driver[i]);
        ASSERT_TRUE(label, feq(reported[i], driver[i], DIFF_EPSILON));
    }
}

/* --- cases -------------------------------------------------------------- */

/* 1. The generated prologue alone. Nothing user-authored, so every row here is
 * the fold's account of the setup the app runs before the program's first
 * line — the baseline every other case builds on. */
static void test_diff_generated_prologue(void) {
    diff_case("inspector vs driver: generated prologue", NULL, 0, 0);

    diff_float_row("GL_LINE_WIDTH", GL_LINE_WIDTH);
    diff_vec_row("GL_COLOR_CLEAR_VALUE", GL_COLOR_CLEAR_VALUE, 4);
    diff_cap_row("GL_BLEND", GL_BLEND);
    diff_enum_row("GL_BLEND_SRC", GL_BLEND_SRC);
    diff_enum_row("GL_BLEND_DST", GL_BLEND_DST);
    diff_vec_row("GL_LIGHT_MODEL_AMBIENT", GL_LIGHT_MODEL_AMBIENT, 4);
    diff_int_row("GL_ATTRIB_STACK_DEPTH", GL_ATTRIB_STACK_DEPTH);
    diff_matrix_row();
}

/* 2. A sweep of plain state writes: one row per cell, all read back from the
 * driver the executor just wrote to. */
static void test_diff_user_state_writes(void) {
    GLCmd cmds[13];
    int n = 0;

    cmds[n] = diff_cmd(CMD_COLOR4F, n, 4, 0.25, 0.5, 0.75, 0.5); n++;
    cmds[n] = diff_cmd(CMD_NORMAL3F, n, 3, 0.0, 1.0, 0.0); n++;
    cmds[n] = diff_cmd(CMD_DEPTH_FUNC, n, 1, (double)GL_GREATER); n++;
    cmds[n] = diff_cmd(CMD_SHADE_MODEL, n, 1, (double)GL_FLAT); n++;
    cmds[n] = diff_cmd(CMD_BLEND_FUNC, n, 2, (double)GL_ONE,
                       (double)GL_ONE_MINUS_SRC_COLOR); n++;
    cmds[n] = diff_cmd(CMD_FRONT_FACE, n, 1, (double)GL_CW); n++;
    cmds[n] = diff_cmd(CMD_POLYGON_MODE, n, 2, (double)GL_FRONT_AND_BACK,
                       (double)GL_LINE); n++;
    cmds[n] = diff_cmd(CMD_POLYGON_OFFSET, n, 2, 1.5, -2.0); n++;
    /* No camera-distance source is installed, so the executor's software
     * point-size fallback passes the literal size through unscaled. */
    cmds[n] = diff_cmd(CMD_POINT_SIZE, n, 1, 6.0); n++;
    cmds[n] = diff_cmd(CMD_STENCIL_FUNC, n, 3, (double)GL_EQUAL, 3.0, 15.0);
    n++;
    cmds[n] = diff_cmd(CMD_STENCIL_MASK, n, 1, 15.0); n++;
    cmds[n] = diff_cmd(CMD_COLOR_MASK, n, 4, 1.0, 0.0, 1.0, 0.0); n++;
    cmds[n] = diff_cmd(CMD_DEPTH_MASK, n, 1, 0.0); n++;

    diff_case("inspector vs driver: user state writes", cmds, n, n);

    diff_vec_row("GL_CURRENT_COLOR", GL_CURRENT_COLOR, 4);
    diff_vec_row("GL_CURRENT_NORMAL", GL_CURRENT_NORMAL, 3);
    diff_enum_row("GL_DEPTH_FUNC", GL_DEPTH_FUNC);
    diff_enum_row("GL_SHADE_MODEL", GL_SHADE_MODEL);
    diff_enum_row("GL_BLEND_SRC", GL_BLEND_SRC);
    diff_enum_row("GL_BLEND_DST", GL_BLEND_DST);
    diff_enum_row("GL_FRONT_FACE", GL_FRONT_FACE);
    diff_polygon_mode_rows();
    diff_float_row("GL_POLYGON_OFFSET_FACTOR", GL_POLYGON_OFFSET_FACTOR);
    diff_float_row("GL_POLYGON_OFFSET_UNITS", GL_POLYGON_OFFSET_UNITS);
    diff_float_row("GL_POINT_SIZE", GL_POINT_SIZE);
    diff_enum_row("GL_STENCIL_FUNC", GL_STENCIL_FUNC);
    diff_int_row("GL_STENCIL_REF", GL_STENCIL_REF);
    diff_stencil_mask_row("GL_STENCIL_VALUE_MASK", GL_STENCIL_VALUE_MASK);
    diff_stencil_mask_row("GL_STENCIL_WRITEMASK", GL_STENCIL_WRITEMASK);
    diff_color_mask_row();
    diff_bool_row("GL_DEPTH_WRITEMASK", GL_DEPTH_WRITEMASK);
}

/* 3. The transform fold: gl_state_inspector.c composes its own 4x4 matrices,
 * so this is the case that would catch a wrong multiplication order or a
 * rotation-matrix slip. */
static void test_diff_transform_fold(void) {
    GLCmd cmds[7];
    int n = 0;

    cmds[n] = diff_cmd(CMD_TRANSLATE3F, n, 3, 1.0, 2.0, -3.0); n++;
    cmds[n] = diff_cmd(CMD_ROTATEF, n, 4, 35.0, 0.0, 1.0, 0.0); n++;
    cmds[n] = diff_cmd(CMD_PUSH_MATRIX, n, 0); n++;
    cmds[n] = diff_cmd(CMD_SCALEF, n, 3, 2.0, 0.5, 1.0); n++;
    cmds[n] = diff_cmd(CMD_ROTATEF, n, 4, -20.0, 1.0, 0.0, 1.0); n++;
    cmds[n] = diff_cmd(CMD_POP_MATRIX, n, 0); n++;
    cmds[n] = diff_cmd(CMD_TRANSLATE3F, n, 3, 0.0, 0.0, 4.0); n++;

    /* Inside the push: the scaled + twice-rotated matrix, stack depth 2. */
    diff_case("inspector vs driver: transforms inside a push", cmds, n, 5);
    diff_matrix_row();
    diff_int_row("GL_MODELVIEW_STACK_DEPTH", GL_MODELVIEW_STACK_DEPTH);

    /* After the pop plus one more translate: the pop must have discarded the
     * scale, and the trailing translate composes onto the restored matrix. */
    diff_case("inspector vs driver: transforms after the pop", cmds, n, n);
    diff_matrix_row();
    diff_int_row("GL_MODELVIEW_STACK_DEPTH", GL_MODELVIEW_STACK_DEPTH);
}

/* 4. glPushAttrib/glPopAttrib group semantics: the pop must restore the cells
 * the mask covers and leave the rest alone. The fold decides membership from
 * attrib_bits; GL decides it from the spec, and here they must agree. */
static void test_diff_attrib_group_scoping(void) {
    GLCmd cmds[6];
    int n = 0;

    cmds[n] = diff_cmd(CMD_COLOR3F, n, 3, 1.0, 0.0, 0.0); n++;
    cmds[n] = diff_cmd(CMD_LINE_WIDTH, n, 1, 2.0); n++;
    cmds[n] = diff_cmd(CMD_PUSH_ATTRIB, n, 1, (double)GL_CURRENT_BIT); n++;
    cmds[n] = diff_cmd(CMD_COLOR3F, n, 3, 0.0, 0.0, 1.0); n++;
    cmds[n] = diff_cmd(CMD_LINE_WIDTH, n, 1, 5.0); n++;
    cmds[n] = diff_cmd(CMD_POP_ATTRIB, n, 0); n++;

    diff_case("inspector vs driver: GL_CURRENT_BIT scope", cmds, n, n);

    /* Covered by the mask: back to red on both sides. */
    diff_vec_row("GL_CURRENT_COLOR", GL_CURRENT_COLOR, 4);
    /* Not covered: the width written inside the scope survives the pop. */
    diff_float_row("GL_LINE_WIDTH", GL_LINE_WIDTH);
    diff_int_row("GL_ATTRIB_STACK_DEPTH", GL_ATTRIB_STACK_DEPTH);
}

/* 5. The raster-color latch (see gl_state_inspector.c's CMD_RASTER_POS3F
 * case): glRasterPos copies the current color once and a later glColor no
 * longer moves the cell. Lighting stays off, which is exactly when the fold
 * claims an exact value — the lit case is reported under a different row name
 * and excluded from this comparison by design. */
static void test_diff_raster_color_latch(void) {
    GLCmd cmds[4];
    int n = 0;

    cmds[n] = diff_cmd(CMD_DISABLE, n, 1, (double)GL_LIGHTING); n++;
    cmds[n] = diff_cmd(CMD_COLOR3F, n, 3, 1.0, 0.0, 0.0); n++;
    cmds[n] = diff_cmd(CMD_RASTER_POS3F, n, 3, 0.0, 0.0, 0.0); n++;
    cmds[n] = diff_cmd(CMD_COLOR3F, n, 3, 0.0, 1.0, 0.0); n++;

    diff_case("inspector vs driver: raster color latch", cmds, n, n);

    diff_require_valid_raster_pos("raster color latch");
    diff_vec_row("GL_CURRENT_RASTER_COLOR", GL_CURRENT_RASTER_COLOR, 4);
    diff_vec_row("GL_CURRENT_COLOR", GL_CURRENT_COLOR, 4);
}

/* 6. The lit raster color, one case per branch of gl_state_lit_color(). The
 * fold evaluates GL's lighting equation to fill this cell, so a driver is the
 * only thing that can say whether it evaluates it correctly; every case here
 * enables GL_LIGHTING and compares the row against GL_CURRENT_RASTER_COLOR.
 *
 * Materials are set explicitly rather than inherited, so each case leans on one
 * term and no expected value depends on the generated setup's light-model
 * ambient constant. Raster positions stay well inside the clip volume (the
 * projection is identity here) because a clipped position makes GL's stored
 * color undefined — asserted per case, see diff_require_valid_raster_pos. */
static void test_diff_lit_raster_color(void) {
    GLCmd cmds[12];
    int n;

    /* (a) Emission + scene ambient: no light enabled, so the per-light sum is
     * empty and the whole value comes from the material. */
    n = 0;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHTING); n++;
    cmds[n] = diff_cmd(CMD_MATERIALFV, n, 6, (double)GL_FRONT,
                       (double)GL_EMISSION, 0.5, 0.25, 0.125, 1.0); n++;
    cmds[n] = diff_cmd(CMD_COLOR3F, n, 3, 1.0, 0.0, 0.0); n++;
    cmds[n] = diff_cmd(CMD_RASTER_POS3F, n, 3, 0.0, 0.0, 0.0); n++;
    diff_case("lit raster color: emission + scene ambient", cmds, n, n);
    diff_require_valid_raster_pos("emission + scene ambient");
    diff_vec_row("GL_CURRENT_RASTER_COLOR", GL_CURRENT_RASTER_COLOR, 4);
    /* The current color is untouched by lighting: the two rows must differ, or
     * this case would also pass with the fold just copying glColor3f. */
    diff_vec_row("GL_CURRENT_COLOR", GL_CURRENT_COLOR, 4);
    {
        const ReplGlStateReportRow *lit = diff_row("GL_CURRENT_RASTER_COLOR");
        const ReplGlStateReportRow *cur = diff_row("GL_CURRENT_COLOR");
        ASSERT_TRUE("lit raster color is not the current color",
                    lit && cur && strcmp(lit->current, cur->current) != 0);
    }

    /* (b) Diffuse from a positional light (slot 0, w = 1), with a normal that
     * has to be carried into eye space by a rotated + translated modelview —
     * the inverse-transpose path. */
    if (diff_driver_is_mesa()) {
        diff_skip("diffuse under a transformed modelview",
                  "Mesa lights the raster position in object space");
    } else {
        n = 0;
        cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHTING); n++;
        cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHT0); n++;
        cmds[n] = diff_cmd(CMD_MATERIALFV, n, 6, (double)GL_FRONT,
                           (double)GL_DIFFUSE, 0.8, 0.6, 0.4, 0.75); n++;
        cmds[n] = diff_cmd(CMD_ROTATEF, n, 4, 40.0, 0.0, 1.0, 0.0); n++;
        cmds[n] = diff_cmd(CMD_TRANSLATE3F, n, 3, 0.2, 0.3, -0.4); n++;
        cmds[n] = diff_cmd(CMD_NORMAL3F, n, 3, 0.0, 0.7071, 0.7071); n++;
        cmds[n] = diff_cmd(CMD_RASTER_POS3F, n, 3, 0.1, 0.2, 0.1); n++;
        diff_case("lit raster color: diffuse + transformed normal", cmds, n, n);
        diff_require_valid_raster_pos("diffuse + transformed normal");
        diff_vec_row("GL_CURRENT_RASTER_COLOR", GL_CURRENT_RASTER_COLOR, 4);
    }

    /* (c) Diffuse from the directional light (slot 1, w = 0), where the stored
     * position is the direction to the light rather than a point. */
    n = 0;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHTING); n++;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHT1); n++;
    cmds[n] = diff_cmd(CMD_MATERIALFV, n, 6, (double)GL_FRONT,
                       (double)GL_DIFFUSE, 0.9, 0.8, 0.7, 1.0); n++;
    cmds[n] = diff_cmd(CMD_NORMAL3F, n, 3, 0.0, 0.5, 0.866); n++;
    cmds[n] = diff_cmd(CMD_RASTER_POS3F, n, 3, 0.1, -0.2, 0.3); n++;
    diff_case("lit raster color: directional light", cmds, n, n);
    diff_require_valid_raster_pos("directional light");
    diff_vec_row("GL_CURRENT_RASTER_COLOR", GL_CURRENT_RASTER_COLOR, 4);

    /* (d) Specular: shininess plus a local viewer, which swings the half vector
     * from the default +z round to "towards the eye at the origin". Both lights
     * on, so the per-light sum accumulates rather than being one term. */
    n = 0;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHTING); n++;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHT0); n++;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHT1); n++;
    cmds[n] = diff_cmd(CMD_LIGHT_MODEL_I, n, 2,
                       (double)GL_LIGHT_MODEL_LOCAL_VIEWER, 1.0); n++;
    cmds[n] = diff_cmd(CMD_MATERIALFV, n, 6, (double)GL_FRONT,
                       (double)GL_SPECULAR, 0.6, 0.6, 0.5, 1.0); n++;
    cmds[n] = diff_cmd(CMD_MATERIALF, n, 3, (double)GL_FRONT,
                       (double)GL_SHININESS, 12.0); n++;
    cmds[n] = diff_cmd(CMD_NORMAL3F, n, 3, 0.0, 0.0, 1.0); n++;
    cmds[n] = diff_cmd(CMD_RASTER_POS3F, n, 3, 0.0, 0.0, -0.5); n++;
    diff_case("lit raster color: specular + local viewer", cmds, n, n);
    diff_require_valid_raster_pos("specular + local viewer");
    diff_vec_row("GL_CURRENT_RASTER_COLOR", GL_CURRENT_RASTER_COLOR, 4);

    /* (e) Saturation: emission past 1.0 must come back clamped, which is the
     * one clamp GL does specify for the lit color. */
    n = 0;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHTING); n++;
    cmds[n] = diff_cmd(CMD_MATERIALFV, n, 6, (double)GL_FRONT,
                       (double)GL_EMISSION, 1.75, 0.5, -0.25, 1.0); n++;
    cmds[n] = diff_cmd(CMD_RASTER_POS3F, n, 3, 0.0, 0.0, 0.0); n++;
    diff_case("lit raster color: saturated emission", cmds, n, n);
    diff_require_valid_raster_pos("saturated emission");
    diff_vec_row("GL_CURRENT_RASTER_COLOR", GL_CURRENT_RASTER_COLOR, 4);

    /* (f) GL_NORMALIZE against a scaled modelview: with the switch off GL lights
     * an unnormalized normal and the fold must not tidy that up; with it on,
     * both normalize. The difference between the two halves is the whole reason
     * the cap is tracked, so both are compared. */
    n = 0;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHTING); n++;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHT0); n++;
    cmds[n] = diff_cmd(CMD_MATERIALFV, n, 6, (double)GL_FRONT,
                       (double)GL_DIFFUSE, 0.9, 0.9, 0.9, 1.0); n++;
    /* Scale up, so the inverse transpose *shrinks* the normal to a quarter
     * length: both halves then land in range and differ by an exact factor,
     * where scaling down saturates the normalized half at (1,1,1) and hides
     * which model produced it. */
    cmds[n] = diff_cmd(CMD_SCALEF, n, 3, 4.0, 4.0, 4.0); n++;
    cmds[n] = diff_cmd(CMD_NORMAL3F, n, 3, 0.0, 0.0, 1.0); n++;
    cmds[n] = diff_cmd(CMD_RASTER_POS3F, n, 3, 0.0, 0.0, 0.0); n++;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_NORMALIZE); n++;
    cmds[n] = diff_cmd(CMD_RASTER_POS3F, n, 3, 0.0, 0.0, 0.0); n++;
    diff_case("lit raster color: unnormalized normal", cmds, n, 6);
    diff_require_valid_raster_pos("unnormalized normal");
    diff_vec_row("GL_CURRENT_RASTER_COLOR", GL_CURRENT_RASTER_COLOR, 4);
    if (diff_driver_is_mesa()) {
        diff_skip("GL_NORMALIZE half", "Mesa ignores it when lighting the "
                                       "raster position");
    } else {
        diff_case("lit raster color: GL_NORMALIZE", cmds, n, n);
        diff_require_valid_raster_pos("GL_NORMALIZE");
        diff_vec_row("GL_CURRENT_RASTER_COLOR", GL_CURRENT_RASTER_COLOR, 4);
    }

    /* (g) glColorMaterial: the material tracks glColor, so the latched value
     * follows the program's color *through* the lighting equation instead of
     * being copied from it. Apple's legacy GL gets this wrong (it stores
     * (0,0,0,1) as if the material were zeroed), so there the case is skipped
     * loudly rather than asserted — see diff_driver_is_apple_legacy. */
    if (diff_driver_is_apple_legacy()) {
        diff_skip("color-material raster latch",
                  "Apple stores (0,0,0,1) here; Mesa agrees with the fold");
    } else {
        n = 0;
        cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHTING); n++;
        cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_LIGHT0); n++;
        cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_COLOR_MATERIAL); n++;
        cmds[n] = diff_cmd(CMD_COLOR_MATERIAL, n, 2, (double)GL_FRONT,
                           (double)GL_AMBIENT_AND_DIFFUSE); n++;
        cmds[n] = diff_cmd(CMD_COLOR4F, n, 4, 0.3, 0.6, 0.9, 0.5); n++;
        cmds[n] = diff_cmd(CMD_NORMAL3F, n, 3, 0.0, 0.0, 1.0); n++;
        cmds[n] = diff_cmd(CMD_RASTER_POS3F, n, 3, 0.0, 0.0, 0.0); n++;
        diff_case("lit raster color: color material", cmds, n, n);
        diff_require_valid_raster_pos("color material");
        diff_vec_row("GL_CURRENT_RASTER_COLOR", GL_CURRENT_RASTER_COLOR, 4);
    }

    /* (h) Out-of-range color with lighting off: the drivers disagree on whether
     * the latched value is clamped, so this accepts either and names which. */
    n = 0;
    cmds[n] = diff_cmd(CMD_DISABLE, n, 1, (double)GL_LIGHTING); n++;
    cmds[n] = diff_cmd(CMD_COLOR4F, n, 4, 1.5, -0.5, 0.25, 1.0); n++;
    cmds[n] = diff_cmd(CMD_RASTER_POS3F, n, 3, 0.0, 0.0, 0.0); n++;
    diff_case("lit raster color: out-of-range latch", cmds, n, n);
    diff_require_valid_raster_pos("out-of-range latch");
    diff_raster_color_raw_or_clamped();
}

/* 7. Materials, including the one ordering that bites: GL_COLOR_MATERIAL makes
 * the tracked components follow the current color from the moment it is
 * ENABLED, not just on later glColor calls. Both drivers report the tracked
 * value here even though the glColor came first, so this is portable — and it is
 * the case that would catch the fold deferring the tracking to the next glColor.
 * (The lit raster color under the same state is Apple's broken path; the
 * material cells themselves are fine there.) */
static void test_diff_materials(void) {
    GLCmd cmds[8];
    int n = 0;

    cmds[n] = diff_cmd(CMD_COLOR4F, n, 4, 0.25, 0.7, 1.0, 0.5); n++;
    cmds[n] = diff_cmd(CMD_COLOR_MATERIAL, n, 2, (double)GL_FRONT,
                       (double)GL_AMBIENT_AND_DIFFUSE); n++;
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_COLOR_MATERIAL); n++;
    cmds[n] = diff_cmd(CMD_MATERIALFV, n, 6, (double)GL_FRONT,
                       (double)GL_SPECULAR, 0.6, 0.5, 0.4, 1.0); n++;
    cmds[n] = diff_cmd(CMD_MATERIALFV, n, 6, (double)GL_FRONT,
                       (double)GL_EMISSION, 0.05, 0.06, 0.07, 1.0); n++;
    cmds[n] = diff_cmd(CMD_MATERIALF, n, 3, (double)GL_FRONT,
                       (double)GL_SHININESS, 24.0); n++;

    diff_case("inspector vs driver: materials + color-material tracking",
              cmds, n, n);

    diff_material_row("GL_FRONT_MATERIAL_AMBIENT", GL_FRONT, GL_AMBIENT, 4);
    diff_material_row("GL_FRONT_MATERIAL_DIFFUSE", GL_FRONT, GL_DIFFUSE, 4);
    diff_material_row("GL_FRONT_MATERIAL_SPECULAR", GL_FRONT, GL_SPECULAR, 4);
    diff_material_row("GL_FRONT_MATERIAL_EMISSION", GL_FRONT, GL_EMISSION, 4);
    diff_material_row("GL_FRONT_MATERIAL_SHININESS", GL_FRONT, GL_SHININESS, 1);
    diff_cap_row("GL_COLOR_MATERIAL", GL_COLOR_MATERIAL);
    diff_enum_row("GL_COLOR_MATERIAL_FACE", GL_COLOR_MATERIAL_FACE);
    diff_enum_row("GL_COLOR_MATERIAL_PARAMETER", GL_COLOR_MATERIAL_PARAMETER);
}

/* 7. Clip planes, in both directions of the "(object)" label (see
 * diff_clip_plane_row): under the identity modelview the generated prologue
 * leaves behind, the fold's object-space equation IS what GL stores, so the row
 * compares directly; under a user transform the two must differ by exactly that
 * transform. The second half is what would catch the fold either transforming
 * the equation itself or being handed an already-transformed one. */
static void test_diff_clip_planes(void) {
    GLCmd cmds[4];
    int n = 0;

    /* Two planes, first and last slot, to pin the GL_CLIP_PLANE0 + n indexing
     * on both sides. */
    cmds[n] = diff_cmd(CMD_ENABLE, n, 1, (double)GL_CLIP_PLANE0); n++;
    cmds[n] = diff_cmd(CMD_CLIP_PLANE, n, 5, (double)GL_CLIP_PLANE0,
                       0.0, 1.0, 0.0, -0.5); n++;
    cmds[n] = diff_cmd(CMD_CLIP_PLANE, n, 5, (double)GL_CLIP_PLANE5,
                       1.0, 0.0, 0.0, 0.25); n++;

    diff_case("inspector vs driver: clip planes under identity", cmds, n, n);
    diff_cap_row("GL_CLIP_PLANE0", GL_CLIP_PLANE0);
    diff_clip_plane_row(0, 0.0f, 0.0f, 0.0f);
    diff_clip_plane_row(5, 0.0f, 0.0f, 0.0f);

    n = 0;
    cmds[n] = diff_cmd(CMD_TRANSLATE3F, n, 3, 0.5, -1.5, 2.0); n++;
    cmds[n] = diff_cmd(CMD_CLIP_PLANE, n, 5, (double)GL_CLIP_PLANE1,
                       0.0, 1.0, 0.0, -0.5); n++;
    /* A second plane whose every coefficient contributes to the transformed
     * distance term, so a dropped or misordered component cannot cancel. */
    cmds[n] = diff_cmd(CMD_CLIP_PLANE, n, 5, (double)GL_CLIP_PLANE2,
                       0.25, -0.5, 0.75, 1.0); n++;

    diff_case("inspector vs driver: clip planes under a translate", cmds, n, n);
    diff_clip_plane_row(1, 0.5f, -1.5f, 2.0f);
    diff_clip_plane_row(2, 0.5f, -1.5f, 2.0f);
    /* The fold's row is the object-space input, so it must NOT have moved with
     * the modelview — the property the closed-form check above rests on. */
    {
        const ReplGlStateReportRow *row =
            diff_row("GL_CLIP_PLANE1_EQUATION (object)");
        ASSERT_TRUE("clip plane row stays in object coordinates",
                    row && strcmp(row->current, "(0, 1, 0, -0.5)") == 0);
    }
}

int main(int argc, char **argv) {
#if defined(__APPLE__) && !defined(FREEGLUT_OSMESA)
    uint32_t display_count = 0;
    if (CGGetActiveDisplayList(0, NULL, &display_count) != kCGErrorSuccess ||
        display_count == 0) {
        fprintf(stderr,
                "test_gl_state_inspector_gl: no active macOS display; "
                "skipping\n");
        return 0;
    }
#endif
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH | GLUT_STENCIL);
    glutInitWindowSize(64, 64);
    if (glutCreateWindow("gl-state-inspector-diff") <= 0) {
        fprintf(stderr, "test_gl_state_inspector_gl: no GL context (need a "
                        "display); skipping\n");
        return 0;  /* opt-in target: absence of a display is not a failure */
    }
    repl_executor_init_resources();
    repl_export_install_light_bridge(&k_diff_light_bridge);

    /* Name the driver: this test's whole premise is that GL is the authority,
     * so when a row disagrees the first question is always "which
     * implementation said so". */
    printf("--- gl_state_inspector differential (real GL context) ---\n");
    printf("    GL_VENDOR   %s\n", (const char *)glGetString(GL_VENDOR));
    printf("    GL_RENDERER %s\n", (const char *)glGetString(GL_RENDERER));
    printf("    GL_VERSION  %s\n", (const char *)glGetString(GL_VERSION));
    test_diff_generated_prologue();
    test_diff_user_state_writes();
    test_diff_transform_fold();
    test_diff_attrib_group_scoping();
    test_diff_raster_color_latch();
    test_diff_lit_raster_color();
    test_diff_materials();
    test_diff_clip_planes();
    diff_close_cursor();
    repl_executor_destroy_resources();
    return test_harness_report(&g_harness, "gl_state_inspector_gl");
}
