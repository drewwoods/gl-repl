#include "repl_core_internal.h"
#include "sample.h"
#include <stdio.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s]\n", label); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d\n", label, (int)(got), (int)(exp)); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    g_run++; \
    if (strcmp(got, exp) == 0) g_pass++; \
    else printf("FAIL [%s] got \"%s\", expected \"%s\"\n", label, got, exp); \
} while (0)

int main() {
    init_predef_vars();
    printf("--- repl_autocomplete tests ---\n");

    /* 1. Basic function completion */
    {
        repl_reset_state();
        strcpy(g_input, "glVer");
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        
        update_autocomplete();
        ASSERT_TRUE("ac_count > 0", g_ac_count > 0);
        ASSERT_STR("first match", g_ac_insert_matches[0], "glVertex3f(");
        ASSERT_STR("ghost text", g_ac_ghost, "tex3f(");
        ASSERT_STR("hint text", g_ac_hint, "x, y, z)");
        
        accept_autocomplete();
        ASSERT_STR("input after accept", g_input, "glVertex3f(");
    }

    /* 2. Enum completion - glBegin */
    {
        repl_reset_state();
        strcpy(g_input, "glBegin(GL_TRI");
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        
        update_autocomplete();
        ASSERT_TRUE("ac_count > 0", g_ac_count > 0);
        ASSERT_STR("first match", g_ac_insert_matches[0], "GL_TRIANGLES");
        ASSERT_STR("ghost text", g_ac_ghost, "ANGLES)");
        
        accept_autocomplete();
        ASSERT_STR("input after accept", g_input, "glBegin(GL_TRIANGLES)");
    }

    /* 3. Multi-argument enum completion - glColorMaterial */
    {
        repl_reset_state();
        strcpy(g_input, "glColorMaterial(GL_FR");
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        
        update_autocomplete();
        ASSERT_STR("first match arg1", g_ac_insert_matches[0], "GL_FRONT");
        ASSERT_STR("ghost text arg1", g_ac_ghost, "ONT, ");
        accept_autocomplete();
        ASSERT_STR("input after arg1", g_input, "glColorMaterial(GL_FRONT, ");
        
        strcat(g_input, "GL_AMB");
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        update_autocomplete();
        ASSERT_STR("first match arg2", g_ac_insert_matches[0], "GL_AMBIENT");
        ASSERT_STR("ghost text arg2", g_ac_ghost, "IENT)");
        accept_autocomplete();
        ASSERT_STR("input after arg2", g_input, "glColorMaterial(GL_FRONT, GL_AMBIENT)");
    }

    /* 4. glPointParameterfv custom completion */
    {
        repl_reset_state();
        strcpy(g_input, "glPointParameterfv(GL_POINT_DIST");
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        
        update_autocomplete();
        ASSERT_STR("match", g_ac_insert_matches[0], "GL_POINT_DISTANCE_ATTENUATION");
        ASSERT_STR("ghost", g_ac_ghost, "ANCE_ATTENUATION, ");
        accept_autocomplete();
        ASSERT_STR("input", g_input, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, ");
    }

    /* 5. User-defined function completion */
    {
        repl_reset_state();
        repl_feed_line_public("func0(radius, height) {");
        repl_feed_line_public("}");
        
        strcpy(g_input, "func0(");
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        
        update_autocomplete();
        ASSERT_STR("user func hint", g_ac_hint, "radius, height)");
        
        strcat(g_input, "10, ");
        g_input_len = (int)strlen(g_input);
        g_cursor_pos = g_input_len;
        update_autocomplete();
        ASSERT_STR("user func hint arg2", g_ac_hint, "height)");
    }

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
