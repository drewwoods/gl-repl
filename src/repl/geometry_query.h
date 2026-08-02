/*
 * src/repl/geometry_query.h - Source/flat geometry context queries.
 */
#ifndef REPL_GEOMETRY_QUERY_H
#define REPL_GEOMETRY_QUERY_H

int repl_find_feeding_normal_cmd(int line_idx);
int repl_find_feeding_color_cmd(int line_idx);

/* When the cursor sits on a CMD_POP_MATRIX line, returns the source-line
 * index of the matching CMD_PUSH_MATRIX (the nearest earlier push at the
 * same nesting level). Returns -1 if the cursor isn't on a pop or no
 * matching push exists. */
int repl_find_matching_push_matrix(int line_idx);

/* Mirror of repl_find_matching_push_matrix: when the cursor sits on a
 * CMD_PUSH_MATRIX line, returns the source-line index of the matching
 * CMD_POP_MATRIX (the nearest later pop at the same nesting level).
 * Returns -1 if the cursor isn't on a push or no matching pop exists. */
int repl_find_matching_pop_matrix(int line_idx);

/* Attribute-stack (glPushAttrib/glPopAttrib) analogues of the matrix bracket
 * matchers above, using the same source-order LIFO heuristic. push→pop when
 * the cursor is on a CMD_POP_ATTRIB; pop→push when on a CMD_PUSH_ATTRIB.
 * Returns -1 for the wrong cursor line or no partner. */
int repl_find_matching_push_attrib(int line_idx);
int repl_find_matching_pop_attrib(int line_idx);

/* Primitive-block (glBegin/glEnd) analogues of the same bracket matchers.
 * begin→end when the cursor is on a CMD_BEGIN; end→begin when on a CMD_END.
 * Returns -1 for the wrong cursor line or no partner. */
int repl_find_matching_begin(int line_idx);
int repl_find_matching_end(int line_idx);

/* When the cursor sits on a color-consuming line (immediate vertex,
 * gluVertex, or glutSolid*), fills out_line_idx[] with up to out_cap
 * source-line indices of the modelview-affecting transforms in scope
 * at that line: CMD_TRANSLATE3F / CMD_SCALEF / CMD_ROTATEF. Walks the
 * source array backwards, honors glPushMatrix/glPopMatrix scopes (a
 * transform inside a popped range is excluded), stops at the nearest
 * CMD_LOAD_IDENTITY in scope, skips function bodies, and stops at the
 * enclosing CMD_FUNC_DEF when the cursor lives inside a function.
 * Returns the number of entries written; 0 if the cursor isn't on a
 * color-consuming line. */
#define MAX_AFFECTING_TRANSFORMS 32
int repl_find_affecting_transforms(int line_idx, int *out_line_idx, int out_cap);

/* Flat-program (cross-function-accurate) variants of the affecting-transform
 * lookup. The source walk above stops at CMD_FUNC_DEF and treats function
 * bodies as opaque, so a vertex *inside* a funcN body never sees the
 * calling-scope transforms. These walk the flattened program instead, where
 * every funcN body is already inlined and every loop unrolled, so call-site
 * transforms and in-body transforms both count.
 *
 * repl_find_affecting_transforms_for_flat_vertex: takes one concrete flat
 * vertex / tess-vertex / glut-solid command index, walks the flat array
 * backward honoring glPushMatrix/glPopMatrix/glLoadIdentity, and fills
 * out_line_idx[] with the deduped *source* lines of the in-scope transforms.
 *
 * repl_find_affecting_transforms_flat: live-cursor wrapper keyed on a source
 * line. Finds every flat expansion of that source vertex line and returns the
 * union of affecting transform source lines across them - deterministic even
 * for reused/recursive function-body vertices with no selected invocation.
 *
 * Both return the number of source lines written; 0 if the target line/index
 * isn't a color-consuming command or the flat program is empty. */
int repl_find_affecting_transforms_for_flat_vertex(int flat_idx,
                                                  int *out_line_idx, int out_cap);
int repl_find_affecting_transforms_flat(int line_idx, int *out_line_idx, int out_cap);

#endif /* REPL_GEOMETRY_QUERY_H */
