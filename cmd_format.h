/*
 * cmd_format.h — Command indentation / depth computation
 *
 * Pure functions that compute indentation from an abstract command sequence.
 * No OpenGL headers required; can be built standalone for testing.
 *
 * Build (standalone test):
 *   gcc -Wall -std=c2x -o test_format test_format.c cmd_format.c -lm
 */
#ifndef CMD_FORMAT_H
#define CMD_FORMAT_H

/* Command types the formatter cares about.
 * All other command types should map to FMT_OTHER. */
typedef enum {
    FMT_OTHER = 0,
    FMT_GL_BEGIN,            /* glBegin(MODE)          — opens a vertex block   */
    FMT_GL_END,              /* glEnd()                — closes a vertex block  */
    FMT_TESS_BEGIN_POLYGON,  /* gluBegin(GLU_POLYGON)  — opens tess polygon     */
    FMT_TESS_BEGIN_CONTOUR,  /* gluBegin(GLU_CONTOUR)  — opens tess contour     */
    FMT_TESS_END,            /* gluEnd()               — closes tess scope      */
} FmtType;

/* Minimal command descriptor: only what the formatter needs. */
typedef struct {
    FmtType type;
    int     valid;   /* 0 = deleted/invalid, skip when counting depth */
} FmtCmd;

/*
 * Returns the tessellator nesting depth by scanning cmds[0..n-1].
 *   0 = outside any gluBegin scope
 *   1 = inside gluBegin(GLU_POLYGON)
 *   2 = inside gluBegin(GLU_CONTOUR)
 * Never returns negative.
 */
int fmt_tess_depth(const FmtCmd *cmds, int n);

/*
 * Returns the glBegin/glEnd nesting depth by scanning cmds[0..n-1].
 *   0 = outside any glBegin block
 *   1 = inside a glBegin block
 * Never returns negative.
 */
int fmt_begin_depth(const FmtCmd *cmds, int n);

/*
 * Write indent for a normal (non-closing) command at position n.
 * Spaces = 2 + 2*tess_depth + 2*begin_depth
 *
 * Examples (tess=2 means inside GLU_CONTOUR, begin=1 means inside glBegin):
 *   tess=0, begin=0  →  "  "    (2 spaces)
 *   tess=1, begin=0  →  "    "  (4 spaces)
 *   tess=2, begin=0  →  "      " (6 spaces)
 *   tess=2, begin=1  →  "        " (8 spaces)
 */
void fmt_indent(const FmtCmd *cmds, int n, char *buf, int buf_sz);

/*
 * Write indent for glEnd at position n (same level as its matching glBegin).
 * Spaces = 2 + 2*tess_depth   (begin depth NOT added)
 *
 * At glEnd position, begin_depth is 1 (glBegin is before it), but glEnd should
 * align with glBegin, not indent further.
 */
void fmt_end_indent(const FmtCmd *cmds, int n, char *buf, int buf_sz);

/*
 * Write indent for gluEnd at position n (same level as its matching gluBegin).
 * Spaces = 2 + 2*(tess_depth - 1)   (closing the current tess scope)
 *
 * At gluEnd position tess_depth counts the scope being closed, so we subtract
 * one to align with the opener.
 */
void fmt_tess_end_indent(const FmtCmd *cmds, int n, char *buf, int buf_sz);

/*
 * Write indent for tessellator leaf commands (gluNormal, gluColor, gluVertex,
 * gluBegin openers).  glBegin depth is intentionally NOT added: these commands
 * belong to the tessellator scope, not the GL vertex block, so they should
 * align with the enclosing gluBegin rather than the enclosing glBegin.
 * Spaces = 2 + 2*tess_depth
 *
 * Example — tess=2, begin=1 (glBegin opened inside the contour):
 *   fmt_indent      → 8 spaces  (gl commands like glVertex3f)
 *   fmt_tess_leaf_indent → 6 spaces  (glu commands like gluNormal)
 */
void fmt_tess_leaf_indent(const FmtCmd *cmds, int n, char *buf, int buf_sz);

/*
 * Reindent a source line for display when the expression text must be
 * preserved verbatim (e.g. has_vars commands where "z" must not be replaced
 * by its current numeric value, or for-loop body lines that reference "i").
 *
 * Computes the correct indent for a normal command at position n via
 * fmt_indent(), strips leading whitespace from raw_expr, and writes
 * new_indent + stripped_expr into out.
 *
 * Example — tess=2, begin=1, raw="  glVertex3f(-1.2, -0.45, z);":
 *   out → "        glVertex3f(-1.2, -0.45, z);"   (8 spaces)
 */
void fmt_reindent_expr(const FmtCmd *cmds, int n,
                       const char *raw_expr, char *out, int out_sz);

/*
 * Reindent a source line by extracting the indent already present in
 * parsed_source (computed by parse_command) and applying it to raw_expr.
 *
 * Use this instead of fmt_reindent_expr when the correct indent was already
 * computed by parse_command — it preserves whatever indent rule (fmt_indent
 * or fmt_tess_leaf_indent) was used there, without recomputing it.
 *
 * parsed_source: the source string produced by parse_command (leading spaces
 *                give the correct indent).
 * raw_expr:      the raw REPL/file line (leading whitespace is stripped and
 *                replaced).
 */
void fmt_reindent_from_parsed(const char *parsed_source, const char *raw_expr,
                               char *out, int out_sz);

#endif /* CMD_FORMAT_H */
