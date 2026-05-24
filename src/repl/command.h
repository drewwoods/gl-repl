/*
 * src/repl/command.h - Core REPL command model types.
 *
 * MAX_COMMANDS / MAX_LINE_LEN moved to config.h (the neutral limits
 * home that source_document.h also draws from). This header only
 * pulls them in via config.h so the two values can't drift.
 */
#ifndef REPL_COMMAND_H
#define REPL_COMMAND_H

#include "gl_includes.h"

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

/* Loosely grouped by kind (geometry, transforms, control flow, GLUT
 * shapes, tess, attributes, ...). Switch dispatch in executor.c /
 * flatten.c / parser.c / replay_annotations.c is keyed on these, so
 * values are append-stable: place a new command next to its relatives
 * rather than reordering existing entries. CMD_TYPE_COUNT stays last. */
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
    CMD_DEPTH_FUNC,
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
    CMD_MATERIALFV,
    CMD_MATERIALF,
    CMD_POINT_SIZE,
    CMD_LINE_WIDTH,
    CMD_POINT_PARAMETER_FV,
    CMD_BLEND_FUNC,
    CMD_CLEAR_COLOR,
    CMD_DEPTH_MASK,
    CMD_COLOR_MASK,
    CMD_RASTER_POS3F,
    CMD_LABEL,
    CMD_TYPE_COUNT
} CmdType;

typedef struct {
    CmdType  type;
    /* No `mode` field: every enum-backed command (table-driven *and*
     * the custom glMaterialfv / glPointParameterfv branches) stores its
     * GL enum arguments in args[] alongside numeric args. GLenums fit
     * float32 exactly (all in use are < 2^24), so (GLenum)args[i]
     * round-trips losslessly. The absence of this field is the
     * compiler-enforced invariant — no grep guard needed. */
    float    args[8];
    int      num_args;              /* Number of meaningful entries in args[] */
    int      var_idx;               /* Predef-var slot for CMD_VAR_ASSIGN
                                     * (the executor / flatten / core read this
                                     * to apply the value held in args[0]).
                                     * Zero / unused for every other CmdType. */
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

/* True for every command type that contributes a per-vertex position to
 * GL: glVertex3f, glVertex2f (implicit z=0), and gluVertex (the tess
 * variant). Use this anywhere the question is "does this cmd emit a
 * vertex?". For tess-vs-immediate-mode distinctions (e.g. choosing
 * between CMD_NORMAL3F and CMD_TESS_NORMAL as the feeding state cmd),
 * spell the subset out at the call site — that's intent, not a
 * uniform predicate. */
static inline int repl_cmd_emits_vertex(CmdType type) {
    return (type == CMD_VERTEX3F ||
            type == CMD_VERTEX2F ||
            type == CMD_TESS_VERTEX);
}

/* True for the opening commands of REPL control-flow blocks: for-loop,
 * function definition, if-block. Block depth tracking treats all three
 * the same way ("a new scope just opened, increment depth"). Used
 * alongside repl_cmd_is_block_end below.
 *
 * Note: these three types live in three separate CmdSyntaxCategory
 * values (CMD_CAT_LOOP / CMD_CAT_FUNCTION / CMD_CAT_CONDITIONAL) — the
 * categories are about syntax-highlight color, this predicate is about
 * control-flow shape. Two distinct axes. */
static inline int repl_cmd_is_block_head(CmdType type) {
    return (type == CMD_FOR_BEGIN ||
            type == CMD_FUNC_DEF  ||
            type == CMD_IF_BEGIN);
}

/* Mirror of repl_cmd_is_block_head — the closing commands of REPL
 * control-flow blocks. */
static inline int repl_cmd_is_block_end(CmdType type) {
    return (type == CMD_FOR_END ||
            type == CMD_FUNC_END ||
            type == CMD_IF_END);
}

#endif /* REPL_COMMAND_H */