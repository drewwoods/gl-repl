/*
 * src/repl/command.h - Core REPL command model types.
 *
 * MAX_COMMANDS / MAX_LINE_LEN moved to config.h (the neutral limits
 * home that source_document.h also draws from). This header only
 * pulls them in via config.h so the two values can't drift.
 */
#ifndef REPL_COMMAND_H
#define REPL_COMMAND_H

#include <gl_includes.h>

#include "config.h"  /* MAX_COMMANDS, MAX_LINE_LEN */

/* Maximum format-string length for CMD_LABEL (excluding
 * the surrounding quotes and trailing NUL). 64 fits ~5 short %f
 * substitutions plus surrounding text and stays well within the
 * MAX_LINE_LEN budget when the canonical line is rebuilt. */
#ifndef GLUT_BITMAP_FMT_MAX
#define GLUT_BITMAP_FMT_MAX 64
#endif

/* Maximum number of %f substitution args for CMD_LABEL.
 * Position takes args[0..2], substitutions live in args[3..6]; the
 * 8-slot args[] cap is enforced both here and in the parser. */
#ifndef GLUT_BITMAP_MAX_SUB_ARGS
#define GLUT_BITMAP_MAX_SUB_ARGS 4
#endif

#include "repl/eval.h"

typedef enum {
    CMD_BEGIN, CMD_END,
    CMD_VERTEX3F, CMD_VERTEX2F,
    CMD_NORMAL3F,
    CMD_COLOR3F, CMD_COLOR4F,
    CMD_ENABLE, CMD_DISABLE,
    CMD_SHADE_MODEL,
    CMD_TRANSLATE3F,
    CMD_SCALEF,
    CMD_ROTATEF,
    CMD_PUSH_MATRIX,
    CMD_POP_MATRIX,
    CMD_LOAD_IDENTITY,
    CMD_COLOR_MATERIAL,
    CMD_LIGHT_MODEL_I,
    CMD_FRONT_FACE,
    CMD_FOR_BEGIN, CMD_FOR_END,
    CMD_FUNC_DEF, CMD_FUNC_END, CMD_CALL,
    CMD_IF_BEGIN, CMD_IF_END,
    CMD_COMMENT,
    CMD_EMPTY,
    CMD_VAR_ASSIGN,
    CMD_SCRATCH_ASSIGN,
    CMD_VAR_DECLARE,
    CMD_GOTO_LABEL, CMD_GOTO,
    CMD_GLUT_TORUS, CMD_GLUT_CUBE, CMD_GLUT_SPHERE, CMD_GLUT_TEAPOT, CMD_GLUT_CONE,
    CMD_TESS_BEGIN_POLYGON,
    CMD_TESS_BEGIN_CONTOUR,
    CMD_TESS_END,
    CMD_TESS_NORMAL,
    CMD_TESS_COLOR,
    CMD_TESS_VERTEX,
    CMD_MATERIALF,
    CMD_POINT_SIZE,
    CMD_LINE_WIDTH,
    CMD_POINT_PARAMETER_FV,
    CMD_BLEND_FUNC,
    CMD_CLEAR_COLOR,
    CMD_DEPTH_MASK,
    CMD_RASTER_POS3F,
    CMD_LABEL,
    CMD_TYPE_COUNT
} CmdType;

typedef struct {
    CmdType  type;
    GLenum   mode;
    float    args[8];
    int      num_args;              /* Number of meaningful entries in args[] */
    int      valid;                 /* Deleted commands remain allocated but skipped */
    int      is_auto;               /* Auto-generated helper, e.g. synthesized normals */
    int      has_vars;              /* Source must be preserved/re-evaluated from text */
    char     var_names[MAX_NAMES_PER_DECL][16];
    int      var_decl_count;        /* Number of names in a CMD_VAR_DECLARE line */
    char     text[GLUT_BITMAP_FMT_MAX]; /* Format string for CMD_LABEL (no quotes) */
    int      src_cmd_idx;           /* Owning source command for flat->source mapping */
    int      call_src_cmd_idx;      /* Immediate call site that expanded this command */
    int      root_call_src_cmd_idx; /* Outermost call site in nested expansion */
    unsigned int func_scope_mask;   /* Function scopes active when command was flattened */
} GLCmd;

static inline int repl_cmd_is_transform(CmdType type) {
    return (type == CMD_TRANSLATE3F || type == CMD_SCALEF  || type == CMD_ROTATEF ||
            type == CMD_PUSH_MATRIX || type == CMD_POP_MATRIX ||
            type == CMD_LOAD_IDENTITY);
}

#endif /* REPL_COMMAND_H */