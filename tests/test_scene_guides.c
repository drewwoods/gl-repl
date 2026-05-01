#include "scene_transform_guides.h"

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
        ASSERT_INT("prepare disabled while replaying",
                   scene_transform_guides_prepare(&snapshot, &plan), 0);
        ASSERT_INT("plan inactive while replaying", plan.active, 0);

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

    printf("%d / %d tests passed\n", g_harness.passed, g_harness.run);
    return g_harness.passed == g_harness.run ? 0 : 1;
}
