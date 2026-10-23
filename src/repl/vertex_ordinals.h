/*
 * src/repl/vertex_ordinals.h - Vertex numbering of a flat command stream.
 *
 * The one place "which vertex is this?" is answered. A flatten numbers its
 * result once (repl_flat_number_vertices), into an array parallel to the flat
 * commands; the vertex walker, the cursor-primitive resolvers, the outline
 * collector and the code-panel gutter read it instead of keeping counters of
 * their own. The rules are the overlay scope contract's "Display indices"
 * paragraph in docs/ARCHITECTURE.md.
 */
#ifndef REPL_VERTEX_ORDINALS_H
#define REPL_VERTEX_ORDINALS_H

#include "repl/command.h"

/* The three ordinals of one flat vertex command. A command that is not a
 * vertex holds REPL_VERTEX_ORDINAL_NONE in all three, and a vertex holds
 * NONE in the family it does not belong to.
 *
 *   run   Display number: glVertex and gluVertex count together, one
 *         zero-based run per innermost head (glBegin or
 *         gluBegin(GLU_POLYGON), never a contour head). A glBegin strip
 *         nested in a GLU polygon owns the run, its gluVertex rows included,
 *         while it is open; at its glEnd() the polygon's run resumes at the
 *         index it had when the strip opened. This is the number the scene
 *         labels and the gutter show.
 *   imm   glVertex only, restarted by each glBegin: the ordinal space GL
 *         assembles primitives in, and the one ReplCursorPolygon ranges use.
 *   tess  gluVertex only, restarted by each gluBegin(GLU_POLYGON) and running
 *         on across its contours and through a nested strip: the space
 *         ReplCursorPolygon contour ranges use.
 *
 * imm and tess are for filtering against the cursor's primitive or contour,
 * never for display. */
typedef struct {
    int run;
    int imm;
    int tess;
} ReplVertexOrdinal;

#define REPL_VERTEX_ORDINAL_NONE (-1)

/* Number cmds[0, count) into out[0, count). An ordinal depends only on the
 * commands before it, so a prefix of a numbered stream is itself numbered:
 * clamping the count (replay) needs no renumbering. Invalid commands are
 * skipped and number nothing. */
void repl_flat_number_vertices(const GLCmd *cmds, int count,
                               ReplVertexOrdinal *out);

/* All three NONE: what a non-vertex command holds. */
static inline ReplVertexOrdinal repl_vertex_ordinal_none(void) {
    ReplVertexOrdinal none = { REPL_VERTEX_ORDINAL_NONE,
                               REPL_VERTEX_ORDINAL_NONE,
                               REPL_VERTEX_ORDINAL_NONE };
    return none;
}

/* Ordinal of flat command `flat_idx`, or all-NONE when the stream has no
 * numbering (a hand-built view) or the index is out of range. */
static inline ReplVertexOrdinal repl_vertex_ordinal_at(
        const ReplVertexOrdinal *ords, int count, int flat_idx) {
    return (ords && flat_idx >= 0 && flat_idx < count)
        ? ords[flat_idx] : repl_vertex_ordinal_none();
}

#endif /* REPL_VERTEX_ORDINALS_H */
