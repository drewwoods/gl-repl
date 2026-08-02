#include "editor/state.h"
#include "app/glr_completion.h"
#include "app/glr_ctrl.h"
#include "repl/eval.h"
#include "repl/state.h"
#include <math.h>
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

/* Mid-line staging: park the cursor at the end of `head` within the
 * full line `head` + `tail` (e.g. head "glColorMaterial(GL_FR",
 * tail ", GL_DIFFUSE);" puts the cursor right after GL_FR). */
static void set_input_text_cursor_mid(const char *head, const char *tail) {
    EditorInputState *inp = editor_state_input_mut();
    snprintf(inp->input, sizeof(inp->input), "%s%s", head, tail);
    inp->input_len = (int)strlen(inp->input);
    editor_cursor_pos_set((int)strlen(head));
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
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("glVer");

        editor_completion_update();
        ASSERT_TRUE("ac_count > 0", g_ac_count > 0);
        ASSERT_STR("first match", g_ac_insert_matches[0], "glVertex2f(");
        ASSERT_STR("ghost text", g_ac_ghost, "tex2f(");
        ASSERT_STR("hint text", g_ac_hint, "x, y)");

        glr_completion_accept_autocomplete();
        ASSERT_STR("input after accept", editor_state_input().input, "glVertex2f(");
    }

    /* 1b. Completion after `IDENT = ` assignment-shaped prefix.
     * The matcher should treat the RHS as the completion prefix so
     * `x = glVer` finishes the same way as a bare `glVer`. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("x = glVer");

        editor_completion_update();
        ASSERT_TRUE("post-assign ac_count > 0", g_ac_count > 0);
        ASSERT_STR("post-assign first match",
                   g_ac_insert_matches[0], "glVertex2f(");
        ASSERT_STR("post-assign ghost", g_ac_ghost, "tex2f(");

        glr_completion_accept_autocomplete();
        ASSERT_STR("post-assign input after accept",
                   editor_state_input().input, "x = glVertex2f(");
    }

    /* 1b2. Case-insensitive matching with case correction on accept:
     * a wrong-case prefix matches the command/enum, and accepting
     * replaces the typed token with the canonical-cased candidate. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("glcolor3f");
        editor_completion_update();
        ASSERT_TRUE("ci command match count > 0", g_ac_count > 0);
        ASSERT_STR("ci command first match", g_ac_insert_matches[0], "glColor3f(");
        glr_completion_accept_autocomplete();
        ASSERT_STR("ci command accept corrects case",
                   editor_state_input().input, "glColor3f(");

        /* Mixed case in an assignment RHS. */
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("x = GLvertex3f");
        editor_completion_update();
        ASSERT_TRUE("ci RHS command match count > 0", g_ac_count > 0);
        glr_completion_accept_autocomplete();
        ASSERT_STR("ci RHS accept corrects case",
                   editor_state_input().input, "x = glVertex3f(");

        /* Enum argument matched and corrected case-insensitively (a
         * partial prefix, since completion only fires when the candidate
         * is strictly longer than what's typed). */
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("glBegin(gl_tri");
        editor_completion_update();
        ASSERT_TRUE("ci enum match count > 0", g_ac_count > 0);
        ASSERT_STR("ci enum first match", g_ac_insert_matches[0], "GL_TRIANGLE_FAN");
        glr_completion_accept_autocomplete();
        ASSERT_STR("ci enum accept corrects case",
                   editor_state_input().input, "glBegin(GL_TRIANGLE_FAN)");
    }

    /* 1c. Assignment via array index (A[0] = ...) and multi-token
     * declaration prefix (float x = ...). */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("A[0] = sin");
        editor_completion_update();
        ASSERT_TRUE("array-assign matches", g_ac_count > 0);

        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("float n = cos");
        editor_completion_update();
        ASSERT_TRUE("float-decl-assign matches", g_ac_count > 0);
    }

    /* 1d. `==` / `<=` / `>=` / `!=` are NOT assignments. The matcher
     * must not skip past their `=` and treat the RHS as a completion
     * prefix (which would surface bogus matches mid-comparison). */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("if(x == glVer");
        editor_completion_update();
        /* `glVer` should not match here - the leading `if(x == ` is
         * not an assignment context. */
        ASSERT_INT("comparison '==' not treated as assignment",
                   g_ac_count, 0);
    }

    /* 2. Enum completion - glBegin */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("glBegin(GL_TRI");

        editor_completion_update();
        ASSERT_TRUE("ac_count > 0", g_ac_count > 0);
        ASSERT_STR("first match", g_ac_insert_matches[0], "GL_TRIANGLE_FAN");
        ASSERT_STR("ghost text", g_ac_ghost, "ANGLE_FAN)");

        glr_completion_accept_autocomplete();
        ASSERT_STR("input after accept", editor_state_input().input, "glBegin(GL_TRIANGLE_FAN)");
    }

    /* 2b. Bitfield all-alias completion is slot metadata: glPushAttrib
     * offers it for the first term and after `|`; glClear does not. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("glPushAttrib(GL_ALL");
        editor_completion_update();
        ASSERT_TRUE("push attrib offers all alias",
                    has_insert_match("GL_ALL_ATTRIB_BITS"));
        glr_completion_accept_autocomplete();
        ASSERT_STR("push attrib accepts all alias",
                   editor_state_input().input,
                   "glPushAttrib(GL_ALL_ATTRIB_BITS)");

        set_input_text("glPushAttrib(GL_CURRENT_BIT | GL_ALL");
        editor_completion_update();
        ASSERT_TRUE("push attrib offers all alias after bar",
                    has_insert_match("GL_ALL_ATTRIB_BITS"));

        set_input_text("glClear(GL_ALL");
        editor_completion_update();
        ASSERT_INT("glClear does not offer attrib all alias", g_ac_count, 0);
    }

    /* 3. Multi-argument enum completion - glColorMaterial */
    {
        glr_ctrl_reset_all(); declare_test_vars();
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

    /* 3b. Mid-line enum completion: cursor at the end of a prior arg's
     * token with later args already typed. Accept splices the candidate
     * at the cursor, preserves the trailing text, adds no suffix, and
     * leaves the cursor after the inserted token. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text_cursor_mid("glColorMaterial(GL_FR", ", GL_DIFFUSE);");
        editor_completion_update();
        ASSERT_TRUE("mid-line arg1 matches", g_ac_count > 0);
        ASSERT_STR("mid-line arg1 match", g_ac_insert_matches[0], "GL_FRONT");
        glr_completion_accept_autocomplete();
        ASSERT_STR("mid-line arg1 accept splices",
                   editor_state_input().input,
                   "glColorMaterial(GL_FRONT, GL_DIFFUSE);");
        ASSERT_INT("mid-line arg1 cursor after token",
                   editor_cursor_pos(),
                   (int)strlen("glColorMaterial(GL_FRONT"));

        /* Last arg with only the closing paren after the cursor. */
        set_input_text_cursor_mid("glColorMaterial(GL_FRONT, GL_AMB", ")");
        editor_completion_update();
        ASSERT_STR("mid-line arg2 match", g_ac_insert_matches[0], "GL_AMBIENT");
        glr_completion_accept_autocomplete();
        ASSERT_STR("mid-line arg2 accept keeps close paren",
                   editor_state_input().input,
                   "glColorMaterial(GL_FRONT, GL_AMBIENT)");

        /* Slot selection: completing a middle bool of glColorMask. */
        set_input_text_cursor_mid("glColorMask(GL_TRUE, GL_FA",
                                  ", GL_TRUE, GL_TRUE)");
        editor_completion_update();
        ASSERT_STR("mid-line colormask slot2 match",
                   g_ac_insert_matches[0], "GL_FALSE");
        glr_completion_accept_autocomplete();
        ASSERT_STR("mid-line colormask slot2 accept",
                   editor_state_input().input,
                   "glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_TRUE)");

        /* Empty prefix inside empty parens offers the full slot table. */
        set_input_text_cursor_mid("glBegin(", ")");
        editor_completion_update();
        ASSERT_TRUE("mid-line empty prefix offers slot enums",
                    has_insert_match("GL_TRIANGLES"));

        /* Case correction works mid-line too. */
        set_input_text_cursor_mid("glBegin(gl_tri", ");");
        editor_completion_update();
        ASSERT_STR("mid-line ci enum match", g_ac_insert_matches[0],
                   "GL_TRIANGLE_FAN");
        glr_completion_accept_autocomplete();
        ASSERT_STR("mid-line ci enum accept corrects case",
                   editor_state_input().input, "glBegin(GL_TRIANGLE_FAN);");
    }

    /* 3c. Mid-line gating: no completion when the cursor is mid-token,
     * inside the command name, or on a non-enum command. */
    {
        glr_ctrl_reset_all(); declare_test_vars();

        /* Cursor mid-token (between GL_F and R). */
        set_input_text_cursor_mid("glColorMaterial(GL_F", "R, GL_DIFFUSE);");
        editor_completion_update();
        ASSERT_INT("mid-token cursor offers nothing", g_ac_count, 0);

        /* Cursor inside the command name. */
        set_input_text_cursor_mid("glColorMat", "erial(GL_FRONT, GL_DIFFUSE);");
        editor_completion_update();
        ASSERT_INT("cursor in command name offers nothing", g_ac_count, 0);

        /* Non-enum command mid-line: function-name completion must not
         * fire away from end-of-input. */
        set_input_text_cursor_mid("glVertex3f(1, 2", ", 3);");
        editor_completion_update();
        ASSERT_INT("non-enum command mid-line offers nothing", g_ac_count, 0);

        /* Trailing tail that is not just call args. */
        set_input_text_cursor_mid("glBegin(GL_TRI", " x");
        editor_completion_update();
        ASSERT_INT("non-arg tail offers nothing", g_ac_count, 0);
    }

    /* 3d. Mid-line slot scan is depth-aware: a comma inside a nested
     * paren after the cursor doesn't break the tail gate, and one
     * before the cursor doesn't advance the slot. */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text_cursor_mid("glMaterialfv(GL_FR",
                                  ", GL_AMBIENT, (GLfloat[]){1, 0, 0, 1});");
        editor_completion_update();
        ASSERT_STR("mid-line face slot match with compound-literal tail",
                   g_ac_insert_matches[0], "GL_FRONT");
        glr_completion_accept_autocomplete();
        ASSERT_STR("mid-line face slot accept",
                   editor_state_input().input,
                   "glMaterialfv(GL_FRONT, GL_AMBIENT, (GLfloat[]){1, 0, 0, 1});");
    }

    /* 4. glPointParameterfv custom completion */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        set_input_text("glPointParameterfv(GL_POINT_DIST");

        editor_completion_update();
        ASSERT_STR("match", g_ac_insert_matches[0], "GL_POINT_DISTANCE_ATTENUATION");
        ASSERT_STR("ghost", g_ac_ghost, "ANCE_ATTENUATION, ");
        glr_completion_accept_autocomplete();
        ASSERT_STR("input", editor_state_input().input, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, ");

        /* Mid-line: pname token with the float args already typed. */
        set_input_text_cursor_mid("glPointParameterfv(GL_POINT_DIST",
                                  ", 1, 0, 0);");
        editor_completion_update();
        ASSERT_STR("mid-line pname match", g_ac_insert_matches[0],
                   "GL_POINT_DISTANCE_ATTENUATION");
        glr_completion_accept_autocomplete();
        ASSERT_STR("mid-line pname accept keeps float args",
                   editor_state_input().input,
                   "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1, 0, 0);");
    }

    /* 4b. glColorMask - 4 bool slots; non-last slots suffix ", ",
     * the final (alpha) slot closes with ")". */
    {
        glr_ctrl_reset_all(); declare_test_vars();
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
        glr_ctrl_reset_all(); declare_test_vars();

        /* Typed "glMaterialfv" - function-prefix hint shows the
         * 3-arg canonical form rather than the misleading
         * 6-distinct-arg form the old glMaterialf spec rendered. */
        set_input_text("glMaterialfv");
        editor_completion_update();
        ASSERT_STR("glMaterialfv func hint",
                   g_ac_hint, "face, pname, (GLfloat[]){r, g, b, a})");

        /* Slot 1 (face) ends with ", " - same as any non-final slot. */
        set_input_text("glMaterialfv(GL_FR");
        editor_completion_update();
        ASSERT_STR("glMaterialfv slot1 match", g_ac_insert_matches[0], "GL_FRONT");
        ASSERT_STR("glMaterialfv slot1 ghost", g_ac_ghost, "ONT, ");

        /* Slot 2 (pname) is the LAST enum slot but NOT the last arg of
         * the call - bug fix: suffix must be ", " (custom-parser row
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

    /* 4c-2. glMaterialf - scalar sibling of glMaterialfv. Function-name
     * completion must offer glMaterialf( as its own match (not silently
     * steer to glMaterialfv), and the pname slot must be restricted to
     * GL_SHININESS. Same -2 num_args shape, so the pname-slot ghost
     * stays open for the trailing value arg. */
    {
        glr_ctrl_reset_all(); declare_test_vars();

        /* Typed exactly "glMaterialf" - the func-completion table has
         * glMaterialf( ordered before glMaterialfv(, so the first
         * (default-selected) match is the scalar variant. */
        set_input_text("glMaterialf");
        editor_completion_update();
        ASSERT_TRUE("glMaterialf prefix offers glMaterialf(",
                    has_insert_match("glMaterialf("));
        ASSERT_TRUE("glMaterialf prefix also offers glMaterialfv(",
                    has_insert_match("glMaterialfv("));
        ASSERT_STR("glMaterialf is the first (default) match",
                   g_ac_insert_matches[0], "glMaterialf(");
        ASSERT_STR("glMaterialf func hint shows scalar shape",
                   g_ac_hint, "face, GL_SHININESS, value)");

        /* Slot 1 (face) - same shape as glMaterialfv. */
        set_input_text("glMaterialf(GL_FR");
        editor_completion_update();
        ASSERT_STR("glMaterialf slot1 match",
                   g_ac_insert_matches[0], "GL_FRONT");
        ASSERT_STR("glMaterialf slot1 ghost", g_ac_ghost, "ONT, ");

        /* Slot 2 (pname) - restricted to GL_SHININESS only. The pname
         * is the last enum slot but NOT the last arg of the call
         * (-2 num_args row, trailing value), so the ghost suffix is
         * ", " (not ")"). */
        set_input_text("glMaterialf(GL_FRONT, GL_SH");
        editor_completion_update();
        ASSERT_INT("glMaterialf slot2 offers exactly one pname",
                   g_ac_count, 1);
        ASSERT_STR("glMaterialf slot2 match is GL_SHININESS",
                   g_ac_insert_matches[0], "GL_SHININESS");
        ASSERT_STR("glMaterialf slot2 ghost stays open for value",
                   g_ac_ghost, "ININESS, ");

        /* Typing the value slot triggers the func-prefix param hint
         * over the trailing arg. */
        set_input_text("glMaterialf(GL_FRONT, GL_SHININESS, ");
        editor_completion_update();
        ASSERT_STR("glMaterialf value-arg hint", g_ac_hint, "value)");
    }

    /* 4d. gluColor - hint must reflect that alpha is optional. The
     * parser accepts 3 or 4 floats (alpha defaults to 1.0 when
     * omitted), so the param-hint walker is configured with only 3
     * mandatory params (r, g, b). The display_text carries "[, a]"
     * separately for popup-list discoverability of the optional arg. */
    {
        glr_ctrl_reset_all(); declare_test_vars();

        set_input_text("gluColor");
        editor_completion_update();
        ASSERT_STR("gluColor func hint stops at b)", g_ac_hint, "r, g, b)");

        set_input_text("gluColor(1, 0, ");
        editor_completion_update();
        ASSERT_STR("gluColor third-arg hint", g_ac_hint, "b)");
    }

    /* 5. User-defined function completion */
    {
        glr_ctrl_reset_all(); declare_test_vars();
        editor_feed_line("func0(radius, height) {");
        editor_feed_line("}");

        set_input_text("func0(");

        editor_completion_update();
        ASSERT_STR("user func hint", g_ac_hint, "radius, height)");

        set_input_text("func0(10, ");
        editor_completion_update();
        ASSERT_STR("user func hint arg2", g_ac_hint, "height)");
    }

    /* 5b. Function aliases complete like built-in names, and the param
     * hint follows the alias spelling as well as the bare funcN one. */
    {
        glr_ctrl_reset_all(); declare_test_vars();

        /* No alias bound yet: nothing to offer. */
        set_input_text("drawC");
        editor_completion_update();
        ASSERT_INT("no alias completion before definition", g_ac_count, 0);

        editor_feed_line("drawCube(size, twist) {");
        editor_feed_line("}");

        set_input_text("drawC");
        editor_completion_update();
        ASSERT_TRUE("alias completion present", has_insert_match("drawCube("));
        ASSERT_STR("alias ghost", g_ac_ghost, "ube(");
        ASSERT_STR("alias param hint", g_ac_hint, "size, twist)");

        glr_completion_accept_autocomplete();
        ASSERT_STR("alias input after accept",
                   editor_state_input().input, "drawCube(");

        /* Matching is case-insensitive; accept inserts the canonical name. */
        set_input_text("DRAWCU");
        editor_completion_update();
        ASSERT_TRUE("alias completes case-insensitively",
                    has_insert_match("drawCube("));
        glr_completion_accept_autocomplete();
        ASSERT_STR("alias case corrected on accept",
                   editor_state_input().input, "drawCube(");

        /* Param hint for a call being typed with the alias spelling. */
        set_input_text("drawCube(1, ");
        editor_completion_update();
        ASSERT_STR("alias call hint arg2", g_ac_hint, "twist)");

        /* A fully-typed alias offers no candidate (nothing left to add). */
        set_input_text("drawCube(");
        editor_completion_update();
        ASSERT_TRUE("fully typed alias offers no candidate",
                    !has_insert_match("drawCube("));
        ASSERT_STR("fully typed alias still hints",
                   g_ac_hint, "size, twist)");
    }

    /* 6. Statement keyword completion - float declarations */
    {
        glr_ctrl_reset_all(); declare_test_vars();
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

            glr_ctrl_reset_all(); declare_test_vars();
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
        glr_ctrl_reset_all(); declare_test_vars();
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
