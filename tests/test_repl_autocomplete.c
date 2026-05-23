#include "editor/state.h"
#include "app/glr_completion.h"
#include "app/glr_ctrl.h"
#include "repl/core_internal.h"
#include "repl/state.h"
#include "repl/core.h"
#include "editor/input.h"
#include "editor/completion.h"
#include "support/test_harness.h"

#define g_ac_count          (editor_state_autocomplete_mut()->match_count)
#define g_ac_ghost          (editor_state_autocomplete_mut()->ghost)
#define g_ac_hint           (editor_state_autocomplete_mut()->hint)
#define g_ac_matches        (editor_state_autocomplete_mut()->matches)
#define g_ac_insert_matches (editor_state_autocomplete_mut()->insert_matches)

#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_INT(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)

#define ASSERT_STR(label, got, exp) \
    TEST_ASSERT_STR(&g_harness, label, got, exp)

static void declare_test_vars(void) {
    char err[128];
    repl_eval_declare_predef_var("x", err, sizeof(err));
    repl_eval_declare_predef_var("y", err, sizeof(err));
    repl_eval_declare_predef_var("z", err, sizeof(err));
    repl_eval_declare_predef_var("i", err, sizeof(err));
    repl_eval_declare_predef_var("j", err, sizeof(err));
    repl_eval_declare_predef_var("k", err, sizeof(err));
    repl_eval_declare_predef_var("n", err, sizeof(err));
}

static void set_input_text(const char *text) {
    EditorInputState *inp = editor_state_input_mut();
    strcpy(inp->input, text);
    inp->input_len = (int)strlen(inp->input);
    editor_cursor_pos_set(inp->input_len);
}

static int has_insert_match(const char *text) {
    for (int i = 0; i < g_ac_count; i++) {
        if (g_ac_insert_matches[i] &&
            strcmp(g_ac_insert_matches[i], text) == 0)
            return 1;
    }
    return 0;
}

int main() {
    repl_eval_init_predef_vars();
    printf("--- repl_autocomplete tests ---\n");

    /* 1. Basic function completion */
    {
        glr_app_reset_all(); declare_test_vars();
        set_input_text("glVer");

        editor_completion_update();
        ASSERT_TRUE("ac_count > 0", g_ac_count > 0);
        ASSERT_STR("first match", g_ac_insert_matches[0], "glVertex3f(");
        ASSERT_STR("ghost text", g_ac_ghost, "tex3f(");
        ASSERT_STR("hint text", g_ac_hint, "x, y, z)");

        glr_completion_accept_autocomplete();
        ASSERT_STR("input after accept", editor_state_input().input, "glVertex3f(");
    }

    /* 1b. Completion after `IDENT = ` assignment-shaped prefix.
     * The matcher should treat the RHS as the completion prefix so
     * `x = glVer` finishes the same way as a bare `glVer`. */
    {
        glr_app_reset_all(); declare_test_vars();
        set_input_text("x = glVer");

        editor_completion_update();
        ASSERT_TRUE("post-assign ac_count > 0", g_ac_count > 0);
        ASSERT_STR("post-assign first match",
                   g_ac_insert_matches[0], "glVertex3f(");
        ASSERT_STR("post-assign ghost", g_ac_ghost, "tex3f(");

        glr_completion_accept_autocomplete();
        ASSERT_STR("post-assign input after accept",
                   editor_state_input().input, "x = glVertex3f(");
    }

    /* 1c. Assignment via array index (A[0] = ...) and multi-token
     * declaration prefix (float x = ...). */
    {
        glr_app_reset_all(); declare_test_vars();
        set_input_text("A[0] = sin");
        editor_completion_update();
        ASSERT_TRUE("array-assign matches", g_ac_count > 0);

        glr_app_reset_all(); declare_test_vars();
        set_input_text("float n = cos");
        editor_completion_update();
        ASSERT_TRUE("float-decl-assign matches", g_ac_count > 0);
    }

    /* 1d. `==` / `<=` / `>=` / `!=` are NOT assignments. The matcher
     * must not skip past their `=` and treat the RHS as a completion
     * prefix (which would surface bogus matches mid-comparison). */
    {
        glr_app_reset_all(); declare_test_vars();
        set_input_text("if(x == glVer");
        editor_completion_update();
        /* `glVer` should not match here — the leading `if(x == ` is
         * not an assignment context. */
        ASSERT_INT("comparison '==' not treated as assignment",
                   g_ac_count, 0);
    }

    /* 2. Enum completion - glBegin */
    {
        glr_app_reset_all(); declare_test_vars();
        set_input_text("glBegin(GL_TRI");

        editor_completion_update();
        ASSERT_TRUE("ac_count > 0", g_ac_count > 0);
        ASSERT_STR("first match", g_ac_insert_matches[0], "GL_TRIANGLES");
        ASSERT_STR("ghost text", g_ac_ghost, "ANGLES)");

        glr_completion_accept_autocomplete();
        ASSERT_STR("input after accept", editor_state_input().input, "glBegin(GL_TRIANGLES)");
    }

    /* 3. Multi-argument enum completion - glColorMaterial */
    {
        glr_app_reset_all(); declare_test_vars();
        set_input_text("glColorMaterial(GL_FR");

        editor_completion_update();
        ASSERT_STR("first match arg1", g_ac_insert_matches[0], "GL_FRONT");
        ASSERT_STR("ghost text arg1", g_ac_ghost, "ONT, ");
        glr_completion_accept_autocomplete();
        ASSERT_STR("input after arg1", editor_state_input().input, "glColorMaterial(GL_FRONT, ");

        set_input_text("glColorMaterial(GL_FRONT, GL_AMB");
        editor_completion_update();
        ASSERT_STR("first match arg2", g_ac_insert_matches[0], "GL_AMBIENT");
        ASSERT_STR("ghost text arg2", g_ac_ghost, "IENT)");
        glr_completion_accept_autocomplete();
        ASSERT_STR("input after arg2", editor_state_input().input, "glColorMaterial(GL_FRONT, GL_AMBIENT)");

        set_input_text("glColorMaterial(GL_FRONT, GL_SH");
        editor_completion_update();
        ASSERT_INT("shininess is not a color-material mode", g_ac_count, 0);
    }

    /* 4. glPointParameterfv custom completion */
    {
        glr_app_reset_all(); declare_test_vars();
        set_input_text("glPointParameterfv(GL_POINT_DIST");

        editor_completion_update();
        ASSERT_STR("match", g_ac_insert_matches[0], "GL_POINT_DISTANCE_ATTENUATION");
        ASSERT_STR("ghost", g_ac_ghost, "ANCE_ATTENUATION, ");
        glr_completion_accept_autocomplete();
        ASSERT_STR("input", editor_state_input().input, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, ");
    }

    /* 4b. glColorMask - 4 bool slots; non-last slots suffix ", ",
     * the final (alpha) slot closes with ")". */
    {
        glr_app_reset_all(); declare_test_vars();
        set_input_text("glColorMask(GL_TR");
        editor_completion_update();
        ASSERT_STR("colormask slot1 match", g_ac_insert_matches[0], "GL_TRUE");
        ASSERT_STR("colormask slot1 ghost", g_ac_ghost, "UE, ");

        set_input_text("glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FA");
        editor_completion_update();
        ASSERT_STR("colormask slot4 match", g_ac_insert_matches[0], "GL_FALSE");
        ASSERT_STR("colormask slot4 ghost", g_ac_ghost, "LSE)");
        glr_completion_accept_autocomplete();
        ASSERT_STR("colormask slot4 accept", editor_state_input().input,
                   "glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE)");
    }

    /* 4c. glMaterialfv - custom-parser row (num_args = -2). The two
     * enum slots are face / pname, but a third arg (the compound
     * literal) still needs typing, so the pname slot's accept suffix
     * must be ", " (not ")"). The function-prefix hint shows the 3-arg
     * canonical form with the compound literal as the trailing param. */
    {
        glr_app_reset_all(); declare_test_vars();

        /* Typed "glMaterialfv" — function-prefix hint shows the
         * 3-arg canonical form rather than the misleading
         * 6-distinct-arg form the old glMaterialf spec rendered. */
        set_input_text("glMaterialfv");
        editor_completion_update();
        ASSERT_STR("glMaterialfv func hint",
                   g_ac_hint, "face, pname, (GLfloat[]){r, g, b, a})");

        /* Slot 1 (face) ends with ", " — same as any non-final slot. */
        set_input_text("glMaterialfv(GL_FR");
        editor_completion_update();
        ASSERT_STR("glMaterialfv slot1 match", g_ac_insert_matches[0], "GL_FRONT");
        ASSERT_STR("glMaterialfv slot1 ghost", g_ac_ghost, "ONT, ");

        /* Slot 2 (pname) is the LAST enum slot but NOT the last arg of
         * the call — bug fix: suffix must be ", " (custom-parser row
         * with trailing compound literal), not ")". */
        set_input_text("glMaterialfv(GL_FRONT, GL_AMB");
        editor_completion_update();
        ASSERT_STR("glMaterialfv slot2 match", g_ac_insert_matches[0], "GL_AMBIENT");
        ASSERT_STR("glMaterialfv slot2 ghost stays open for next arg",
                   g_ac_ghost, "IENT, ");
        glr_completion_accept_autocomplete();
        ASSERT_STR("glMaterialfv slot2 accept doesn't close call",
                   editor_state_input().input,
                   "glMaterialfv(GL_FRONT, GL_AMBIENT, ");

        /* Once the user starts the third arg, the param hint takes
         * over and offers the compound literal. */
        set_input_text("glMaterialfv(GL_FRONT, GL_AMBIENT, ");
        editor_completion_update();
        ASSERT_STR("glMaterialfv 3rd arg hint",
                   g_ac_hint, "(GLfloat[]){r, g, b, a})");
    }

    /* 5. User-defined function completion */
    {
        glr_app_reset_all(); declare_test_vars();
        editor_feed_line("func0(radius, height) {");
        editor_feed_line("}");

        set_input_text("func0(");

        editor_completion_update();
        ASSERT_STR("user func hint", g_ac_hint, "radius, height)");

        set_input_text("func0(10, ");
        editor_completion_update();
        ASSERT_STR("user func hint arg2", g_ac_hint, "height)");
    }

    /* 6. Statement keyword completion - float declarations */
    {
        glr_app_reset_all(); declare_test_vars();
        set_input_text("flo");

        editor_completion_update();
        ASSERT_TRUE("float completion present", has_insert_match("float "));
        ASSERT_STR("float match", g_ac_insert_matches[0], "float ");
        ASSERT_STR("float ghost", g_ac_ghost, "at ");

        glr_completion_accept_autocomplete();
        ASSERT_STR("float input after accept", editor_state_input().input, "float ");
    }

    /* 7. Supported funcN completions cover func0..func9 */
    {
        for (int fn = 0; fn <= 9; fn++) {
            char prefix[16];
            char def_text[16];
            char call_text[16];
            char label[64];

            glr_app_reset_all(); declare_test_vars();
            snprintf(prefix, sizeof(prefix), "func%d", fn);
            snprintf(def_text, sizeof(def_text), "func%d() {", fn);
            snprintf(call_text, sizeof(call_text), "func%d()", fn);
            set_input_text(prefix);

            editor_completion_update();

            snprintf(label, sizeof(label), "func%d def completion", fn);
            ASSERT_TRUE(label, has_insert_match(def_text));
            snprintf(label, sizeof(label), "func%d call completion", fn);
            ASSERT_TRUE(label, has_insert_match(call_text));
        }
    }

    /* 8. Scratch-array completions */
    {
        glr_app_reset_all(); declare_test_vars();
        set_input_text("A");

        editor_completion_update();
        ASSERT_TRUE("scratch array completion present", has_insert_match("A["));
        ASSERT_STR("scratch array ghost", g_ac_ghost, "[");

        glr_completion_accept_autocomplete();
        ASSERT_STR("scratch array input after accept", editor_state_input().input, "A[");
    }

    printf("\n");
    return test_harness_report(&g_harness, "test_repl_autocomplete");
}
