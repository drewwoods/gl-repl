#include "subsystems/hidden_lines/hidden_lines.h"

#include "gl_includes.h"
#include "config.h"
#include "repl/executor.h"

#define HIDDEN_LINES_TESS_VERT_BUF_SIZE 256

typedef void (*HiddenLinesGluCallback)(void);

typedef struct {
    GLdouble pos[3];
} HiddenLinesTessVertex;

static GLUtesselator *g_hidden_tess = NULL;
static HiddenLinesTessVertex g_hidden_tess_verts[HIDDEN_LINES_TESS_VERT_BUF_SIZE];
static int g_hidden_tess_vert_count = 0;

static void hidden_tess_begin_cb(GLenum mode) {
    glBegin(mode);
}

static void hidden_tess_end_cb(void) {
    glEnd();
}

static void hidden_tess_vertex_cb(void *vertex_data) {
    HiddenLinesTessVertex *v = (HiddenLinesTessVertex *)vertex_data;
    glVertex3dv(v->pos);
}

static void hidden_tess_combine_cb(GLdouble coords[3],
                                   void *vertex_data[4],
                                   GLfloat weight[4],
                                   void **out_data) {
    HiddenLinesTessVertex *v;
    (void)vertex_data;
    (void)weight;
    if (g_hidden_tess_vert_count >= HIDDEN_LINES_TESS_VERT_BUF_SIZE) {
        *out_data = NULL;
        return;
    }

    v = &g_hidden_tess_verts[g_hidden_tess_vert_count++];
    v->pos[0] = coords[0];
    v->pos[1] = coords[1];
    v->pos[2] = coords[2];
    *out_data = v;
}

static void hidden_tess_error_cb(GLenum err) {
    (void)err;
}

void hidden_lines_destroy_resources(void) {
    if (g_hidden_tess) {
        gluDeleteTess(g_hidden_tess);
        g_hidden_tess = NULL;
    }
    g_hidden_tess_vert_count = 0;
}

void hidden_lines_init_resources(void) {
    hidden_lines_destroy_resources();

    g_hidden_tess = gluNewTess();
    if (!g_hidden_tess)
        return;

    gluTessCallback(g_hidden_tess, GLU_TESS_BEGIN,
                    (HiddenLinesGluCallback)hidden_tess_begin_cb);
    gluTessCallback(g_hidden_tess, GLU_TESS_END,
                    (HiddenLinesGluCallback)hidden_tess_end_cb);
    gluTessCallback(g_hidden_tess, GLU_TESS_VERTEX,
                    (HiddenLinesGluCallback)hidden_tess_vertex_cb);
    gluTessCallback(g_hidden_tess, GLU_TESS_COMBINE,
                    (HiddenLinesGluCallback)hidden_tess_combine_cb);
    gluTessCallback(g_hidden_tess, GLU_TESS_ERROR,
                    (HiddenLinesGluCallback)hidden_tess_error_cb);
    gluTessCallback(g_hidden_tess, GLU_TESS_EDGE_FLAG,
                    (HiddenLinesGluCallback)glEdgeFlag);
}

static int hidden_lines_ensure_tess(void) {
    if (!g_hidden_tess)
        hidden_lines_init_resources();
    return g_hidden_tess != NULL;
}

static int hidden_lines_begin_mode_writes_fill_depth(GLenum mode) {
    switch (mode) {
    case GL_POINTS:
    case GL_LINES:
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        return 0;
    default:
        return 1;
    }
}

static int hidden_lines_is_wireframe_purpose(SceneExecutePurpose purpose) {
    return purpose == SCENE_EXEC_WIREFRAME_HIDDEN_LINES ||
           purpose == SCENE_EXEC_WIREFRAME_DEPTH_FILL ||
           purpose == SCENE_EXEC_WIREFRAME_VISIBLE_LINES;
}

static int hidden_lines_is_tess_cmd(CmdType type) {
    switch (type) {
    case CMD_TESS_BEGIN_POLYGON:
    case CMD_TESS_BEGIN_CONTOUR:
    case CMD_TESS_END:
    case CMD_TESS_NORMAL:
    case CMD_TESS_COLOR:
    case CMD_TESS_VERTEX:
        return 1;
    default:
        return 0;
    }
}

static void hidden_lines_execute_tess_cmd(const GLCmd *cmd, int *tess_depth) {
    if (!cmd || !tess_depth)
        return;

    switch (cmd->type) {
    case CMD_TESS_BEGIN_POLYGON:
        if (hidden_lines_ensure_tess()) {
            g_hidden_tess_vert_count = 0;
            gluTessBeginPolygon(g_hidden_tess, NULL);
            *tess_depth = 1;
        }
        break;
    case CMD_TESS_BEGIN_CONTOUR:
        if (g_hidden_tess && *tess_depth == 1) {
            gluTessBeginContour(g_hidden_tess);
            *tess_depth = 2;
        }
        break;
    case CMD_TESS_END:
        if (g_hidden_tess && *tess_depth == 2) {
            gluTessEndContour(g_hidden_tess);
            *tess_depth = 1;
        } else if (g_hidden_tess && *tess_depth == 1) {
            gluTessEndPolygon(g_hidden_tess);
            *tess_depth = 0;
        }
        break;
    case CMD_TESS_VERTEX:
        if (g_hidden_tess &&
            *tess_depth == 2 &&
            g_hidden_tess_vert_count < HIDDEN_LINES_TESS_VERT_BUF_SIZE) {
            HiddenLinesTessVertex *v =
                &g_hidden_tess_verts[g_hidden_tess_vert_count++];
            v->pos[0] = cmd->args[0];
            v->pos[1] = cmd->args[1];
            v->pos[2] = cmd->args[2];
            gluTessVertex(g_hidden_tess, v->pos, v);
        }
        break;
    case CMD_TESS_NORMAL:
    case CMD_TESS_COLOR:
    default:
        break;
    }
}

static void hidden_lines_finish_tess(int *tess_depth) {
    if (!tess_depth || !g_hidden_tess)
        return;
    if (*tess_depth == 2) {
        gluTessEndContour(g_hidden_tess);
        *tess_depth = 1;
    }
    if (*tess_depth == 1)
        gluTessEndPolygon(g_hidden_tess);
    *tess_depth = 0;
}

static int hidden_lines_cursor_owns_cmd(CmdType type) {
    if (repl_cmd_is_transform(type))
        return 1;

    switch (type) {
    case CMD_BEGIN:
    case CMD_END:
    case CMD_VERTEX3F:
    case CMD_VERTEX2F:
    case CMD_GLUT_TORUS:
    case CMD_GLUT_CUBE:
    case CMD_GLUT_SPHERE:
    case CMD_GLUT_TEAPOT:
    case CMD_GLUT_CONE:
    case CMD_GOTO_LABEL:
    case CMD_GOTO:
    case CMD_IF_BEGIN:
    case CMD_IF_END:
    case CMD_VAR_ASSIGN:
    case CMD_SCRATCH_ASSIGN:
        return 1;
    default:
        return 0;
    }
}

void hidden_lines_execute(const HiddenLinesRenderContext *ctx,
                          SceneExecutePurpose purpose) {
    ReplExecutionOptions options = {0};
    ReplExecCursor cursor;
    const GLCmd *cmd;
    int cmd_count;
    int tess_depth = 0;
    int skipping_non_fill_begin = 0;
    int depth_fill = purpose == SCENE_EXEC_WIREFRAME_DEPTH_FILL;

    if (!ctx || !hidden_lines_is_wireframe_purpose(purpose))
        return;
    if (!ctx->program.cmds || ctx->program.cmd_count <= 0)
        return;

    cmd_count = ctx->flat_cmd_count;
    if (cmd_count < 0 || cmd_count > ctx->program.cmd_count)
        cmd_count = ctx->program.cmd_count;
    if (cmd_count <= 0)
        return;

    options.flat_cmd_count = cmd_count;
    options.program = ctx->program;
    options.text = ctx->text;
    options.status_out = ctx->status_out;
    options.status_out_sz = ctx->status_out_sz;
    cursor = repl_exec_cursor_begin(&options);

    glPushMatrix();
    while ((cmd = repl_exec_cursor_peek(&cursor)) != NULL) {
        if (!cmd->valid) {
            repl_exec_cursor_advance(&cursor);
            continue;
        }

        if (skipping_non_fill_begin) {
            if (cmd->type == CMD_END)
                skipping_non_fill_begin = 0;
            repl_exec_cursor_advance(&cursor);
            continue;
        }

        if (depth_fill &&
            cmd->type == CMD_BEGIN &&
            !hidden_lines_begin_mode_writes_fill_depth((GLenum)cmd->args[0])) {
            skipping_non_fill_begin = 1;
            repl_exec_cursor_advance(&cursor);
            continue;
        }

        if (hidden_lines_is_tess_cmd(cmd->type)) {
            hidden_lines_execute_tess_cmd(cmd, &tess_depth);
            repl_exec_cursor_advance(&cursor);
            continue;
        }

        if (repl_cmd_is_transform(cmd->type) &&
            (cursor.in_begin || tess_depth != 0)) {
            repl_exec_cursor_advance(&cursor);
            continue;
        }

        if (hidden_lines_cursor_owns_cmd(cmd->type)) {
            if (!repl_exec_cursor_step(&cursor))
                break;
            continue;
        }

        repl_apply_state_bookkeeping(cmd);
        repl_exec_cursor_advance(&cursor);
    }

    hidden_lines_finish_tess(&tess_depth);
    repl_exec_cursor_end(&cursor);
    glPopMatrix();
}
