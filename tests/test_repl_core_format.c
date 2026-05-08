#include "repl_core_internal.h"
#include "repl_state.h"
#include "repl_core.h"
#include "ui/state.h"
#include "ui/layout.h"            /* CODE_PANEL_LAYOUT_* */
#include "glr_defaults.h"    /* CFG_DEFAULT_* */
#include "support/test_harness.h"

#define g_panel_frac (ui_state_code_panel_mut()->panel_frac)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("y", err, sizeof(err));
    repl_eval_declare_predef_var("z", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
}

int main(void) {
    const char *line = "glVertex3f(x+1, y, z);";
    const char *tmp_path = "/tmp/repl_core_format_input.c";
    const char *tmp_loop_path = "/tmp/repl_core_format_loop_input.c";
    const char *tmp_dump_path = "/tmp/repl_core_format_dump.txt";

    repl_eval_init_predef_vars();
    repl_reset_state();
    declare_test_vars();

    GLCmd interactive_cmd;
    char interactive_text[MAX_LINE_LEN] = "";
    memset(&interactive_cmd, 0, sizeof(interactive_cmd));
    ASSERT_TRUE("interactive parse normalize",
                repl_parse_and_normalize(line, 0, NULL, 0, 1, &interactive_cmd,
                                         interactive_text, sizeof(interactive_text)) == 1);

    repl_reset_state();
    declare_test_vars();
    repl_feed_line_public(line);
    ASSERT_TRUE("feed inserted one", repl_state_document_count() == 1);
    ASSERT_TRUE("feed matches interactive",
                strcmp(editor_buffer_line(0), interactive_text) == 0);

    {
        FILE *f = fopen(tmp_path, "w");
        ASSERT_TRUE("open tmp file", f != NULL);
        if (f) {
            fprintf(f, "// Snippet start\n");
            fprintf(f, "%s\n", line);
            fprintf(f, "// Snippet end\n");
            fclose(f);
        }
    }

    repl_reset_state();
    declare_test_vars();
    ASSERT_TRUE("load from file", repl_export_load_from_file(tmp_path) == 1);
    ASSERT_TRUE("load inserted one", repl_state_document_count() == 1);
    ASSERT_TRUE("load matches interactive",
                strcmp(editor_buffer_line(0), interactive_text) == 0);

    {
        char out[256];
        repl_normalize_from_parsed("      glVertex3f(1,2,3);",
                                   "  glVertex3f(x, y, z)   ",
                                   1, out, sizeof(out));
        ASSERT_TRUE("normalize preserves vars", strstr(out, "x") != NULL);
        ASSERT_TRUE("normalize has semicolon", out[strlen(out) - 1] == ';');
        ASSERT_TRUE("normalize keeps indent", out[0] == ' ' && out[1] == ' ');
    }

    {
        FILE *f = fopen(tmp_loop_path, "w");
        ASSERT_TRUE("open tmp loop file", f != NULL);
        if (f) {
            fprintf(f, "// Snippet start\n");
            fprintf(f, "for (float i = 0; i < 3; i += 1.0f) {\n");
            fprintf(f, "glVertex3f(i + x, 0, 0);\n");
            fprintf(f, "}\n");
            fprintf(f, "// Snippet end\n");
            fclose(f);
        }
    }

    repl_reset_state();
    declare_test_vars();
    ASSERT_TRUE("load loop file", repl_export_load_from_file(tmp_loop_path) == 1);
    ASSERT_TRUE("loop imported 3 cmds", repl_state_document_count() == 3);
    ASSERT_TRUE("loop header type", repl_state_document_cmds_mut()[0].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("loop body type", repl_state_document_cmds_mut()[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("loop body has vars", repl_state_document_cmds_mut()[1].has_vars == 1);
    ASSERT_TRUE("loop body keeps expression", strstr(editor_buffer_line(1), "i + x") != NULL);
    ASSERT_TRUE("loop body indented", strncmp(editor_buffer_line(1), "    ", 4) == 0);

    {
        editor_buffer_set_line(1, "glVertex3f(i+x,0,0)");
        editor_buffer_set_line(2, "}");
        repl_reformat_commands();
        const char *buf1 = editor_buffer_line(1);
        const char *buf2 = editor_buffer_line(2);
        ASSERT_TRUE("reformat body semicolon", buf1 && buf1[strlen(buf1) - 1] == ';');
        ASSERT_TRUE("reformat body indent", buf1 && strncmp(buf1, "    ", 4) == 0);
        ASSERT_TRUE("reformat body comma spacing", buf1 && strstr(buf1, ", 0, 0") != NULL);
        ASSERT_TRUE("reformat close brace indent", buf2 && strcmp(buf2, "  }") == 0);
    }

    repl_reset_state();
    declare_test_vars();
    repl_feed_line_public("for(i, 0, 3) {");
    repl_feed_line_public("x = i + 1;");
    repl_feed_line_public("}");
    ASSERT_TRUE("assign in loop cmd count", repl_state_document_count() == 3);
    ASSERT_TRUE("assign in loop type", repl_state_document_cmds_mut()[1].type == CMD_VAR_ASSIGN);
    ASSERT_TRUE("assign in loop indent", strncmp(editor_buffer_line(1), "    ", 4) == 0);
    ASSERT_TRUE("assign in loop keeps expression", strstr(editor_buffer_line(1), "i + 1") != NULL);
    {
        editor_buffer_set_line(1, "x=i+1");
        repl_reformat_commands();
        const char *buf1 = editor_buffer_line(1);
        ASSERT_TRUE("reformat assign in loop indent", buf1 && strncmp(buf1, "    ", 4) == 0);
        ASSERT_TRUE("reformat assign in loop semicolon",
                    buf1 && buf1[strlen(buf1) - 1] == ';');
    }

    {
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("glu sphere constant parse",
                    repl_parse_and_normalize("glutSolidSphere(0.25, 16, 12);", 0, NULL, 0, 0,
                                             &cmd, cmd_text, sizeof(cmd_text)) == 1);
        ASSERT_TRUE("glut sphere constant no quadric",
                    strstr(cmd_text, "g_quadric") == NULL);
        ASSERT_TRUE("glut sphere constant syntax",
                    strstr(cmd_text, "glutSolidSphere(0.25, 16, 12);") != NULL);
    }

    {
        GLCmd cmd;
        char cmd_text[MAX_LINE_LEN] = "";
        ExprVar vars[2] = {
            { "radius", 0.5f },
            { "stacks", 12.0f }
        };
        memset(&cmd, 0, sizeof(cmd));
        ASSERT_TRUE("glut sphere var parse",
                    repl_parse_and_normalize("glutSolidSphere(radius, 16, stacks);",
                                             0, vars, 2, 1, &cmd,
                                             cmd_text, sizeof(cmd_text)) == 1);
        ASSERT_TRUE("glut sphere var keeps radius", strstr(cmd_text, "radius") != NULL);
        ASSERT_TRUE("glut sphere var keeps stacks", strstr(cmd_text, "stacks") != NULL);
        ASSERT_TRUE("glut sphere var no quadric", strstr(cmd_text, "g_quadric") == NULL);
    }

    {
        char dump_buf[8192];
        FILE *dump_f = fopen(tmp_dump_path, "w");
        ASSERT_TRUE("open dump file", dump_f != NULL);
        if (dump_f) {
            repl_dump_code_panel_text(dump_f, editor_buffer_view());
            fclose(dump_f);
        }

        dump_f = fopen(tmp_dump_path, "r");
        ASSERT_TRUE("read dump file", dump_f != NULL);
        if (dump_f) {
            size_t n = fread(dump_buf, 1, sizeof(dump_buf) - 1, dump_f);
            dump_buf[n] = '\0';
            fclose(dump_f);
            ASSERT_TRUE("dump has source section",
                        strstr(dump_buf, "--- source ---") != NULL);
            ASSERT_TRUE("dump includes current formatted assignment",
                        strstr(dump_buf, "    x = i+1;") != NULL);
        }
    }

    repl_reset_state();
    repl_feed_line_public("float a = 2, b, c = 3;");
    ASSERT_TRUE("decl cmd count", repl_state_document_count() == 1);
    ASSERT_TRUE("decl cmd type", repl_state_document_cmds_mut()[0].type == CMD_VAR_DECLARE);
    editor_buffer_set_line(0, "float a=max(1, 2),b,c=abs(-3)// vars");
    repl_reformat_commands();
    {
        const char *buf0 = editor_buffer_line(0);
        ASSERT_TRUE("reformat decl keeps initializer text and comment",
                    buf0 && strcmp(buf0,
                                   "  float a = max(1, 2), b, c = abs(-3); // vars") == 0);
    }

    {
        const char *wrapped = "  glColor4f(0.125, 0.250, 0.500, 1.000);";
        char visual_buf[8192];
        FILE *dump_f = fopen(tmp_dump_path, "w");

        repl_reset_state();
        declare_test_vars();
        repl_state_presentation_mut()->wrap_at_comma = 1;
        repl_state_presentation_mut()->show_vertex_indices = 0; repl_state_sync_ui_chrome();
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; repl_state_sync_ui_chrome();
        ui_state_viewport_set_size(360, ui_state_viewport().window_h);
        g_panel_frac = 0.75f;

        memset(&repl_state_document_cmds_mut()[0], 0, sizeof(repl_state_document_cmds_mut()[0]));
        repl_state_document_cmds_mut()[0].type = CMD_COLOR4F;
        repl_state_document_cmds_mut()[0].valid = 1;
        editor_buffer_set_line(0, wrapped);
        repl_state_document_count_set(1);

        ASSERT_TRUE("open visual dump file", dump_f != NULL);
        if (dump_f) {
            repl_dump_code_panel_visual_text(dump_f, editor_buffer_view());
            fclose(dump_f);
        }

        dump_f = fopen(tmp_dump_path, "r");
        ASSERT_TRUE("read visual dump file", dump_f != NULL);
        if (dump_f) {
            size_t n = fread(visual_buf, 1, sizeof(visual_buf) - 1, dump_f);
            visual_buf[n] = '\0';
            fclose(dump_f);
            ASSERT_TRUE("visual dump wraps first comma",
                        strstr(visual_buf, "glColor4f(0.125,\n") != NULL);
            ASSERT_TRUE("visual dump has continuation row",
                        strstr(visual_buf, "\n             0.250,\n") != NULL);
            ASSERT_TRUE("visual dump omits unwrapped full row",
                        strstr(visual_buf, "glColor4f(0.125, 0.250") == NULL);
        }
    }

    {
        const char *wrapped = "  glVertex3f(123456789012345678901234567890, 0.250, 0.500);";
        char visual_buf[8192];
        FILE *dump_f = fopen(tmp_dump_path, "w");

        repl_reset_state();
        declare_test_vars();
        repl_state_presentation_mut()->wrap_at_comma = 1;
        repl_state_presentation_mut()->show_vertex_indices = 0; repl_state_sync_ui_chrome();
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; repl_state_sync_ui_chrome();
        ui_state_viewport_set_size(360, ui_state_viewport().window_h);
        g_panel_frac = 0.75f;

        memset(&repl_state_document_cmds_mut()[0], 0, sizeof(repl_state_document_cmds_mut()[0]));
        repl_state_document_cmds_mut()[0].type = CMD_VERTEX3F;
        repl_state_document_cmds_mut()[0].valid = 1;
        editor_buffer_set_line(0, wrapped);
        repl_state_document_count_set(1);

        ASSERT_TRUE("open overflow visual dump file", dump_f != NULL);
        if (dump_f) {
            repl_dump_code_panel_visual_text(dump_f, editor_buffer_view());
            fclose(dump_f);
        }

        dump_f = fopen(tmp_dump_path, "r");
        ASSERT_TRUE("read overflow visual dump file", dump_f != NULL);
        if (dump_f) {
            size_t n = fread(visual_buf, 1, sizeof(visual_buf) - 1, dump_f);
            visual_buf[n] = '\0';
            fclose(dump_f);
            ASSERT_TRUE("visual dump wraps at first offscreen comma",
                        strstr(visual_buf,
                               "glVertex3f(123456789012345678901234567890,\n") != NULL);
            ASSERT_TRUE("visual dump keeps continuation after overflow comma",
                       strstr(visual_buf, "\n              0.250,\n") != NULL);
            ASSERT_TRUE("visual dump omits overflow-comma row from staying flat",
                        strstr(visual_buf,
                               "123456789012345678901234567890, 0.250") == NULL);
        }
    }

    {
        const char *wrapped =
            "  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, (GLfloat[]){0.2, 0, 0.15});";
        char visual_buf[8192];
        FILE *dump_f = fopen(tmp_dump_path, "w");

        repl_reset_state();
        declare_test_vars();
        repl_state_presentation_mut()->wrap_at_comma = 1;
        repl_state_presentation_mut()->show_vertex_indices = 0; repl_state_sync_ui_chrome();
        repl_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT; repl_state_sync_ui_chrome();
        ui_state_viewport_set_size(360, ui_state_viewport().window_h);
        g_panel_frac = 0.75f;

        memset(&repl_state_document_cmds_mut()[0], 0, sizeof(repl_state_document_cmds_mut()[0]));
        repl_state_document_cmds_mut()[0].type = CMD_POINT_PARAMETER_FV;
        repl_state_document_cmds_mut()[0].valid = 1;
        editor_buffer_set_line(0, wrapped);
        repl_state_document_count_set(1);

        ASSERT_TRUE("open point-parameter visual dump file", dump_f != NULL);
        if (dump_f) {
            repl_dump_code_panel_visual_text(dump_f, editor_buffer_view());
            fclose(dump_f);
        }

        dump_f = fopen(tmp_dump_path, "r");
        ASSERT_TRUE("read point-parameter visual dump file", dump_f != NULL);
        if (dump_f) {
            size_t n = fread(visual_buf, 1, sizeof(visual_buf) - 1, dump_f);
            visual_buf[n] = '\0';
            fclose(dump_f);
            ASSERT_TRUE("point-parameter visual dump wraps long enum arg",
                        strstr(visual_buf,
                               "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION,\n") != NULL);
            ASSERT_TRUE("point-parameter continuation uses capped hang indent",
                        strstr(visual_buf, "\n               (GLfloat[])\n") != NULL);
            ASSERT_TRUE("point-parameter continuation avoids extreme indent",
                        strstr(visual_buf, "\n                      (GLfloat[])\n") == NULL);
        }
    }

    {
        const char *wrapped =
            "  glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, (GLfloat[]){0.2, 0, 0.15});";
        char visual_buf[8192];
        FILE *dump_f = fopen(tmp_dump_path, "w");

        repl_reset_state();
        declare_test_vars();
        repl_state_presentation_mut()->wrap_at_comma = 1;
        repl_state_presentation_mut()->show_vertex_indices = 0; repl_state_sync_ui_chrome();
        ui_state_viewport_set_size(260, ui_state_viewport().window_h);
        g_panel_frac = 0.75f;

        memset(&repl_state_document_cmds_mut()[0], 0, sizeof(repl_state_document_cmds_mut()[0]));
        repl_state_document_cmds_mut()[0].type = CMD_POINT_PARAMETER_FV;
        repl_state_document_cmds_mut()[0].valid = 1;
        editor_buffer_set_line(0, wrapped);
        repl_state_document_count_set(1);

        ASSERT_TRUE("open narrow point-parameter visual dump file", dump_f != NULL);
        if (dump_f) {
            repl_dump_code_panel_visual_text(dump_f, editor_buffer_view());
            fclose(dump_f);
        }

        dump_f = fopen(tmp_dump_path, "r");
        ASSERT_TRUE("read narrow point-parameter visual dump file", dump_f != NULL);
        if (dump_f) {
            size_t n = fread(visual_buf, 1, sizeof(visual_buf) - 1, dump_f);
            visual_buf[n] = '\0';
            fclose(dump_f);
            ASSERT_TRUE("narrow point-parameter source starts with command text",
                        strstr(visual_buf, "--- source ---\n  glPointParameterfv(") != NULL);
            ASSERT_TRUE("narrow point-parameter source avoids blank first wrap row",
                        strstr(visual_buf, "--- source ---\n  \n") == NULL);
        }
    }

    {
        const int layouts[] = {
            CODE_PANEL_LAYOUT_TOP,
            CODE_PANEL_LAYOUT_BOTTOM,
            CODE_PANEL_LAYOUT_HIDDEN
        };
        const char *const layout_names[] = {
            "top",
            "bottom",
            "hidden"
        };
        const char *src = "  glVertex3f(1, 2, 3);";
        char visual_buf[8192];

        for (int layout_idx = 0; layout_idx < 3; layout_idx++) {
            FILE *dump_f = fopen(tmp_dump_path, "w");

            repl_reset_state();
            declare_test_vars();
            repl_state_presentation_mut()->wrap_at_comma = 1;
            repl_state_presentation_mut()->show_vertex_indices = 0; repl_state_sync_ui_chrome();
            ui_state_viewport_set_size(360, 800);
            g_panel_frac = 0.5f;
            repl_state_presentation_mut()->code_panel_layout = layouts[layout_idx]; repl_state_sync_ui_chrome();

            memset(&repl_state_document_cmds_mut()[0], 0, sizeof(repl_state_document_cmds_mut()[0]));
            repl_state_document_cmds_mut()[0].type = CMD_VERTEX3F;
            repl_state_document_cmds_mut()[0].valid = 1;
            editor_buffer_set_line(0, src);
            repl_state_document_count_set(1);

            ASSERT_TRUE("open layout visual dump file", dump_f != NULL);
            if (dump_f) {
                repl_dump_code_panel_visual_text(dump_f, editor_buffer_view());
                fclose(dump_f);
            }

            dump_f = fopen(tmp_dump_path, "r");
            ASSERT_TRUE("read layout visual dump file", dump_f != NULL);
            if (dump_f) {
                size_t n = fread(visual_buf, 1, sizeof(visual_buf) - 1, dump_f);
                visual_buf[n] = '\0';
                fclose(dump_f);
                {
                    char label[128];
                    snprintf(label, sizeof(label),
                             "%s visual dump keeps source on one row",
                             layout_names[layout_idx]);
                    ASSERT_TRUE(label,
                                strstr(visual_buf, "--- source ---\n  glVertex3f(1, 2, 3);\n") != NULL);
                    snprintf(label, sizeof(label),
                             "%s visual dump does not use side-panel width",
                             layout_names[layout_idx]);
                    ASSERT_TRUE(label,
                                strstr(visual_buf, "--- source ---\n  glVertex3f(1,\n") == NULL);
                }
            }
        }
        repl_state_presentation_mut()->code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT; repl_state_sync_ui_chrome();
        g_panel_frac = CFG_DEFAULT_PANEL_FRAC;
    }

    return test_harness_report(&g_harness, "repl_core_format");
}
