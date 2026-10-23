/*
 * src/repl/flatten_query.h - Queries over the live flat command stream.
 */
#ifndef REPL_FLATTEN_QUERY_H
#define REPL_FLATTEN_QUERY_H

#include "repl/flatten.h"

/* Shared provenance and geometry-block membership for overlays/replay.
 * A valid immediate source extent decides alone; tess polygons also accept
 * flat provenance so comment rows between contours retain their polygon. */
int repl_cursor_flat_cmd_matches(FlatProgramView program,
                                 const CursorBlockState *cursor, int flat_idx);
int repl_cursor_source_begin_head_matches(FlatProgramView program,
                                          const CursorBlockState *cursor,
                                          int begin_idx);
int repl_cursor_flat_block_matches(FlatProgramView program,
                                   const CursorBlockState *cursor,
                                   int begin_idx, int is_tess);

/* The same source-boundary check for collectors that already know the close.
 * NULL end means an unterminated block or a restart before its close. */
int repl_cursor_source_begin_matches(const CursorBlockState *cursor,
                                     const GLCmd *head, const GLCmd *end);

/* Display number of each source row's vertex, from the flat program's own
 * numbering (vertex_ordinals.h). out[row] is the shared glVertex/gluVertex run
 * of the LAST flat instance of that row - the copy the Last-instance scope
 * labels - or REPL_VERTEX_ORDINAL_NONE for a row that is no vertex or that the
 * flatten never emitted (a branch not taken, a function nobody calls). Only
 * rows lo..hi are resolved (lo > hi: none, and the flat program is not
 * scanned); the rest read NONE. Loop rows have no single number: the caller
 * decides how to show them. */
void repl_flatten_vertex_runs_by_row(int *out, int row_count, int lo, int hi);

/* Refresh the current-block highlight by scanning the flat program to find
 * the innermost BEGIN/END block containing the current edit line. Called
 * before building scene config to ensure cursor block bounds are current. */
void repl_flatten_refresh_current_block_highlight(int edit_line_idx);

int  repl_flat_cmd_matches_cursor(int flat_idx, int edit_line_idx);

/* ---- Flat-cost attribution (command-budget readout) ----
 *
 * How many commands of the flat (expanded) program are attributable to
 * the source line under the cursor - i.e. where the MAX_FLAT_COMMANDS
 * flatten budget is being spent. LINE / CALL / FUNC counts aggregate
 * over the whole frame: a plain line inside a loop counts once per
 * iteration, a function counts across every call site (via
 * func_scope_mask, exact at any nesting depth including recursion), and
 * a call site counts its inclusive expansion (exact for top-level calls
 * via root_call_src_cmd_idx).
 *
 * A for/if BLOCK, by contrast, counts one SINGLE invocation of the
 * scope - its first contiguous run in the flat stream (direct body plus
 * expansions of calls made inside it, exact to two levels of call
 * nesting) - NOT the whole-frame aggregate. This keeps nested scopes
 * distinct: an inner loop reports one enclosing-iteration's worth while
 * the outer loop reports its full run. A top-level or singly-invoked
 * block's single invocation is its whole frame contribution, so those
 * readouts are unchanged. Pure queries over the live flat program;
 * flatten first if the dirty flag is set. */
typedef enum {
    REPL_FLAT_COST_NONE = 0,  /* nothing attributable (comment/empty/decl) */
    REPL_FLAT_COST_LINE,      /* plain top-level line */
    REPL_FLAT_COST_CALL,      /* funcN()/alias call site */
    REPL_FLAT_COST_BLOCK,     /* for/if block (head, body, or end line) */
    REPL_FLAT_COST_FUNC,      /* funcN definition (head, body, or end line) */
} ReplFlatCostKind;

typedef struct {
    ReplFlatCostKind kind;
    int              count;
} ReplFlatCost;

ReplFlatCost repl_flatten_cost_at_line(int line_idx);

/* ---- Single-polygon cursor resolution (SINGLE_POLYGON overlay scope) ----
 *
 * Which primitive of the cursor's glBegin/glEnd block is "current"? The
 * cursor's position between vertex lines picks the polygon being built
 * there: any line after glBegin up to the Nth vertex of a primitive
 * belongs to that primitive; the line after its last vertex starts the
 * next one. The result is a block-local vertex-ordinal range - in the
 * flat program's glVertex-only numbering (vertex_ordinals.h) - resolved
 * against the cursor's cached flat block. Unrolled copies of a block
 * share their ordinal layout, so the overlay applies the range to
 * whichever copy its scope selects.
 *
 * valid == 0 means no single primitive was resolved and callers should
 * fall back to whole-block behavior: cursor outside any glBegin block,
 * cursor on the glBegin line itself, a mode where the block IS one
 * primitive (GL_POLYGON, GL_LINE_LOOP), or a vertex-less block. Tess
 * (GLU) polygons are never resolved here - the block cache tracks
 * CMD_BEGIN blocks only - so GLU geometry keeps its existing scoping.
 *
 * fan_anchor == 1 marks GL_TRIANGLE_FAN's shared center: ordinal 0
 * belongs to the selected triangle in addition to [first, last]. */
typedef struct {
    int valid;
    int first;       /* first vertex ordinal in the block, inclusive */
    int last;        /* last vertex ordinal, inclusive */
    int fan_anchor;  /* 1 = ordinal 0 (fan center) is also selected */
} ReplCursorPolygon;

ReplCursorPolygon repl_flatten_cursor_polygon(int edit_line_idx);

/* The tessellator counterpart: the gluBegin(GLU_CONTOUR) ... gluEnd() run
 * holding the cursor, as a range of polygon-local tess-vertex ordinals.
 * Ordinals count gluVertex calls from the polygon's gluBegin(GLU_POLYGON)
 * and run on across contours (the inner contour of a two-contour polygon
 * starts where the outer one stopped), which is the numbering the 3D vertex
 * labels and the code-panel gutter both show.
 *
 * The cursor counts as in a contour from its gluBegin(GLU_CONTOUR) line
 * through its gluEnd() line, inclusive. valid == 0 means whole-polygon
 * behavior: cursor outside every contour (on the polygon's own begin/end
 * line, between contours, outside any polygon), or an empty contour.
 * Resolved against the first flat instance of the contour; unrolled copies
 * of a polygon share their ordinal layout. fan_anchor is always 0. The
 * ordinals are the program's gluVertex-only numbering (vertex_ordinals.h). */
ReplCursorPolygon repl_flatten_cursor_tess_contour(int edit_line_idx);

#endif /* REPL_FLATTEN_QUERY_H */
