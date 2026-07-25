#include "render3d/guides/transform_guides.h"
#include "gl_includes.h"
#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#endif

#include "support/test_harness.h"
#include <stdio.h>
#include "repl/flatten.h"
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    TEST_ASSERT_INT(&g_harness, label, got, exp); \
} while (0)

#define TEST_GUIDE_LABEL_CAP 8

typedef struct TestGuideLabelRecord {
    float pos[3];
    void *font[RENDER3D_GUIDE_LABEL_MAX_RUNS];
    char text[RENDER3D_GUIDE_LABEL_MAX_RUNS][160];
    int run_count;
} TestGuideLabelRecord;

typedef struct TestGuideLabelCapture {
    TestGuideLabelRecord labels[TEST_GUIDE_LABEL_CAP];
    int count;
} TestGuideLabelCapture;

static void test_guide_label_record(void *user_data,
                                    const Render3dGuideLabelSpec *label) {
    TestGuideLabelCapture *capture = (TestGuideLabelCapture *)user_data;
    TestGuideLabelRecord *record;
    int run_count;
    int i;

    if (!capture || !label || capture->count >= TEST_GUIDE_LABEL_CAP)
        return;
    record = &capture->labels[capture->count++];
    memset(record, 0, sizeof(*record));
    record->pos[0] = label->pos[0];
    record->pos[1] = label->pos[1];
    record->pos[2] = label->pos[2];
    run_count = label->run_count;
    if (run_count < 0) run_count = 0;
    if (run_count > RENDER3D_GUIDE_LABEL_MAX_RUNS)
        run_count = RENDER3D_GUIDE_LABEL_MAX_RUNS;
    record->run_count = run_count;
    for (i = 0; i < run_count; i++) {
        record->font[i] = label->runs[i].font;
        if (label->runs[i].text) {
            strncpy(record->text[i], label->runs[i].text,
                    sizeof(record->text[i]) - 1);
            record->text[i][sizeof(record->text[i]) - 1] = '\0';
        }
    }
}

static void install_test_label_sink(Render3dGuideSnapshot *snapshot,
                                    TestGuideLabelCapture *capture) {
    memset(capture, 0, sizeof(*capture));
    snapshot->label_sink.record = test_guide_label_record;
    snapshot->label_sink.user_data = capture;
}

static Render3dGuideSnapshot base_snapshot(const GLCmd *source_cmds,
                                        int source_count,
                                        const GLCmd *flat_cmds,
                                        int flat_count,
                                        int edit_line_idx,
                                        const char *input) {
    Render3dGuideSnapshot snapshot = {0};
    snapshot.show_guides = 1;
    snapshot.replaying = 0;
    snapshot.edit_line_idx = edit_line_idx;
    snapshot.source_cmds = source_cmds;
    snapshot.source_cmd_count = source_count;
    snapshot.flat_program = (FlatProgramView){
        .cmds = flat_cmds,
        .local_vars = NULL,
        .cmd_count = flat_count
    };
    snapshot.input = input;
    snapshot.input_len = (int)strlen(input);
    return snapshot;
}

#ifdef GL_STUBS
static void test_transform_guides_render(void) {
    printf("--- transform guides rendering ---\n");

    /* 1. Translate guide rendering */
    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_TRANSLATE3F;
        source_cmds[0].valid = 1;
        source_cmds[0].args[0] = 2.0f;
        source_cmds[0].args[1] = 0.0f;
        source_cmds[0].args[2] = 0.0f;

        flat_cmds[0].type = CMD_TRANSLATE3F;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[0].args[0] = 2.0f;
        flat_cmds[0].args[1] = 0.0f;
        flat_cmds[0].args[2] = 0.0f;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0,
                          "glTranslatef(2,0,0)");
        snapshot.edit_line_committed_text = "glTranslatef(2,0,0);";
        snapshot.alpha_scale = 1.0f;
        snapshot.anim_time = 0.0f;
        snapshot.xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;
        TestGuideLabelCapture capture;
        install_test_label_sink(&snapshot, &capture);

        int prepared = render3d_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("translate prepared", prepared, 1);

        float cam_view[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

        gl_stub_counts_reset();
        render3d_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
        ASSERT_TRUE("translate rendering calls glBegin", gl_stub_counts[GL_STUB_glBegin] > 0);
        ASSERT_TRUE("translate rendering calls glPushAttrib", gl_stub_counts[GL_STUB_glPushAttrib] > 0);
        ASSERT_INT("translate cone brackets one colorless depth prepass",
                   (int)gl_stub_counts[GL_STUB_glColorMask], 2);
        ASSERT_INT("translate cone uses LEQUAL for its depth and color passes",
                   (int)gl_stub_counts[GL_STUB_glDepthFunc], 1);
        /* Seven ghost-pass primitives (no base cap) plus ten solid-pass
         * primitives (including the two-fan depth prepass). */
        ASSERT_INT("translate ghost cone omits its base cap",
                   (int)gl_stub_counts[GL_STUB_glBegin], 17);
        ASSERT_INT("translate records one endpoint label", capture.count, 1);
        ASSERT_INT("translate label has primary and detail runs",
                   capture.labels[0].run_count, 2);
        ASSERT_TRUE("translate label names the transform",
                    strcmp(capture.labels[0].text[0], " move") == 0);
        ASSERT_TRUE("translate label records endpoint coordinates",
                    strstr(capture.labels[0].text[1], "2.00") != NULL);
    }

    /* 2. Scale guide rendering */
    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_SCALEF;
        source_cmds[0].valid = 1;
        source_cmds[0].args[0] = 2.0f;
        source_cmds[0].args[1] = 2.0f;
        source_cmds[0].args[2] = 2.0f;

        flat_cmds[0].type = CMD_SCALEF;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[0].args[0] = 2.0f;
        flat_cmds[0].args[1] = 2.0f;
        flat_cmds[0].args[2] = 2.0f;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0,
                          "glScalef(2,2,2)");
        snapshot.edit_line_committed_text = "glScalef(2,2,2);";
        snapshot.alpha_scale = 1.0f;
        snapshot.anim_time = 0.0f;
        snapshot.xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;
        TestGuideLabelCapture capture;
        install_test_label_sink(&snapshot, &capture);

        int prepared = render3d_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("scale prepared", prepared, 1);

        float cam_view[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

        gl_stub_counts_reset();
        render3d_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
        ASSERT_TRUE("scale rendering calls glBegin", gl_stub_counts[GL_STUB_glBegin] > 0);
        ASSERT_INT("scale records one factor label", capture.count, 1);
        ASSERT_INT("scale label has primary and detail runs",
                   capture.labels[0].run_count, 2);
        ASSERT_TRUE("scale label names the transform",
                    strcmp(capture.labels[0].text[0], " scale") == 0);
        ASSERT_TRUE("scale label records the factors",
                    strstr(capture.labels[0].text[1], "2.00") != NULL);
    }

    /* 3. Rotate guide rendering */
    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_ROTATEF;
        source_cmds[0].valid = 1;
        source_cmds[0].args[0] = 45.0f;
        source_cmds[0].args[1] = 0.0f;
        source_cmds[0].args[2] = 1.0f;
        source_cmds[0].args[3] = 0.0f;

        flat_cmds[0].type = CMD_ROTATEF;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[0].args[0] = 45.0f;
        flat_cmds[0].args[1] = 0.0f;
        flat_cmds[0].args[2] = 1.0f;
        flat_cmds[0].args[3] = 0.0f;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0,
                          "glRotatef(45,0,1,0)");
        snapshot.edit_line_committed_text = "glRotatef(45,0,1,0);";
        snapshot.alpha_scale = 1.0f;
        snapshot.anim_time = 0.0f;
        snapshot.xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;
        TestGuideLabelCapture capture;
        install_test_label_sink(&snapshot, &capture);

        int prepared = render3d_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("rotate prepared", prepared, 1);

        float cam_view[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

        gl_stub_counts_reset();
        render3d_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
        ASSERT_TRUE("rotate rendering calls glBegin", gl_stub_counts[GL_STUB_glBegin] > 0);
        ASSERT_INT("rotate records one angle label", capture.count, 1);
        ASSERT_TRUE("rotate label records the swept angle",
                    strstr(capture.labels[0].text[0], "+45") != NULL);
    }

    /* 4. World-aligned translation guide rendering (exercises compute_after_cursor_origin) */
    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[3] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_TRANSLATE3F;
        source_cmds[0].valid = 1;
        source_cmds[0].args[0] = 2.0f;
        source_cmds[0].args[1] = 0.0f;
        source_cmds[0].args[2] = 0.0f;

        flat_cmds[0].type = CMD_TRANSLATE3F;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[0].args[0] = 2.0f;
        flat_cmds[0].args[1] = 0.0f;
        flat_cmds[0].args[2] = 0.0f;

        flat_cmds[1].type = CMD_TRANSLATE3F;
        flat_cmds[1].valid = 1;
        flat_cmds[1].src_cmd_idx = 1; /* post-cursor */
        flat_cmds[1].args[0] = 1.0f;
        flat_cmds[1].args[1] = 2.0f;
        flat_cmds[1].args[2] = 3.0f;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 2, 0,
                          "glTranslatef(2,0,0)");
        snapshot.edit_line_committed_text = "glTranslatef(2,0,0);";
        snapshot.alpha_scale = 1.0f;
        snapshot.anim_time = 0.0f;
        snapshot.xform_guide_mode = RENDER3D_XFORM_GUIDE_WORLD;

        int prepared = render3d_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("world-aligned translate prepared", prepared, 1);
        ASSERT_INT("after_flat_idx set correctly", plan.after_flat_idx, 1);

        float cam_view[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

        gl_stub_counts_reset();
        render3d_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
        ASSERT_TRUE("world-aligned translate rendering calls glBegin", gl_stub_counts[GL_STUB_glBegin] > 0);
    }

    /* 5. Rotate guide rendering with non-zero translation origin (exercises build_rotate_arc) */
    {
        extern float g_gl_stub_modelview_matrix[16];
        g_gl_stub_modelview_matrix[12] = 1.0f; /* non-zero translation origin */
        g_gl_stub_modelview_matrix[13] = 0.0f;
        g_gl_stub_modelview_matrix[14] = 0.0f;

        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_ROTATEF;
        source_cmds[0].valid = 1;
        source_cmds[0].args[0] = 45.0f;
        source_cmds[0].args[1] = 0.0f;
        source_cmds[0].args[2] = 1.0f;
        source_cmds[0].args[3] = 0.0f;

        flat_cmds[0].type = CMD_ROTATEF;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[0].args[0] = 45.0f;
        flat_cmds[0].args[1] = 0.0f;
        flat_cmds[0].args[2] = 1.0f;
        flat_cmds[0].args[3] = 0.0f;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0,
                          "glRotatef(45,0,1,0)");
        snapshot.edit_line_committed_text = "glRotatef(45,0,1,0);";
        snapshot.alpha_scale = 1.0f;
        snapshot.anim_time = 0.0f;
        snapshot.xform_guide_mode = RENDER3D_XFORM_GUIDE_WORLD;

        int prepared = render3d_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("rotate with origin prepared", prepared, 1);

        float cam_view[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

        gl_stub_counts_reset();
        render3d_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
        ASSERT_TRUE("rotate with origin rendering calls glBegin", gl_stub_counts[GL_STUB_glBegin] > 0);

        /* Reset stub matrix to identity */
        g_gl_stub_modelview_matrix[12] = 0.0f;
    }

    /* 5. Live edit: the input buffer diverges from the committed line, so the
     * guide renders from the pre-evaluated cursor args (not the committed
     * flat args), with identity defaults for untyped slots. Here the user is
     * mid-typing glTranslatef(5, 6  — z is untyped, defaulting to 0. */
    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_TRANSLATE3F;
        source_cmds[0].valid = 1;
        flat_cmds[0].type = CMD_TRANSLATE3F;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[0].args[0] = 1.0f; /* committed (1,0,0) — must NOT be drawn */

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0, "glTranslatef(5, 6");
        snapshot.edit_line_committed_text = "glTranslatef(1,0,0);";
        snapshot.alpha_scale = 1.0f;
        snapshot.xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;
        /* As the controller would fill them: two slots typed, z untyped. */
        snapshot.xform_args[0] = 5.0f; snapshot.xform_filled[0] = 1;
        snapshot.xform_args[1] = 6.0f; snapshot.xform_filled[1] = 1;
        snapshot.xform_n_filled = 2;

        int prepared = render3d_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("live-edit translate prepared", prepared, 1);

        float cam_view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        gl_stub_counts_reset();
        render3d_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
        ASSERT_TRUE("live-edit translate renders from cursor args",
                    gl_stub_counts[GL_STUB_glBegin] > 0);
    }

    /* 6. Brand-new line: a transform typed on a not-yet-committed line (no
     * source cmd, edit_line at the source tail) still activates, anchored at
     * the insertion point (the flat tail), rendered via the on_end flush. */
    {
        GLCmd source_cmds[1] = {0};
        GLCmd flat_cmds[1] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_VERTEX3F;
        source_cmds[0].valid = 1;
        flat_cmds[0].type = CMD_VERTEX3F;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;

        /* edit_line 1 == source_cmd_count: a fresh appended line. */
        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 1, flat_cmds, 1, 1, "glScalef(2,2,2)");
        snapshot.edit_line_committed_text = NULL; /* nothing committed yet */
        snapshot.alpha_scale = 1.0f;
        snapshot.xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;
        snapshot.xform_args[0] = 2.0f; snapshot.xform_filled[0] = 1;
        snapshot.xform_args[1] = 2.0f; snapshot.xform_filled[1] = 1;
        snapshot.xform_args[2] = 2.0f; snapshot.xform_filled[2] = 1;
        snapshot.xform_n_filled = 3;

        int prepared = render3d_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("new-line transform prepared", prepared, 1);
        ASSERT_INT("new-line anchor is the flat tail", plan.cursor_flat_idx, 1);

        float cam_view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        gl_stub_counts_reset();
        render3d_transform_guides_render_if_due(&snapshot, &plan,
                                             plan.cursor_flat_idx, cam_view);
        ASSERT_TRUE("new-line transform renders at tail flush",
                    gl_stub_counts[GL_STUB_glBegin] > 0);
    }
}

/* Req 6: during replay the prepared plan's transform guide renders when the
 * walk reaches the chosen transform's flat index (not the vertex). */
static void test_replay_transform_guide_render(void) {
    printf("--- replay transform guide rendering ---\n");

    GLCmd source_cmds[3] = {0};
    GLCmd flat_cmds[3] = {0};
    Render3dTransformGuidePlan plan;

    source_cmds[0].type = CMD_TRANSLATE3F; source_cmds[0].valid = 1;
    source_cmds[1].type = CMD_VERTEX3F;    source_cmds[1].valid = 1;

    flat_cmds[0].type = CMD_TRANSLATE3F; flat_cmds[0].valid = 1; flat_cmds[0].src_cmd_idx = 0;
    flat_cmds[0].args[0] = 2.0f;
    flat_cmds[1].type = CMD_VERTEX3F;    flat_cmds[1].valid = 1; flat_cmds[1].src_cmd_idx = 1;
    flat_cmds[1].args[0] = 0.5f; flat_cmds[1].args[1] = 0.5f;

    Render3dGuideSnapshot snapshot =
        base_snapshot(source_cmds, 2, flat_cmds, 2, -1, "");
    snapshot.replaying = 1;
    snapshot.replay_focus_anchor_flat_idx = 1;
    snapshot.alpha_scale = 1.0f;
    snapshot.anim_time = 0.0f;
    snapshot.xform_guide_mode = RENDER3D_XFORM_GUIDE_FRAME;

    int prepared = render3d_transform_guides_prepare(&snapshot, &plan);
    ASSERT_INT("replay render: plan prepared", prepared, 1);
    ASSERT_INT("replay render: focus is the translate", plan.cursor_flat_idx, 0);

    float cam_view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    /* Not due at the vertex flat idx. */
    gl_stub_counts_reset();
    render3d_transform_guides_render_if_due(&snapshot, &plan, 1, cam_view);
    ASSERT_INT("replay render: nothing drawn off the focus index",
               (int)gl_stub_counts[GL_STUB_glBegin], 0);

    /* Due at the transform flat idx → draws the translate guide. */
    gl_stub_counts_reset();
    render3d_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
    ASSERT_TRUE("replay render: translate guide draws at the transform idx",
                gl_stub_counts[GL_STUB_glBegin] > 0);
    ASSERT_INT("replay render: plan consumed after draw", plan.consumed, 1);
}

#include "render3d/guides/geometry_guides.h"

static void test_geometry_guides_render(void) {
    printf("--- geometry guides rendering ---\n");

    /* 1. 2-DOF vertex plane guide */
    {
        Render3dGuideSnapshot snapshot = {0};
        TestGuideLabelCapture capture;
        snapshot.show_guides = 1;
        snapshot.input = "glVertex3f(1.0,";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.vertex_n_filled = 1;
        snapshot.vertex_filled[0] = 1;
        snapshot.vertex_args[0] = 1.0f;
        snapshot.alpha_scale = 1.0f;
        install_test_label_sink(&snapshot, &capture);

        render3d_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_INT("vertex plane records one coordinate label", capture.count, 1);
        ASSERT_TRUE("vertex plane label names the pinned coordinate",
                    strcmp(capture.labels[0].text[0], "x = 1") == 0);
    }

    /* 2. 1-DOF vertex line guide */
    {
        Render3dGuideSnapshot snapshot = {0};
        TestGuideLabelCapture capture;
        snapshot.show_guides = 1;
        snapshot.input = "glVertex3f(1.0, 2.0,";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.vertex_n_filled = 2;
        snapshot.vertex_filled[0] = 1;
        snapshot.vertex_filled[1] = 1;
        snapshot.vertex_filled[2] = 0;
        snapshot.vertex_args[0] = 1.0f;
        snapshot.vertex_args[1] = 2.0f;
        snapshot.vertex_args[2] = 0.0f;
        snapshot.alpha_scale = 1.0f;
        install_test_label_sink(&snapshot, &capture);

        gl_stub_counts_reset();
        render3d_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_TRUE("1-DOF vertex line guide renders", gl_stub_counts[GL_STUB_glBegin] > 0);
        ASSERT_INT("vertex line records one free-axis label", capture.count, 1);
        ASSERT_TRUE("vertex line label names the free axis",
                    strcmp(capture.labels[0].text[0], "z") == 0);
    }

    /* 3. Normal guide with valid base pos (normal_base_pos_valid = 1) */
    {
        Render3dGuideSnapshot snapshot = {0};
        TestGuideLabelCapture capture;
        snapshot.show_guides = 1;
        snapshot.input = "glNormal3f(0.0, 1.0, 0.0)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.cursor_pos = 18;
        snapshot.normal_n_filled = 3;
        snapshot.normal_args[0] = 0.0f;
        snapshot.normal_args[1] = 1.0f;
        snapshot.normal_args[2] = 0.0f;
        snapshot.normal_base_pos[0] = 1.0f;
        snapshot.normal_base_pos[1] = 2.0f;
        snapshot.normal_base_pos[2] = 3.0f;
        snapshot.normal_base_pos_valid = 1;
        snapshot.alpha_scale = 1.0f;
        install_test_label_sink(&snapshot, &capture);

        gl_stub_counts_reset();
        render3d_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_TRUE("normal guide with base pos renders", gl_stub_counts[GL_STUB_glBegin] > 0);
        ASSERT_INT("normal guide records one compound label", capture.count, 1);
        ASSERT_INT("normal guide label has primary and detail runs",
                   capture.labels[0].run_count, 2);
        ASSERT_TRUE("normal guide records the n prefix",
                    strcmp(capture.labels[0].text[0], " n") == 0);
        ASSERT_TRUE("normal guide records direction detail",
                    strstr(capture.labels[0].text[1], "(0.00, 1.00, 0.00)") != NULL);
    }

    /* 4. Raster-position guide uses the same breathing point marker as a
     * fully specified vertex guide. */
    {
        Render3dGuideSnapshot snapshot = {0};
        snapshot.show_guides = 1;
        snapshot.input = "glRasterPos3f(1.0, 2.0, 3.0)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.raster_pos_n_filled = 3;
        snapshot.raster_pos_args[0] = 1.0f;
        snapshot.raster_pos_args[1] = 2.0f;
        snapshot.raster_pos_args[2] = 3.0f;
        snapshot.alpha_scale = 1.0f;

        gl_stub_counts_reset();
        render3d_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_TRUE("raster-pos guide renders point marker",
                    gl_stub_counts[GL_STUB_glBegin] > 0);
        ASSERT_INT("raster-pos marker emits halo and core vertices",
                   (int)gl_stub_counts[GL_STUB_glVertex3f], 2);
    }

    /* 5. Normal guide with valid source search fallback */
    {
        GLCmd source_cmds[2] = {0};
        source_cmds[0].type = CMD_NORMAL3F;
        source_cmds[0].valid = 1;
        source_cmds[1].type = CMD_VERTEX3F;
        source_cmds[1].valid = 1;
        source_cmds[1].args[0] = 4.0f;
        source_cmds[1].args[1] = 5.0f;
        source_cmds[1].args[2] = 6.0f;

        Render3dGuideSnapshot snapshot = {0};
        snapshot.show_guides = 1;
        snapshot.input = "glNormal3f(0.0, 1.0, 0.0)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.cursor_pos = 18;
        snapshot.normal_n_filled = 3;
        snapshot.normal_args[0] = 0.0f;
        snapshot.normal_args[1] = 1.0f;
        snapshot.normal_args[2] = 0.0f;
        snapshot.edit_line_idx = 0;
        snapshot.source_cmds = source_cmds;
        snapshot.source_cmd_count = 2;
        snapshot.alpha_scale = 1.0f;

        gl_stub_counts_reset();
        render3d_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_TRUE("normal guide with source fallback renders", gl_stub_counts[GL_STUB_glBegin] > 0);
    }

    /* 6. Normal guide with invalid/no anchor found */
    {
        GLCmd source_cmds[1] = {0};
        source_cmds[0].type = CMD_NORMAL3F;
        source_cmds[0].valid = 1;

        Render3dGuideSnapshot snapshot = {0};
        snapshot.show_guides = 1;
        snapshot.input = "glNormal3f(0.0, 1.0, 0.0)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.cursor_pos = 18;
        snapshot.normal_n_filled = 3;
        snapshot.normal_args[0] = 0.0f;
        snapshot.normal_args[1] = 1.0f;
        snapshot.normal_args[2] = 0.0f;
        snapshot.edit_line_idx = 0;
        snapshot.source_cmds = source_cmds;
        snapshot.source_cmd_count = 1;
        snapshot.alpha_scale = 1.0f;

        gl_stub_counts_reset();
        render3d_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_INT("normal guide with no vertex does not render", (int)gl_stub_counts[GL_STUB_glBegin], 0);
    }

    /* 7. Clip-plane guide compound label. */
    {
        Render3dGuideSnapshot snapshot = {0};
        TestGuideLabelCapture capture;
        snapshot.show_guides = 1;
        snapshot.input = "glClipPlane(GL_CLIP_PLANE0, (GLdouble[]){0,1,0,0})";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.clip_plane_idx = 0;
        snapshot.clip_plane_args[1] = 1.0f;
        snapshot.clip_plane_n_filled = 4;
        snapshot.clip_plane_cap_enabled = 1;
        snapshot.alpha_scale = 1.0f;
        install_test_label_sink(&snapshot, &capture);

        render3d_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_INT("clip plane records one compound label", capture.count, 1);
        ASSERT_INT("clip-plane label has primary and detail runs",
                   capture.labels[0].run_count, 2);
        ASSERT_TRUE("clip-plane label records its slot",
                    strcmp(capture.labels[0].text[0], " P0") == 0);
        ASSERT_TRUE("clip-plane label records its equation",
                    strstr(capture.labels[0].text[1], "(0.00, 1.00, 0.00, 0.00)") != NULL);
    }
}
#endif

int main(void) {
    printf("--- scene guide planner tests ---\n");

    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_TRANSLATE3F;
        source_cmds[0].valid = 1;
        flat_cmds[0].type = CMD_TRANSLATE3F;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0,
                          "glTranslatef(1,2,3)");
        snapshot.edit_line_committed_text = "glTranslatef(1,2,3);";
        snapshot.replaying = 1;
        /* Replaying with no valid focus vertex (the default flat idx 0 is a
         * transform, not a vertex) produces no plan — req 6 only guides when a
         * replay vertex is in focus. */
        ASSERT_INT("replay without a focus vertex produces no plan",
                   render3d_transform_guides_prepare(&snapshot, &plan), 0);
        ASSERT_INT("plan inactive without a focus vertex", plan.active, 0);

        snapshot.replaying = 0;
        snapshot.show_guides = 0;
        ASSERT_INT("prepare disabled when guides hidden",
                   render3d_transform_guides_prepare(&snapshot, &plan), 0);
        ASSERT_INT("plan inactive when guides hidden", plan.active, 0);

        snapshot.show_guides = 1;
        snapshot.edit_line_idx = -1;
        ASSERT_INT("prepare disabled for invalid edit line",
                   render3d_transform_guides_prepare(&snapshot, &plan), 0);
        ASSERT_INT("plan inactive for invalid edit line", plan.active, 0);
    }

    {
        GLCmd source_cmds[3] = {0};
        GLCmd flat_cmds[3] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_TRANSLATE3F;
        source_cmds[0].valid = 0;
        flat_cmds[0].type = CMD_TRANSLATE3F;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 3, flat_cmds, 1, 0,
                          "glTranslatef(1,2,3)");
        snapshot.edit_line_committed_text = "glTranslatef(1,2,3);";
        ASSERT_INT("prepare rejects invalid source cmd",
                   render3d_transform_guides_prepare(&snapshot, &plan), 0);

        source_cmds[0].valid = 1;
        source_cmds[0].type = CMD_VERTEX3F;
        snapshot.input = "glVertex3f(1,2,3)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glVertex3f(1,2,3);";
        ASSERT_INT("prepare rejects non-transform source cmd",
                   render3d_transform_guides_prepare(&snapshot, &plan), 0);

        source_cmds[0].type = CMD_SCALEF;
        flat_cmds[0].type = CMD_SCALEF;
        snapshot.input = "glScalef(2,2,3)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glScalef(2,2,2);";
        /* Modified-but-still-a-transform input now activates the live guide
         * (it tracks the cursor args before commit) rather than hiding until
         * the line matches the committed source again. */
        ASSERT_INT("prepare activates for live-edited transform text",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("plan active for live-edited transform", plan.active, 1);

        source_cmds[0].type = CMD_TRANSLATE3F;
        flat_cmds[0].type = CMD_TRANSLATE3F;
        snapshot.input = "glTranslatef(1,2,3)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glTranslatef(1,2,3);";
        ASSERT_INT("prepare accepts translate",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("plan active for translate", plan.active, 1);

        source_cmds[0].type = CMD_SCALEF;
        flat_cmds[0].type = CMD_SCALEF;
        snapshot.input = "glScalef(2,2,2)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glScalef(2,2,2);";
        ASSERT_INT("prepare accepts scale",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);

        source_cmds[0].type = CMD_ROTATEF;
        flat_cmds[0].type = CMD_ROTATEF;
        snapshot.input = "glRotatef(45,0,1,0)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glRotatef(45,0,1,0);";
        ASSERT_INT("prepare accepts rotate",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);
    }

    {
        GLCmd source_cmds[3] = {0};
        GLCmd flat_cmds[5] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[1].type = CMD_TRANSLATE3F;
        source_cmds[1].valid = 1;

        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[0].type = CMD_VERTEX3F;
        flat_cmds[1].valid = 1;
        flat_cmds[1].src_cmd_idx = 1;
        flat_cmds[1].type = CMD_TRANSLATE3F;
        flat_cmds[2].valid = 1;
        flat_cmds[2].src_cmd_idx = 1;
        flat_cmds[2].type = CMD_VERTEX3F;
        flat_cmds[3].valid = 1;
        flat_cmds[3].src_cmd_idx = 2;
        flat_cmds[3].type = CMD_VERTEX3F;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 3, flat_cmds, 4, 1,
                          "glTranslatef(1,2,3)");
        snapshot.edit_line_committed_text = "glTranslatef(1,2,3);";
        ASSERT_INT("prepare activates for indexed flat stream",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("cursor flat idx picks first matching src cmd",
                   plan.cursor_flat_idx, 1);
        ASSERT_INT("after flat idx picks first different source",
                   plan.after_flat_idx, 3);
        ASSERT_INT("plan consumed reset on prepare", plan.consumed, 0);
    }

    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_SCALEF;
        source_cmds[0].valid = 1;

        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[0].type = CMD_SCALEF;
        flat_cmds[1].valid = 1;
        flat_cmds[1].src_cmd_idx = 0;
        flat_cmds[1].type = CMD_VERTEX3F;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 2, 0,
                          "glScalef(2,2,2)");
        snapshot.edit_line_committed_text = "glScalef(2,2,2);";
        ASSERT_INT("prepare activates when only cursor-source cmds remain",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("after index falls back to flat count",
                   plan.after_flat_idx, 2);
    }

    /* Req 6: replay path builds a transform-guide plan anchored on the
     * replay-focused vertex. Flat program:
     *   0 translate (src 0)   1 rotate (src 1)   2 vertex (src 2)
     * with the replay focus on the vertex at flat idx 2. */
    {
        GLCmd source_cmds[3] = {0};
        GLCmd flat_cmds[3] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_TRANSLATE3F; source_cmds[0].valid = 1;
        source_cmds[1].type = CMD_ROTATEF;     source_cmds[1].valid = 1;
        source_cmds[2].type = CMD_VERTEX3F;    source_cmds[2].valid = 1;

        flat_cmds[0].type = CMD_TRANSLATE3F; flat_cmds[0].valid = 1; flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[1].type = CMD_ROTATEF;     flat_cmds[1].valid = 1; flat_cmds[1].src_cmd_idx = 1;
        flat_cmds[1].args[0] = 45.0f; flat_cmds[1].args[2] = 1.0f; /* 45deg about +Y */
        flat_cmds[2].type = CMD_VERTEX3F;    flat_cmds[2].valid = 1; flat_cmds[2].src_cmd_idx = 2;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 3, flat_cmds, 3, -1, "");
        snapshot.replaying = 1;
        snapshot.replay_focus_anchor_flat_idx = 2;

        /* (b) Default: no cursor transform selected → nearest in-scope
         * affecting transform before the vertex is the rotate (flat idx 1). */
        ASSERT_INT("replay default prepares a plan",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("replay default focuses nearest transform (rotate)",
                   plan.cursor_flat_idx, 1);
        /* after-cursor anchor = first following flat cmd from a different
         * source (the vertex at idx 2), exactly like the edit-mode plan — the
         * guide draws in the transform's frame, not on the vertex. */
        ASSERT_INT("replay plan after-anchor is next different-src cmd",
                   plan.after_flat_idx, 2);
        ASSERT_INT("replay plan active", plan.active, 1);

        /* (a) Cursor parked on the translate source line (idx 0) → focus the
         * matching flat translate (idx 0) instead of the nearest rotate. */
        snapshot.edit_line_idx = 0;
        snapshot.input = "glTranslatef(1,2,3)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glTranslatef(1,2,3);";
        ASSERT_INT("replay cursor-on-transform prepares a plan",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("replay cursor focuses the cursor's transform (translate)",
                   plan.cursor_flat_idx, 0);

        /* Cursor on a non-transform line falls back to the nearest. */
        snapshot.edit_line_idx = 2; /* the vertex line */
        snapshot.input = "glVertex3f(0,0,0)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glVertex3f(0,0,0);";
        ASSERT_INT("replay cursor-on-non-transform falls back to nearest",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("fallback focuses nearest transform (rotate)",
                   plan.cursor_flat_idx, 1);
    }

    /* Req 6: replay with no affecting transform before the vertex → no plan. */
    {
        GLCmd source_cmds[1] = {0};
        GLCmd flat_cmds[1] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_VERTEX3F; source_cmds[0].valid = 1;
        flat_cmds[0].type = CMD_VERTEX3F;   flat_cmds[0].valid = 1; flat_cmds[0].src_cmd_idx = 0;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 1, flat_cmds, 1, -1, "");
        snapshot.replaying = 1;
        snapshot.replay_focus_anchor_flat_idx = 0;
        ASSERT_INT("replay vertex with no transforms → no plan",
                   render3d_transform_guides_prepare(&snapshot, &plan), 0);
        ASSERT_INT("no plan is inactive", plan.active, 0);
    }

    /* Req 6: a glPushMatrix/glPopMatrix-popped transform does not affect a
     * later vertex, so it isn't chosen. Flat program:
     *   0 push  1 translate(src1)  2 pop  3 rotate(src3)  4 vertex(src4) */
    {
        GLCmd source_cmds[5] = {0};
        GLCmd flat_cmds[5] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[1].type = CMD_TRANSLATE3F; source_cmds[1].valid = 1;
        source_cmds[3].type = CMD_ROTATEF;     source_cmds[3].valid = 1;
        source_cmds[4].type = CMD_VERTEX3F;    source_cmds[4].valid = 1;

        flat_cmds[0].type = CMD_PUSH_MATRIX; flat_cmds[0].valid = 1; flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[1].type = CMD_TRANSLATE3F; flat_cmds[1].valid = 1; flat_cmds[1].src_cmd_idx = 1;
        flat_cmds[2].type = CMD_POP_MATRIX;  flat_cmds[2].valid = 1; flat_cmds[2].src_cmd_idx = 2;
        flat_cmds[3].type = CMD_ROTATEF;     flat_cmds[3].valid = 1; flat_cmds[3].src_cmd_idx = 3;
        flat_cmds[3].args[0] = 30.0f; flat_cmds[3].args[2] = 1.0f;
        flat_cmds[4].type = CMD_VERTEX3F;    flat_cmds[4].valid = 1; flat_cmds[4].src_cmd_idx = 4;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 5, flat_cmds, 5, -1, "");
        snapshot.replaying = 1;
        snapshot.replay_focus_anchor_flat_idx = 4;
        ASSERT_INT("replay skips popped transform, prepares a plan",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("popped translate excluded; rotate chosen",
                   plan.cursor_flat_idx, 3);
    }

    /* The replay anchor can be a glutSolid* (no REPL vertex). It still picks up
     * the affecting transform and prepares a plan exactly like a vertex anchor.
     * Flat program: 0 translate(src0)  1 glutSolidCube(src1). */
    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        Render3dTransformGuidePlan plan;

        source_cmds[0].type = CMD_TRANSLATE3F; source_cmds[0].valid = 1;
        source_cmds[1].type = CMD_GLUT_CUBE;   source_cmds[1].valid = 1;

        flat_cmds[0].type = CMD_TRANSLATE3F; flat_cmds[0].valid = 1; flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[1].type = CMD_GLUT_CUBE;   flat_cmds[1].valid = 1; flat_cmds[1].src_cmd_idx = 1;

        Render3dGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 2, -1, "");
        snapshot.replaying = 1;
        snapshot.replay_focus_anchor_flat_idx = 1; /* the glut solid */
        ASSERT_INT("replay glut-solid anchor prepares a plan",
                   render3d_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("glut-solid anchor focuses the translate",
                   plan.cursor_flat_idx, 0);
        ASSERT_INT("glut-solid anchor plan active", plan.active, 1);
    }

#ifdef GL_STUBS
    test_transform_guides_render();
    test_replay_transform_guide_render();
    test_geometry_guides_render();
#endif

    printf("%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return g_harness.passed == g_harness.run ? 0 : 1;
}
