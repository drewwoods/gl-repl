#include "repl_core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s]\n", label); \
} while (0)

static size_t read_text_file(const char *path, char *buf, size_t buf_sz) {
    FILE *f = fopen(path, "r");
    size_t nread = 0;

    if (buf_sz == 0)
        return 0;
    if (f) {
        nread = fread(buf, 1, buf_sz - 1, f);
        fclose(f);
    }
    buf[nread] = '\0';
    return nread;
}

static int count_substr(const char *haystack, const char *needle) {
    int count = 0;
    size_t needle_len;
    const char *p;

    if (!haystack || !needle || !needle[0])
        return 0;

    needle_len = strlen(needle);
    p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    return count;
}

static int find_init_line(const char *needle) {
    char line[256];

    for (int i = 0; i < init_section_line_count(); i++) {
        init_section_line(i, line, sizeof(line));
        if (strcmp(line, needle) == 0)
            return i;
    }
    return -1;
}

static int find_init_line_substr(const char *needle) {
    char line[256];

    for (int i = 0; i < init_section_line_count(); i++) {
        init_section_line(i, line, sizeof(line));
        if (strstr(line, needle) != NULL)
            return i;
    }
    return -1;
}

int main(void) {
    const char *path = "/tmp/repl_core_roundtrip_output.c";
    const char *func_path = "/tmp/repl_core_func_output.c";
    const char *quadric_path = "/tmp/repl_core_quadric_output.c";
    const char *tess_path = "/tmp/repl_core_tess_output.c";

    init_predef_vars();
    repl_reset_state();

    repl_feed_line_public("x = 1.25;");
    repl_feed_line_public("glBegin(GL_LINE_STRIP);");
    repl_feed_line_public("for(i, 0, 3) {");
    repl_feed_line_public("glVertex3f(i + x, 0, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("glEnd();");

    ASSERT_TRUE("pre-save cmds", g_num_cmds > 0);
    ASSERT_TRUE("init has host-only ambient line",
                find_init_line("  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);") >= 0);
    ASSERT_TRUE("init has quadric texture line",
                find_init_line("  gluQuadricTexture(g_quadric, GL_FALSE);") >= 0);
    ASSERT_TRUE("init has tess init line",
                find_init_line("  g_tess = gluNewTess();") >= 0);
    ASSERT_TRUE("init has color material enable bootstrap",
                find_init_line_substr("glEnable(GL_COLOR_MATERIAL);") >= 0);
    ASSERT_TRUE("init has color material bootstrap",
                find_init_line_substr("glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);") >= 0);
    ASSERT_TRUE("init has two-side bootstrap",
                find_init_line_substr("glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);") >= 0);
    ASSERT_TRUE("init has blend enable bootstrap",
                find_init_line_substr("glEnable(GL_BLEND);") >= 0);
    ASSERT_TRUE("init has blend func bootstrap",
                find_init_line_substr("glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);") >= 0);
    ASSERT_TRUE("init has point attenuation bootstrap",
                find_init_line_substr("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") >= 0);
    ASSERT_TRUE("init has tess edge-flag callback",
                find_init_line_substr("GLU_TESS_EDGE_FLAG") >= 0);

    int before_n = g_num_cmds;
    CmdType before_types[MAX_COMMANDS];
    for (int i = 0; i < before_n; i++) before_types[i] = g_cmds[i].type;

    g_multisample_enabled = 0;
    g_line_smooth_enabled = 1;
    g_show_outlines = 0;
    g_show_vpoints = 0;
    repl_save_output(path);
    {
        char buf[16384];
        read_text_file(path, buf, sizeof(buf));
        ASSERT_TRUE("saved t uses elapsed time",
                    strstr(buf, "t = 0.001f * (float)glutGet(GLUT_ELAPSED_TIME)") != NULL);
        ASSERT_TRUE("saved multisample header state",
                    strstr(buf, "glDisable(GL_MULTISAMPLE);") != NULL);
        ASSERT_TRUE("saved line smooth header state",
                    strstr(buf, "glEnable(GL_LINE_SMOOTH);") != NULL);
        ASSERT_TRUE("saved geometry helper",
                    strstr(buf, "static void render_repl_geometry(void)") != NULL);
        ASSERT_TRUE("saved snippet marker start",
                    strstr(buf, "// Snippet start") != NULL);
        ASSERT_TRUE("saved snippet marker end",
                    strstr(buf, "// Snippet end") != NULL);
        ASSERT_TRUE("saved outline helper omitted when disabled",
                    strstr(buf, "render_repl_outline_overlay") == NULL);
        ASSERT_TRUE("saved vertex points helper omitted when disabled",
                    strstr(buf, "render_repl_vertex_points_overlay") == NULL);
        ASSERT_TRUE("saved init ambient model line",
                    strstr(buf, "glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);") != NULL);
        ASSERT_TRUE("saved init quadric texture line",
                    strstr(buf, "gluQuadricTexture(g_quadric, GL_FALSE);") != NULL);
        ASSERT_TRUE("saved non-tess export omits tess global",
                    strstr(buf, "static GLUtesselator *g_tess = NULL;") == NULL);
        ASSERT_TRUE("saved non-tess export omits tess setup line",
                    strstr(buf, "g_tess = gluNewTess();") == NULL);
        ASSERT_TRUE("saved non-tess export omits tess callback line",
                    strstr(buf, "GLU_TESS_EDGE_FLAG") == NULL);
        ASSERT_TRUE("saved init color material line once",
                    count_substr(buf, "glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);") == 1);
        ASSERT_TRUE("saved init light model line once",
                    count_substr(buf, "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);") == 1);
        ASSERT_TRUE("saved init point attenuation line once",
                    count_substr(buf, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") == 1);
    }

    g_init_attenuate_points = 0;
    ASSERT_TRUE("init hides point attenuation when disabled",
                find_init_line_substr("glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") < 0);
    repl_save_output(path);
    {
        char buf[16384];
        read_text_file(path, buf, sizeof(buf));
        ASSERT_TRUE("saved init omits point attenuation when disabled",
                    strstr(buf, "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION") == NULL);
    }
    g_init_attenuate_points = 1;

    repl_reset_state();
    ASSERT_TRUE("load saved output", repl_load_from_file(path) == 1);
    ASSERT_TRUE("roundtrip cmd count", g_num_cmds == before_n);

    for (int i = 0; i < before_n; i++) {
        char label[64];
        snprintf(label, sizeof(label), "roundtrip type %d", i);
        ASSERT_TRUE(label, g_cmds[i].type == before_types[i]);
    }

    repl_flatten_commands();
    ASSERT_TRUE("flatten produced cmds", g_num_flat_cmds > 0);

    /* Camera state round-trip: non-default eye/center must survive save+load. */
    repl_reset_state();
    repl_feed_line_public("glBegin(GL_POINTS);");
    repl_feed_line_public("glVertex3f(0, 0, 0);");
    repl_feed_line_public("glEnd();");
    g_cam_rx   = 31.523f;
    g_cam_ry   = 31.4799f;
    g_cam_dist = 7.59313f;
    g_cam_px   = -2.00036f;
    g_cam_py   = -0.234623f;
    repl_save_output(path);
    {
        float saved_rx = g_cam_rx, saved_ry = g_cam_ry, saved_dist = g_cam_dist;
        float saved_px = g_cam_px, saved_py = g_cam_py;
        g_cam_rx = 20.0f; g_cam_ry = 30.0f; g_cam_dist = 5.0f;
        g_cam_px = 0.0f;  g_cam_py = 0.0f;
        repl_reset_state();
        ASSERT_TRUE("load camera output", repl_load_from_file(path) == 1);
        ASSERT_TRUE("camera rx restored",   fabsf(g_cam_rx   - saved_rx)   < 1e-2f);
        ASSERT_TRUE("camera ry restored",   fabsf(g_cam_ry   - saved_ry)   < 1e-2f);
        ASSERT_TRUE("camera dist restored", fabsf(g_cam_dist - saved_dist) < 1e-2f);
        ASSERT_TRUE("camera px restored",   fabsf(g_cam_px   - saved_px)   < 1e-2f);
        ASSERT_TRUE("camera py restored",   fabsf(g_cam_py   - saved_py)   < 1e-2f);
    }

    repl_reset_state();
    repl_feed_line_public("x = 1;");
    repl_feed_line_public("func0(radius, yoff) {");
    repl_feed_line_public("glVertex3f(radius, yoff, 0);");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(1.5, x + 2);");

    before_n = g_num_cmds;
    for (int i = 0; i < before_n; i++) before_types[i] = g_cmds[i].type;

    g_show_outlines = 1;
    g_show_vpoints = 1;
    repl_save_output(func_path);
    {
        char buf[32768];
        read_text_file(func_path, buf, sizeof(buf));
        ASSERT_TRUE("saved func signature",
                    strstr(buf, "static void func0(float radius, float yoff)") != NULL);
        ASSERT_TRUE("saved func emitted only once",
                    count_substr(buf, "static void func0(float radius, float yoff) {") == 1);
        ASSERT_TRUE("saved geometry helper func call",
                    strstr(buf, "func0(1.5, x + 2);") != NULL);
        ASSERT_TRUE("saved geometry helper once",
                    count_substr(buf, "static void render_repl_geometry(void)") == 1);
        ASSERT_TRUE("saved no outline helper variants",
                    strstr(buf, "render_repl_outline_") == NULL);
        ASSERT_TRUE("saved no vpoint helper variants",
                    strstr(buf, "render_repl_vpoints_") == NULL);
        ASSERT_TRUE("saved geometry called thrice in display",
                    count_substr(buf, "render_repl_geometry();") == 3);
        ASSERT_TRUE("saved outline pass polygon mode",
                    strstr(buf, "glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);") != NULL);
        ASSERT_TRUE("saved vpoints pass polygon mode",
                    strstr(buf, "glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);") != NULL);
        ASSERT_TRUE("saved overlay disables color material",
                    strstr(buf, "glDisable(GL_COLOR_MATERIAL);") != NULL);
    }

    repl_reset_state();
    ASSERT_TRUE("load saved param func output", repl_load_from_file(func_path) == 1);
    ASSERT_TRUE("param func roundtrip cmd count", g_num_cmds == before_n);
    {
        int have_var = 0, have_def = 0, have_body = 0, have_end = 0, have_call = 0;
        for (int i = 0; i < g_num_cmds; i++) {
            if (g_cmds[i].type == CMD_VAR_ASSIGN) have_var++;
            if (g_cmds[i].type == CMD_FUNC_DEF) have_def++;
            if (g_cmds[i].type == CMD_VERTEX3F) have_body++;
            if (g_cmds[i].type == CMD_FUNC_END) have_end++;
            if (g_cmds[i].type == CMD_CALL) have_call++;
        }
        ASSERT_TRUE("param func roundtrip has var assign", have_var == 1);
        ASSERT_TRUE("param func roundtrip has func def", have_def == 1);
        ASSERT_TRUE("param func roundtrip has body", have_body == 1);
        ASSERT_TRUE("param func roundtrip has func end", have_end == 1);
        ASSERT_TRUE("param func roundtrip has func call", have_call == 1);
    }

    repl_flatten_commands();
    ASSERT_TRUE("param func flatten count", g_num_flat_cmds >= 2);
    ASSERT_TRUE("param func flatten vertex type", g_flat_cmds[g_num_flat_cmds - 1].type == CMD_VERTEX3F);
    ASSERT_TRUE("param func flatten x",
                fabsf(g_flat_cmds[g_num_flat_cmds - 1].args[0] - 1.5f) < 1e-6f);
    ASSERT_TRUE("param func flatten y",
                fabsf(g_flat_cmds[g_num_flat_cmds - 1].args[1] - 3.0f) < 1e-6f);

    repl_reset_state();
    repl_feed_line_public("x = 0.25;");
    repl_feed_line_public("gluSphere(x, 16, 12);");
    repl_feed_line_public("gluCylinder(0.15, 0.05, 1.5, 8, 1);");
    repl_feed_line_public("gluDisk(0, 0.35, 12, 1);");
    repl_feed_line_public("gluPartialDisk(0.1, 0.5, 12, 4, 30, 180);");
    g_show_outlines = 0;
    g_show_vpoints = 0;
    repl_save_output(quadric_path);
    {
        char buf[16384];
        read_text_file(quadric_path, buf, sizeof(buf));
        ASSERT_TRUE("saved quadric sphere includes g_quadric",
                    strstr(buf, "gluSphere(g_quadric, x, 16, 12);") != NULL);
        ASSERT_TRUE("saved quadric cylinder includes g_quadric",
                    strstr(buf, "gluCylinder(g_quadric, 0.15, 0.05, 1.5, 8, 1);") != NULL);
        ASSERT_TRUE("saved quadric disk includes g_quadric",
                    strstr(buf, "gluDisk(g_quadric, 0, 0.35, 12, 1);") != NULL);
        ASSERT_TRUE("saved quadric partial disk includes g_quadric",
                    strstr(buf, "gluPartialDisk(g_quadric, 0.1, 0.5, 12, 4, 30, 180);") != NULL);
    }

    repl_reset_state();
    ASSERT_TRUE("load saved quadric output", repl_load_from_file(quadric_path) == 1);
    {
        int sphere_seen = 0;
        int quadric_cmds = 0;
        for (int i = 0; i < g_num_cmds; i++) {
            if (!g_cmds[i].valid)
                continue;
            if (g_cmds[i].type == CMD_GLU_SPHERE ||
                g_cmds[i].type == CMD_GLU_CYLINDER ||
                g_cmds[i].type == CMD_GLU_DISK ||
                g_cmds[i].type == CMD_GLU_PARTIAL_DISK) {
                quadric_cmds++;
                ASSERT_TRUE("loaded quadric source omits g_quadric",
                            strstr(g_cmds[i].source, "g_quadric") == NULL);
                if (g_cmds[i].type == CMD_GLU_SPHERE &&
                    strstr(g_cmds[i].source, "gluSphere(x, 16, 12);") != NULL)
                    sphere_seen = 1;
            }
        }
        ASSERT_TRUE("loaded quadric cmd count", quadric_cmds == 4);
        ASSERT_TRUE("loaded quadric sphere keeps expr source", sphere_seen == 1);
    }

    repl_reset_state();
    repl_feed_line_public("func0(radius) {");
    repl_feed_line_public("gluBegin(GLU_POLYGON);");
    repl_feed_line_public("gluBegin(GLU_CONTOUR);");
    repl_feed_line_public("for(i, 0, 4) {");
    repl_feed_line_public("gluColor(0.25 + 0.15*sin(i), 0.3, 0.4);");
    repl_feed_line_public("gluVertex(radius*cos(i), 0, radius*sin(i));");
    repl_feed_line_public("}");
    repl_feed_line_public("gluEnd();");
    repl_feed_line_public("gluEnd();");
    repl_feed_line_public("}");
    repl_feed_line_public("func0(2.0);");
    g_show_outlines = 1;
    g_show_vpoints = 1;
    repl_save_output(tess_path);
    {
        char buf[65536];
        read_text_file(tess_path, buf, sizeof(buf));
        ASSERT_TRUE("saved tess color keeps loop expr",
                    strstr(buf, "_tc[0]=0.25 + 0.15*sinf(i);") != NULL);
        ASSERT_TRUE("saved tess vertex keeps param expr",
                    strstr(buf, "_v->pos[0]=radius*cosf(i);") != NULL);
        ASSERT_TRUE("saved tess vertex keeps z expr",
                    strstr(buf, "_v->pos[2]=radius*sinf(i);") != NULL);
        ASSERT_TRUE("saved tess func emitted only once",
                    count_substr(buf, "static void func0(float radius) {") == 1);
        ASSERT_TRUE("saved tess func call once",
                    count_substr(buf, "func0(2.0);") == 1);
        ASSERT_TRUE("saved tess export includes tess global",
                    strstr(buf, "static GLUtesselator *g_tess = NULL;") != NULL);
        ASSERT_TRUE("saved tess export includes tess init",
                    strstr(buf, "g_tess = gluNewTess();") != NULL);
        ASSERT_TRUE("saved tess export includes tess callback",
                    strstr(buf, "GLU_TESS_EDGE_FLAG") != NULL);
        ASSERT_TRUE("saved tess no outline helper variants",
                    strstr(buf, "render_repl_outline_") == NULL);
        ASSERT_TRUE("saved tess no vpoint helper variants",
                    strstr(buf, "render_repl_vpoints_") == NULL);
    }

    printf("repl_core_io: %d/%d passed\n", g_pass, g_run);
    return (g_run == g_pass) ? 0 : 1;
}
