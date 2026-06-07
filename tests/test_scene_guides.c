#include "scene/guides/transform_guides.h"
#include "gl_includes.h"
#ifdef GL_STUBS
#include <GL/gl_stub_counts.h>
#endif

#include "support/test_harness.h"
#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) do { \
    TEST_ASSERT_TRUE(&g_harness, label, cond); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    TEST_ASSERT_INT(&g_harness, label, got, exp); \
} while (0)

static SceneGuideSnapshot base_snapshot(const GLCmd *source_cmds,
                                        int source_count,
                                        const GLCmd *flat_cmds,
                                        int flat_count,
                                        int edit_line_idx,
                                        const char *input) {
    SceneGuideSnapshot snapshot = {0};
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
        SceneTransformGuidePlan plan;

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

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0,
                          "glTranslatef(2,0,0)");
        snapshot.edit_line_committed_text = "glTranslatef(2,0,0);";
        snapshot.alpha_scale = 1.0f;
        snapshot.anim_time = 0.0f;
        snapshot.xform_guide_mode = SCENE_XFORM_GUIDE_FRAME;

        int prepared = scene_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("translate prepared", prepared, 1);

        float cam_view[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

        gl_stub_counts_reset();
        scene_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
        ASSERT_TRUE("translate rendering calls glBegin", gl_stub_counts[GL_STUB_glBegin] > 0);
        ASSERT_TRUE("translate rendering calls glPushAttrib", gl_stub_counts[GL_STUB_glPushAttrib] > 0);
    }

    /* 2. Scale guide rendering */
    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        SceneTransformGuidePlan plan;

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

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0,
                          "glScalef(2,2,2)");
        snapshot.edit_line_committed_text = "glScalef(2,2,2);";
        snapshot.alpha_scale = 1.0f;
        snapshot.anim_time = 0.0f;
        snapshot.xform_guide_mode = SCENE_XFORM_GUIDE_FRAME;

        int prepared = scene_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("scale prepared", prepared, 1);

        float cam_view[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

        gl_stub_counts_reset();
        scene_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
        ASSERT_TRUE("scale rendering calls glBegin", gl_stub_counts[GL_STUB_glBegin] > 0);
    }

    /* 3. Rotate guide rendering */
    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        SceneTransformGuidePlan plan;

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

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0,
                          "glRotatef(45,0,1,0)");
        snapshot.edit_line_committed_text = "glRotatef(45,0,1,0);";
        snapshot.alpha_scale = 1.0f;
        snapshot.anim_time = 0.0f;
        snapshot.xform_guide_mode = SCENE_XFORM_GUIDE_FRAME;

        int prepared = scene_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("rotate prepared", prepared, 1);

        float cam_view[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

        gl_stub_counts_reset();
        scene_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
        ASSERT_TRUE("rotate rendering calls glBegin", gl_stub_counts[GL_STUB_glBegin] > 0);
    }

    /* 4. World-aligned translation guide rendering (exercises compute_after_cursor_origin) */
    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[3] = {0};
        SceneTransformGuidePlan plan;

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

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 2, 0,
                          "glTranslatef(2,0,0)");
        snapshot.edit_line_committed_text = "glTranslatef(2,0,0);";
        snapshot.alpha_scale = 1.0f;
        snapshot.anim_time = 0.0f;
        snapshot.xform_guide_mode = SCENE_XFORM_GUIDE_WORLD;

        int prepared = scene_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("world-aligned translate prepared", prepared, 1);
        ASSERT_INT("after_flat_idx set correctly", plan.after_flat_idx, 1);

        float cam_view[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

        gl_stub_counts_reset();
        scene_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
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
        SceneTransformGuidePlan plan;

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

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0,
                          "glRotatef(45,0,1,0)");
        snapshot.edit_line_committed_text = "glRotatef(45,0,1,0);";
        snapshot.alpha_scale = 1.0f;
        snapshot.anim_time = 0.0f;
        snapshot.xform_guide_mode = SCENE_XFORM_GUIDE_WORLD;

        int prepared = scene_transform_guides_prepare(&snapshot, &plan);
        ASSERT_INT("rotate with origin prepared", prepared, 1);

        float cam_view[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

        gl_stub_counts_reset();
        scene_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
        ASSERT_TRUE("rotate with origin rendering calls glBegin", gl_stub_counts[GL_STUB_glBegin] > 0);

        /* Reset stub matrix to identity */
        g_gl_stub_modelview_matrix[12] = 0.0f;
    }
}

/* Req 6: during replay the prepared plan's transform guide renders when the
 * walk reaches the chosen transform's flat index (not the vertex). */
static void test_replay_transform_guide_render(void) {
    printf("--- replay transform guide rendering ---\n");

    GLCmd source_cmds[3] = {0};
    GLCmd flat_cmds[3] = {0};
    SceneTransformGuidePlan plan;

    source_cmds[0].type = CMD_TRANSLATE3F; source_cmds[0].valid = 1;
    source_cmds[1].type = CMD_VERTEX3F;    source_cmds[1].valid = 1;

    flat_cmds[0].type = CMD_TRANSLATE3F; flat_cmds[0].valid = 1; flat_cmds[0].src_cmd_idx = 0;
    flat_cmds[0].args[0] = 2.0f;
    flat_cmds[1].type = CMD_VERTEX3F;    flat_cmds[1].valid = 1; flat_cmds[1].src_cmd_idx = 1;
    flat_cmds[1].args[0] = 0.5f; flat_cmds[1].args[1] = 0.5f;

    SceneGuideSnapshot snapshot =
        base_snapshot(source_cmds, 2, flat_cmds, 2, -1, "");
    snapshot.replaying = 1;
    snapshot.replay_focus_vertex_flat_idx = 1;
    snapshot.alpha_scale = 1.0f;
    snapshot.anim_time = 0.0f;
    snapshot.xform_guide_mode = SCENE_XFORM_GUIDE_FRAME;

    int prepared = scene_transform_guides_prepare(&snapshot, &plan);
    ASSERT_INT("replay render: plan prepared", prepared, 1);
    ASSERT_INT("replay render: focus is the translate", plan.cursor_flat_idx, 0);

    float cam_view[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    /* Not due at the vertex flat idx. */
    gl_stub_counts_reset();
    scene_transform_guides_render_if_due(&snapshot, &plan, 1, cam_view);
    ASSERT_INT("replay render: nothing drawn off the focus index",
               (int)gl_stub_counts[GL_STUB_glBegin], 0);

    /* Due at the transform flat idx → draws the translate guide. */
    gl_stub_counts_reset();
    scene_transform_guides_render_if_due(&snapshot, &plan, 0, cam_view);
    ASSERT_TRUE("replay render: translate guide draws at the transform idx",
                gl_stub_counts[GL_STUB_glBegin] > 0);
    ASSERT_INT("replay render: plan consumed after draw", plan.consumed, 1);
}

#include "scene/guides/geometry_guides.h"

static void test_geometry_guides_render(void) {
    printf("--- geometry guides rendering ---\n");

    /* 1. 1-DOF vertex line guide */
    {
        SceneGuideSnapshot snapshot = {0};
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

        gl_stub_counts_reset();
        scene_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_TRUE("1-DOF vertex line guide renders", gl_stub_counts[GL_STUB_glBegin] > 0);
    }

    /* 2. Normal guide with valid base pos (normal_base_pos_valid = 1) */
    {
        SceneGuideSnapshot snapshot = {0};
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

        gl_stub_counts_reset();
        scene_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_TRUE("normal guide with base pos renders", gl_stub_counts[GL_STUB_glBegin] > 0);
    }

    /* 3. Normal guide with valid source search fallback */
    {
        GLCmd source_cmds[2] = {0};
        source_cmds[0].type = CMD_NORMAL3F;
        source_cmds[0].valid = 1;
        source_cmds[1].type = CMD_VERTEX3F;
        source_cmds[1].valid = 1;
        source_cmds[1].args[0] = 4.0f;
        source_cmds[1].args[1] = 5.0f;
        source_cmds[1].args[2] = 6.0f;

        SceneGuideSnapshot snapshot = {0};
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
        scene_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_TRUE("normal guide with source fallback renders", gl_stub_counts[GL_STUB_glBegin] > 0);
    }

    /* 4. Normal guide with invalid/no anchor found */
    {
        GLCmd source_cmds[1] = {0};
        source_cmds[0].type = CMD_NORMAL3F;
        source_cmds[0].valid = 1;

        SceneGuideSnapshot snapshot = {0};
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
        scene_geometry_guides_render_for_cursor(&snapshot);
        ASSERT_INT("normal guide with no vertex does not render", (int)gl_stub_counts[GL_STUB_glBegin], 0);
    }
}
#endif

int main(void) {
    printf("--- scene guide planner tests ---\n");

    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        SceneTransformGuidePlan plan;

        source_cmds[0].type = CMD_TRANSLATE3F;
        source_cmds[0].valid = 1;
        flat_cmds[0].type = CMD_TRANSLATE3F;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 1, 0,
                          "glTranslatef(1,2,3)");
        snapshot.edit_line_committed_text = "glTranslatef(1,2,3);";
        snapshot.replaying = 1;
        /* Replaying with no valid focus vertex (the default flat idx 0 is a
         * transform, not a vertex) produces no plan — req 6 only guides when a
         * replay vertex is in focus. */
        ASSERT_INT("replay without a focus vertex produces no plan",
                   scene_transform_guides_prepare(&snapshot, &plan), 0);
        ASSERT_INT("plan inactive without a focus vertex", plan.active, 0);

        snapshot.replaying = 0;
        snapshot.show_guides = 0;
        ASSERT_INT("prepare disabled when guides hidden",
                   scene_transform_guides_prepare(&snapshot, &plan), 0);
        ASSERT_INT("plan inactive when guides hidden", plan.active, 0);

        snapshot.show_guides = 1;
        snapshot.edit_line_idx = -1;
        ASSERT_INT("prepare disabled for invalid edit line",
                   scene_transform_guides_prepare(&snapshot, &plan), 0);
        ASSERT_INT("plan inactive for invalid edit line", plan.active, 0);
    }

    {
        GLCmd source_cmds[3] = {0};
        GLCmd flat_cmds[3] = {0};
        SceneTransformGuidePlan plan;

        source_cmds[0].type = CMD_TRANSLATE3F;
        source_cmds[0].valid = 0;
        flat_cmds[0].type = CMD_TRANSLATE3F;
        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 3, flat_cmds, 1, 0,
                          "glTranslatef(1,2,3)");
        snapshot.edit_line_committed_text = "glTranslatef(1,2,3);";
        ASSERT_INT("prepare rejects invalid source cmd",
                   scene_transform_guides_prepare(&snapshot, &plan), 0);

        source_cmds[0].valid = 1;
        source_cmds[0].type = CMD_VERTEX3F;
        snapshot.input = "glVertex3f(1,2,3)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glVertex3f(1,2,3);";
        ASSERT_INT("prepare rejects non-transform source cmd",
                   scene_transform_guides_prepare(&snapshot, &plan), 0);

        source_cmds[0].type = CMD_SCALEF;
        snapshot.input = "glScalef(2,2,3)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glScalef(2,2,2);";
        ASSERT_INT("prepare rejects modified source text",
                   scene_transform_guides_prepare(&snapshot, &plan), 0);

        source_cmds[0].type = CMD_TRANSLATE3F;
        flat_cmds[0].type = CMD_TRANSLATE3F;
        snapshot.input = "glTranslatef(1,2,3)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glTranslatef(1,2,3);";
        ASSERT_INT("prepare accepts translate",
                   scene_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("plan active for translate", plan.active, 1);

        source_cmds[0].type = CMD_SCALEF;
        flat_cmds[0].type = CMD_SCALEF;
        snapshot.input = "glScalef(2,2,2)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glScalef(2,2,2);";
        ASSERT_INT("prepare accepts scale",
                   scene_transform_guides_prepare(&snapshot, &plan), 1);

        source_cmds[0].type = CMD_ROTATEF;
        flat_cmds[0].type = CMD_ROTATEF;
        snapshot.input = "glRotatef(45,0,1,0)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glRotatef(45,0,1,0);";
        ASSERT_INT("prepare accepts rotate",
                   scene_transform_guides_prepare(&snapshot, &plan), 1);
    }

    {
        GLCmd source_cmds[3] = {0};
        GLCmd flat_cmds[5] = {0};
        SceneTransformGuidePlan plan;

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

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 3, flat_cmds, 4, 1,
                          "glTranslatef(1,2,3)");
        snapshot.edit_line_committed_text = "glTranslatef(1,2,3);";
        ASSERT_INT("prepare activates for indexed flat stream",
                   scene_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("cursor flat idx picks first matching src cmd",
                   plan.cursor_flat_idx, 1);
        ASSERT_INT("after flat idx picks first different source",
                   plan.after_flat_idx, 3);
        ASSERT_INT("plan consumed reset on prepare", plan.consumed, 0);
    }

    {
        GLCmd source_cmds[2] = {0};
        GLCmd flat_cmds[2] = {0};
        SceneTransformGuidePlan plan;

        source_cmds[0].type = CMD_SCALEF;
        source_cmds[0].valid = 1;

        flat_cmds[0].valid = 1;
        flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[0].type = CMD_SCALEF;
        flat_cmds[1].valid = 1;
        flat_cmds[1].src_cmd_idx = 0;
        flat_cmds[1].type = CMD_VERTEX3F;

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 2, flat_cmds, 2, 0,
                          "glScalef(2,2,2)");
        snapshot.edit_line_committed_text = "glScalef(2,2,2);";
        ASSERT_INT("prepare activates when only cursor-source cmds remain",
                   scene_transform_guides_prepare(&snapshot, &plan), 1);
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
        SceneTransformGuidePlan plan;

        source_cmds[0].type = CMD_TRANSLATE3F; source_cmds[0].valid = 1;
        source_cmds[1].type = CMD_ROTATEF;     source_cmds[1].valid = 1;
        source_cmds[2].type = CMD_VERTEX3F;    source_cmds[2].valid = 1;

        flat_cmds[0].type = CMD_TRANSLATE3F; flat_cmds[0].valid = 1; flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[1].type = CMD_ROTATEF;     flat_cmds[1].valid = 1; flat_cmds[1].src_cmd_idx = 1;
        flat_cmds[1].args[0] = 45.0f; flat_cmds[1].args[2] = 1.0f; /* 45deg about +Y */
        flat_cmds[2].type = CMD_VERTEX3F;    flat_cmds[2].valid = 1; flat_cmds[2].src_cmd_idx = 2;

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 3, flat_cmds, 3, -1, "");
        snapshot.replaying = 1;
        snapshot.replay_focus_vertex_flat_idx = 2;

        /* (b) Default: no cursor transform selected → nearest in-scope
         * affecting transform before the vertex is the rotate (flat idx 1). */
        ASSERT_INT("replay default prepares a plan",
                   scene_transform_guides_prepare(&snapshot, &plan), 1);
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
                   scene_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("replay cursor focuses the cursor's transform (translate)",
                   plan.cursor_flat_idx, 0);

        /* Cursor on a non-transform line falls back to the nearest. */
        snapshot.edit_line_idx = 2; /* the vertex line */
        snapshot.input = "glVertex3f(0,0,0)";
        snapshot.input_len = (int)strlen(snapshot.input);
        snapshot.edit_line_committed_text = "glVertex3f(0,0,0);";
        ASSERT_INT("replay cursor-on-non-transform falls back to nearest",
                   scene_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("fallback focuses nearest transform (rotate)",
                   plan.cursor_flat_idx, 1);
    }

    /* Req 6: replay with no affecting transform before the vertex → no plan. */
    {
        GLCmd source_cmds[1] = {0};
        GLCmd flat_cmds[1] = {0};
        SceneTransformGuidePlan plan;

        source_cmds[0].type = CMD_VERTEX3F; source_cmds[0].valid = 1;
        flat_cmds[0].type = CMD_VERTEX3F;   flat_cmds[0].valid = 1; flat_cmds[0].src_cmd_idx = 0;

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 1, flat_cmds, 1, -1, "");
        snapshot.replaying = 1;
        snapshot.replay_focus_vertex_flat_idx = 0;
        ASSERT_INT("replay vertex with no transforms → no plan",
                   scene_transform_guides_prepare(&snapshot, &plan), 0);
        ASSERT_INT("no plan is inactive", plan.active, 0);
    }

    /* Req 6: a glPushMatrix/glPopMatrix-popped transform does not affect a
     * later vertex, so it isn't chosen. Flat program:
     *   0 push  1 translate(src1)  2 pop  3 rotate(src3)  4 vertex(src4) */
    {
        GLCmd source_cmds[5] = {0};
        GLCmd flat_cmds[5] = {0};
        SceneTransformGuidePlan plan;

        source_cmds[1].type = CMD_TRANSLATE3F; source_cmds[1].valid = 1;
        source_cmds[3].type = CMD_ROTATEF;     source_cmds[3].valid = 1;
        source_cmds[4].type = CMD_VERTEX3F;    source_cmds[4].valid = 1;

        flat_cmds[0].type = CMD_PUSH_MATRIX; flat_cmds[0].valid = 1; flat_cmds[0].src_cmd_idx = 0;
        flat_cmds[1].type = CMD_TRANSLATE3F; flat_cmds[1].valid = 1; flat_cmds[1].src_cmd_idx = 1;
        flat_cmds[2].type = CMD_POP_MATRIX;  flat_cmds[2].valid = 1; flat_cmds[2].src_cmd_idx = 2;
        flat_cmds[3].type = CMD_ROTATEF;     flat_cmds[3].valid = 1; flat_cmds[3].src_cmd_idx = 3;
        flat_cmds[3].args[0] = 30.0f; flat_cmds[3].args[2] = 1.0f;
        flat_cmds[4].type = CMD_VERTEX3F;    flat_cmds[4].valid = 1; flat_cmds[4].src_cmd_idx = 4;

        SceneGuideSnapshot snapshot =
            base_snapshot(source_cmds, 5, flat_cmds, 5, -1, "");
        snapshot.replaying = 1;
        snapshot.replay_focus_vertex_flat_idx = 4;
        ASSERT_INT("replay skips popped transform, prepares a plan",
                   scene_transform_guides_prepare(&snapshot, &plan), 1);
        ASSERT_INT("popped translate excluded; rotate chosen",
                   plan.cursor_flat_idx, 3);
    }

#ifdef GL_STUBS
    test_transform_guides_render();
    test_replay_transform_guide_render();
    test_geometry_guides_render();
#endif

    printf("%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return g_harness.passed == g_harness.run ? 0 : 1;
}
