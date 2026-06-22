#include "editor/state.h"
#include "app/glr_ctrl.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/normalize.h"
#include "repl/state_views.h"
#include "repl/state_owners.h"
#include "repl/export.h"
#include "editor/input.h"
#include "src/editor/reformat.h"
#include "support/test_harness.h"

#include <stdio.h>
#include "source_document.h"
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
    const char *tmp_else_path = "/tmp/repl_core_format_else_input.c";
    const char *tmp_dump_path = "/tmp/repl_core_format_dump.txt";
    repl_eval_init_predef_vars();
    glr_ctrl_reset_all();
    declare_test_vars();

    GLCmd interactive_cmd;
    char interactive_text[MAX_LINE_LEN] = "";
    memset(&interactive_cmd, 0, sizeof(interactive_cmd));
    ASSERT_TRUE("interactive parse normalize",
                repl_parse_and_normalize(line, 0, NULL, 0, 1, &interactive_cmd,
                                         interactive_text, sizeof(interactive_text)) == 1);

    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line(line);
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

    glr_ctrl_reset_all();
    declare_test_vars();
    ASSERT_TRUE("load from file", repl_export_load_from_file(tmp_path, NULL) == 1);
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

    glr_ctrl_reset_all();
    declare_test_vars();
    ASSERT_TRUE("load loop file", repl_export_load_from_file(tmp_loop_path, NULL) == 1);
    ASSERT_TRUE("loop imported 3 cmds", repl_state_document_count() == 3);
    ASSERT_TRUE("loop header type", repl_state_document_cmds_mut()[0].type == CMD_FOR_BEGIN);
    ASSERT_TRUE("loop body type", repl_state_document_cmds_mut()[1].type == CMD_VERTEX3F);
    ASSERT_TRUE("loop body has vars", repl_state_document_cmds_mut()[1].has_vars == 1);
    ASSERT_TRUE("loop body keeps expression", strstr(editor_buffer_line(1), "i + x") != NULL);
    ASSERT_TRUE("loop body indented", strncmp(editor_buffer_line(1), "    ", 4) == 0);

    {
        editor_buffer_set_line(1, "glVertex3f(i+x,0,0)");
        editor_buffer_set_line(2, "}");
        editor_reformat_commands();
        const char *buf1 = editor_buffer_line(1);
        const char *buf2 = editor_buffer_line(2);
        ASSERT_TRUE("reformat body semicolon", buf1 && buf1[strlen(buf1) - 1] == ';');
        ASSERT_TRUE("reformat body indent", buf1 && strncmp(buf1, "    ", 4) == 0);
        ASSERT_TRUE("reformat body comma spacing", buf1 && strstr(buf1, ", 0, 0") != NULL);
        ASSERT_TRUE("reformat close brace indent", buf2 && strcmp(buf2, "  }") == 0);
    }

    {
        FILE *f = fopen(tmp_else_path, "w");
        ASSERT_TRUE("open tmp else file", f != NULL);
        if (f) {
            fprintf(f, "// Snippet start\n");
            fprintf(f, "if (x < 0) {\n");
            fprintf(f, "glVertex3f(-1, 0, 0);\n");
            fprintf(f, "} else if (x < 1) {\n");
            fprintf(f, "glVertex3f(1, 0, 0);\n");
            fprintf(f, "} else {\n");
            fprintf(f, "glVertex3f(2, 0, 0);\n");
            fprintf(f, "}\n");
            fprintf(f, "// Snippet end\n");
            fclose(f);
        }
    }

    glr_ctrl_reset_all();
    declare_test_vars();
    ASSERT_TRUE("load else-if file", repl_export_load_from_file(tmp_else_path, NULL) == 1);
    ASSERT_TRUE("else-if imported 7 cmds", repl_state_document_count() == 7);
    ASSERT_TRUE("else-if header type", repl_state_document_cmds_mut()[0].type == CMD_IF_BEGIN);
    ASSERT_TRUE("else-if separator type", repl_state_document_cmds_mut()[2].type == CMD_ELSE_IF);
    ASSERT_TRUE("else separator type", repl_state_document_cmds_mut()[4].type == CMD_ELSE);
    ASSERT_TRUE("else-if end type", repl_state_document_cmds_mut()[6].type == CMD_IF_END);
    ASSERT_TRUE("else-if canonical text",
                strcmp(editor_buffer_line(2), "  } else if(x < 1) {") == 0);
    ASSERT_TRUE("else canonical text",
                strcmp(editor_buffer_line(4), "  } else {") == 0);

    editor_buffer_set_line(2, "        } else if (x < 1) {");
    editor_buffer_set_line(4, "        } else {");
    editor_buffer_set_line(6, "        }");
    editor_reformat_commands();
    ASSERT_TRUE("reformat else-if separator indent",
                strcmp(editor_buffer_line(2), "  } else if(x < 1) {") == 0);
    ASSERT_TRUE("reformat else body indent",
                strcmp(editor_buffer_line(5), "    glVertex3f(2, 0, 0);") == 0);
    ASSERT_TRUE("reformat else close indent",
                strcmp(editor_buffer_line(6), "  }") == 0);

    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line("for(i, 0, 3) {");
    editor_feed_line("x = i + 1;");
    editor_feed_line("}");
    ASSERT_TRUE("assign in loop cmd count", repl_state_document_count() == 3);
    ASSERT_TRUE("assign in loop type", repl_state_document_cmds_mut()[1].type == CMD_VAR_ASSIGN);
    ASSERT_TRUE("assign in loop indent", strncmp(editor_buffer_line(1), "    ", 4) == 0);
    ASSERT_TRUE("assign in loop keeps expression", strstr(editor_buffer_line(1), "i + 1") != NULL);
    {
        editor_buffer_set_line(1, "x=i+1");
        editor_reformat_commands();
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
            repl_dump_code_panel_text(dump_f, source_document_view());
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

    glr_ctrl_reset_all();
    editor_feed_line("float a = 2, b, c = 3;");
    ASSERT_TRUE("decl cmd count", repl_state_document_count() == 1);
    ASSERT_TRUE("decl cmd type", repl_state_document_cmds_mut()[0].type == CMD_VAR_DECLARE);
    editor_buffer_set_line(0, "float a=max(1, 2),b,c=abs(-3)// vars");
    editor_reformat_commands();
    {
        const char *buf0 = editor_buffer_line(0);
        ASSERT_TRUE("reformat decl keeps initializer text and comment",
                    buf0 && strcmp(buf0,
                                   "  static float a = max(1, 2), b, c = abs(-3); // vars") == 0);
    }

    glr_ctrl_reset_all();
    editor_feed_line("func0() {");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("}");
    editor_feed_line("gluBegin(GLU_POLYGON);");
    editor_feed_line("func0();");
    editor_feed_line("gluEnd();");
    {
        int n = repl_state_document_count();
        ASSERT_TRUE("tess+func cmd count", n == 6);
        ASSERT_TRUE("tess+func call type",
                     repl_state_document_cmds_mut()[4].type == CMD_CALL);
        const char *call_line = editor_buffer_line(4);
        ASSERT_TRUE("funcN call inside tess block includes tess indent",
                     call_line && strncmp(call_line, "    ", 4) == 0);
        ASSERT_TRUE("funcN call inside tess block not over-indented",
                     call_line && call_line[4] != ' ');
    }

    /* glPushMatrix/glPopMatrix indent their bodies like glBegin/glEnd:
     * the push and pop lines sit at the enclosing level, and everything
     * between them gains one indent level (2 spaces per open push). */
    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glTranslatef(1, 0, 0);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glPopMatrix();");
    {
        ASSERT_TRUE("push block cmd count", repl_state_document_count() == 4);
        ASSERT_TRUE("push line type", repl_state_document_cmds_mut()[0].type == CMD_PUSH_MATRIX);
        ASSERT_TRUE("pop line type", repl_state_document_cmds_mut()[3].type == CMD_POP_MATRIX);
        ASSERT_TRUE("push line not indented",
                    strcmp(editor_buffer_line(0), "  glPushMatrix();") == 0);
        ASSERT_TRUE("push body translate indented",
                    strcmp(editor_buffer_line(1), "    glTranslatef(1, 0, 0);") == 0);
        ASSERT_TRUE("push body vertex indented",
                    strcmp(editor_buffer_line(2), "    glVertex3f(0, 0, 0);") == 0);
        ASSERT_TRUE("pop line aligns with push",
                    strcmp(editor_buffer_line(3), "  glPopMatrix();") == 0);
    }

    /* Nested push: each level adds one more indent; each pop lines up
     * with its matching push. */
    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glPopMatrix();");
    editor_feed_line("glPopMatrix();");
    {
        ASSERT_TRUE("nested push outer", strcmp(editor_buffer_line(0), "  glPushMatrix();") == 0);
        ASSERT_TRUE("nested push inner", strcmp(editor_buffer_line(1), "    glPushMatrix();") == 0);
        ASSERT_TRUE("nested push body",  strcmp(editor_buffer_line(2), "      glVertex3f(0, 0, 0);") == 0);
        ASSERT_TRUE("nested pop inner",  strcmp(editor_buffer_line(3), "    glPopMatrix();") == 0);
        ASSERT_TRUE("nested pop outer",  strcmp(editor_buffer_line(4), "  glPopMatrix();") == 0);
    }

    /* glBegin/glEnd nested inside a push block gain the matrix indent;
     * the body vertices then gain the begin indent on top of it. */
    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glBegin(GL_TRIANGLES);");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glEnd();");
    editor_feed_line("glPopMatrix();");
    {
        ASSERT_TRUE("begin-in-push: push", strcmp(editor_buffer_line(0), "  glPushMatrix();") == 0);
        ASSERT_TRUE("begin-in-push: begin", strcmp(editor_buffer_line(1), "    glBegin(GL_TRIANGLES);") == 0);
        ASSERT_TRUE("begin-in-push: vertex", strcmp(editor_buffer_line(2), "      glVertex3f(0, 0, 0);") == 0);
        ASSERT_TRUE("begin-in-push: end", strcmp(editor_buffer_line(3), "    glEnd();") == 0);
        ASSERT_TRUE("begin-in-push: pop", strcmp(editor_buffer_line(4), "  glPopMatrix();") == 0);
    }

    /* A body command carrying a predef var routes through the editor's
     * manual indent rewriter (not the parser's canonical text); it must
     * still pick up the matrix-depth indent. */
    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glVertex3f(x, 0, 0);");
    editor_feed_line("glPopMatrix();");
    {
        ASSERT_TRUE("vars body has vars", repl_state_document_cmds_mut()[1].has_vars == 1);
        ASSERT_TRUE("vars body keeps expression", strstr(editor_buffer_line(1), "x") != NULL);
        ASSERT_TRUE("vars body indented",
                    strncmp(editor_buffer_line(1), "    glVertex3f", 14) == 0);
    }

    /* Reformat (Ctrl+R) re-derives the same indentation from scratch. */
    glr_ctrl_reset_all();
    declare_test_vars();
    editor_feed_line("glPushMatrix();");
    editor_feed_line("glVertex3f(0, 0, 0);");
    editor_feed_line("glPopMatrix();");
    editor_buffer_set_line(0, "glPushMatrix()");      /* strip indent + semicolon */
    editor_buffer_set_line(1, "glVertex3f(0,0,0)");
    editor_buffer_set_line(2, "glPopMatrix()");
    editor_reformat_commands();
    {
        ASSERT_TRUE("reformat push", strcmp(editor_buffer_line(0), "  glPushMatrix();") == 0);
        ASSERT_TRUE("reformat push body", strcmp(editor_buffer_line(1), "    glVertex3f(0, 0, 0);") == 0);
        ASSERT_TRUE("reformat pop", strcmp(editor_buffer_line(2), "  glPopMatrix();") == 0);
    }

    return test_harness_report(&g_harness, "repl_core_format");
}
