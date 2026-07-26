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
 *   - "GL_CURRENT_RASTER_COLOR (unlit input)" — under lighting GL stores the
 *     lit color; the fold reports what fed the lighting equation.
 * Their exact-value twins (the "(eye)" light position, the unqualified raster
 * color) are compared.
 *
 * In GL_TEST_BINS / `make gl-tests` (needs a real context; a display, or
 * FREEGLUT_OSMESA=1 headless) — NOT in `make test` / `make test-stubs`.
 */
#include "gl_includes.h"
#include "repl/gl_state_inspector.h"
#include "repl/init_state.h"
#include "repl/executor.h"
#include "repl/command_spec.h"
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

    diff_vec_row("GL_CURRENT_RASTER_COLOR", GL_CURRENT_RASTER_COLOR, 4);
    diff_vec_row("GL_CURRENT_COLOR", GL_CURRENT_COLOR, 4);
    ASSERT_TRUE("lit-input row absent with lighting off",
                diff_row("GL_CURRENT_RASTER_COLOR (unlit input)") == NULL);
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

    printf("--- gl_state_inspector differential (real GL context) ---\n");
    test_diff_generated_prologue();
    test_diff_user_state_writes();
    test_diff_transform_fold();
    test_diff_attrib_group_scoping();
    test_diff_raster_color_latch();
    diff_close_cursor();
    repl_executor_destroy_resources();
    return test_harness_report(&g_harness, "gl_state_inspector_gl");
}
