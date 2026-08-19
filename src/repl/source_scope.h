/*
 * src/repl/source_scope.h - Source-command scope/depth queries and indentation.
 *
 * Analyzes the nesting structure of the source command array, providing query
 * APIs for block depth (how deeply nested inside for/func/if blocks a command
 * is), indentation hints for auto-formatting, and block boundary detection.
 *
 * Block depth: a command at depth 0 is top-level; inside one for/func/if block
 * it's depth 1; nested deeper it's depth 2+. Used by the auto-formatter to
 * indent new lines and by the UI to visually indent the code panel. Indentation
 * for tessellation (gluTessCallback setup) uses a separate depth counter because
 * tessellation callbacks are treated as a distinct nesting level in the visual
 * hierarchy.
 *
 * Scope detection: queries identify whether a position is inside a glBegin/glEnd
 * block (required for certain GL calls like glVertex3f, which are only valid
 * between glBegin and glEnd). Block boundary detection (find_block_end,
 * nearest_open_block_at) allows the parser and UI to locate matching braces and
 * validate nesting.
 *
 * Caching: ReplSourceScopeView owns prefix-depth arrays for the document it is
 * bound to. Callers that already have an explicit command-array snapshot bind a
 * view once and then query it in O(1). The legacy live wrappers keep a
 * process-wide view and invalidate it whenever the source command array changes
 * (via repl_source_scope_depth_cache_invalidate).
 *
 * Indentation formatting: Two variants - standard indentation for regular blocks
 * and tess-aware indentation for gluTessCallback setup. Both accept a destination
 * buffer; repl_source_scope_cmd_indent_chars() returns the character count without
 * writing. Used by the formatter and code-panel renderer for visual indentation.
 */
#ifndef REPL_SOURCE_SCOPE_H
#define REPL_SOURCE_SCOPE_H

#include "repl/command.h"

/* REPL_INDENT_TEXT_MAX (the shared indent-buffer capacity for callers of
 * the *_indent helpers below) lives in config.h so editor / UI code can
 * size their indent buffers without pulling in this header. */

typedef struct ReplSourceScopeView {
    const GLCmd *cmds;
    int count;
    int built;
    int block_depth_prefix[MAX_EDITOR_COMMANDS + 1];
    int begin_depth_prefix[MAX_EDITOR_COMMANDS + 1];
    int tess_depth_prefix[MAX_EDITOR_COMMANDS + 1];
    int matrix_depth_prefix[MAX_EDITOR_COMMANDS + 1];
} ReplSourceScopeView;

typedef struct {
    const ReplSourceScopeView *scope;
} ReplSourceScopeLiveView;

/* Bind a source-scope view to an explicit command array and build its
 * prefix-depth cache immediately. The bound document must remain stable while
 * the view is queried. */
void repl_source_scope_view_bind(ReplSourceScopeView *view,
                                 const GLCmd *cmds, int count);

/* Return a handle to the warm live-document view. The pointed-to view remains
 * valid until the next source-scope invalidation. Prefer explicit
 * ReplSourceScopeView instances for snapshot/non-live documents. */
ReplSourceScopeLiveView repl_source_scope_live_view(void);

int  repl_source_scope_view_in_begin_block_at(const ReplSourceScopeView *view,
                                              int line_idx);
int  repl_source_scope_view_block_depth_at(const ReplSourceScopeView *view,
                                           int pos);
int  repl_source_scope_view_tess_scope_depth_at(const ReplSourceScopeView *view,
                                                int pos);
int  repl_source_scope_view_matrix_scope_depth_at(const ReplSourceScopeView *view,
                                                  int pos);
void repl_source_scope_view_cmd_indent(const ReplSourceScopeView *view,
                                       int pos, char *buf, int buf_sz);
void repl_source_scope_view_begin_indent(const ReplSourceScopeView *view,
                                         int pos, char *buf, int buf_sz);
void repl_source_scope_view_tess_close_indent(const ReplSourceScopeView *view,
                                              int pos, char *buf, int buf_sz);
void repl_source_scope_view_cmd_tess_indent(const ReplSourceScopeView *view,
                                            int pos, char *buf, int buf_sz);
void repl_source_scope_view_matrix_close_indent(const ReplSourceScopeView *view,
                                                int pos, char *buf, int buf_sz);
int  repl_source_scope_view_cmd_indent_chars(const ReplSourceScopeView *view,
                                             int pos);
int  repl_source_scope_view_find_block_end(const ReplSourceScopeView *view,
                                           int begin_idx);
CmdType repl_source_scope_view_nearest_open_block_at(const ReplSourceScopeView *view,
                                                     int pos);
int repl_source_scope_view_in_loop_at(const ReplSourceScopeView *view,
                                      int pos);
int repl_source_scope_view_block_extent(const ReplSourceScopeView *view,
                                        int line_idx,
                                        int *out_start, int *out_count);
int repl_source_scope_view_line_is_block_head(const ReplSourceScopeView *view,
                                              int line_idx);
int repl_source_scope_view_collect_unbalanced(const ReplSourceScopeView *view,
                                              int *out_lines, int max);

/* Invalidate the live-document prefix-depth cache. Called whenever the source command array
 * changes (e.g., after insert, delete, or edit) to force recomputation of block
 * depths on the next query. */
void repl_source_scope_depth_cache_invalidate(void);

/* Check whether a command is inside a glBegin/glEnd block. Returns 1 if the
 * command at line_idx is between a glBegin and its matching glEnd, 0 otherwise.
 * repl_source_scope_in_begin_block() checks the current editor line. Used by
 * the parser to validate that vertex/normal/color commands are inside a
 * glBegin/glEnd pair. */
int  repl_source_scope_in_begin_block_at(int line_idx);
int  repl_source_scope_in_begin_block(void);

/* Compute block depth: how deeply nested the command at pos is inside
 * for/func/if blocks. Depth 0 = top-level, depth 1 = inside one block, etc.
 * repl_source_scope_tess_scope_depth_at() treats gluTessCallback setup as a
 * distinct depth level for indentation. Used by the formatter and code-panel
 * renderer for visual indentation. */
int  repl_source_scope_block_depth_at(int pos);
int  repl_source_scope_tess_scope_depth_at(int pos);

/* Matrix-stack scope depth: how many glPushMatrix scopes are open before
 * the command at pos (glPushMatrix opens a level, glPopMatrix closes it).
 * Folded into every indent helper so push/pop bodies indent like a
 * glBegin block. Used by the editor's manual indent rewriter. */
int  repl_source_scope_matrix_scope_depth_at(int pos);

/* Collect the document indices of structurally unbalanced bracket
 * commands: glPushMatrix/glBegin openers with no matching close, and
 * orphan glPopMatrix/glEnd closers with no matching open. Linear stack
 * matching (the REPL's relaxed model, matching the indentation depth).
 * Fills out_lines and returns the count, capped at max. */
int  repl_source_scope_collect_unbalanced(int *out_lines, int max);

/* Format indentation for a command at pos. Writes spaces to buf (up to buf_sz)
 * representing the appropriate indentation level. repl_source_scope_cmd_tess_indent()
 * uses tess-aware depth for tessellation callback setup. Used by the formatter
 * and code-panel renderer. */
void repl_source_scope_cmd_indent(int pos, char *buf, int buf_sz);

/* glBegin/glEnd-style indent (2 + 2*tess + 2*block + 2*matrix; begin-depth
 * excluded). Used for the glBegin and matching glEnd lines. */
void repl_source_scope_begin_indent(int pos, char *buf, int buf_sz);
void repl_source_scope_tess_close_indent(int pos, char *buf, int buf_sz);
void repl_source_scope_cmd_tess_indent(int pos, char *buf, int buf_sz);

/* glPopMatrix indent: like a normal command but one matrix level shallower,
 * so the glPopMatrix line aligns with its matching glPushMatrix (mirrors how
 * glEnd aligns with glBegin via repl_source_scope_begin_indent). */
void repl_source_scope_matrix_close_indent(int pos, char *buf, int buf_sz);

/* Return the number of indentation characters (spaces) for a command at pos,
 * without writing to a buffer. Used by layout and sizing queries. */
int  repl_source_scope_cmd_indent_chars(int pos);

/* Locate block boundaries. repl_source_scope_find_block_end() returns the index
 * of the closing brace for a block starting at begin_idx (which should be a
 * CMD_FOR, CMD_FUNC_DEF, or CMD_IF). repl_source_scope_nearest_open_block_at()
 * returns the type of the innermost open block at pos (CMD_FOR, CMD_FUNC_DEF,
 * CMD_IF, or CMD_NONE if at top-level), used for validation and error messages. */
int     repl_source_scope_find_block_end(int begin_idx);
CmdType repl_source_scope_nearest_open_block_at(int pos);

/* Function-navigation queries. "Enclosing" includes the CMD_FUNC_DEF and
 * matching CMD_FUNC_END rows, so a cursor sitting on the header or closer
 * still belongs to that function. Nested for/if frames are transparent.
 *
 * repl_source_scope_enclosing_func_at() writes the FUNC_DEF index and its
 * matching closer (or -1 if unmatched) and returns 1, or 0 when `pos` is
 * outside any function. next/prev return the first FUNC_DEF strictly after
 * / before `pos`, or -1. */
int repl_source_scope_view_enclosing_func_at(const ReplSourceScopeView *view,
                                            int pos,
                                            int *out_def, int *out_end);
int repl_source_scope_enclosing_func_at(int pos, int *out_def, int *out_end);
int repl_source_scope_view_next_func_def(const ReplSourceScopeView *view,
                                         int pos);
int repl_source_scope_next_func_def(int pos);
int repl_source_scope_view_prev_func_def(const ReplSourceScopeView *view,
                                         int pos);
int repl_source_scope_prev_func_def(int pos);

/* Locate the definition of function slot `fn`. Returns the position of the
 * first CMD_FUNC_DEF whose args[0] is `fn`, or -1 when the slot has no
 * definition (or `fn` is outside 0..REPL_FUNC_SLOT_COUNT-1). Positions are
 * document row indices, the same space as _next_func_def / _prev_func_def. */
int repl_source_scope_view_find_func_def_line(const ReplSourceScopeView *view,
                                              int fn);
int repl_source_scope_find_func_def_line(int fn);

/* Returns 1 if `pos` sits inside a for-loop body that a `break` / `continue`
 * statement there would bind to. If-blocks between the statement and the loop
 * are transparent; an enclosing CMD_FUNC_DEF is a hard boundary. Used by the
 * parser to reject a loop-less break/continue at commit time. */
int repl_source_scope_in_loop_at(int pos);

/* Return the inclusive [head..end] extent of a structured block. For
 * CMD_FOR_BEGIN / CMD_FUNC_DEF / CMD_IF_BEGIN rows, `line_idx` is the block
 * head. For CMD_ELSE_IF / CMD_ELSE rows, the extent expands to the whole
 * enclosing if-chain. Returns 0 otherwise. Used by clipboard and other editor
 * features to expand a single-line operation to whole-block scope without
 * reading CmdType themselves. */
int repl_source_scope_block_extent(int line_idx,
                                   int *out_start, int *out_count);

/* Returns 1 if the line at `line_idx` is a structured-block head
 * (FOR_BEGIN, FUNC_DEF, or IF_BEGIN). Used by the editor's Enter-key
 * dispatch to apply sticky-edit semantics on block headers without
 * reading CmdType directly. */
int repl_line_is_block_head(int line_idx);

/* Range/array predicates: does the given command range / array contain any
 * CMD_VAR_DECLARE row? Used by clipboard, delete, and other guards that
 * need to refuse operations that would orphan declared variables, while
 * keeping the editor side free of CmdType reads.
 *
 * - _range walks the live document over [start, start+count).
 * - _array walks an external GLCmd[] of the supplied count (e.g., the
 *   clipboard buffer), so paste-time guards don't need to reach into
 *   the document.
 *
 * Both are bounds-checked and return 0 for empty / invalid ranges. */
int repl_range_contains_var_decl(int start, int count);
int repl_array_contains_var_decl(const GLCmd *cmds, int count);

#endif
