/*
 * repl_source_scope.h - Source-command scope/depth queries and indentation.
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
 * Caching: A prefix-depth cache (repl_source_scope.c) memoizes block-depth
 * computation from line 0 to each position. The cache is invalidated whenever
 * the source command array changes (via repl_source_scope_depth_cache_invalidate).
 * Cache hits avoid O(n) re-traversal on each query, keeping UI responsiveness
 * constant.
 *
 * Indentation formatting: Two variants — standard indentation for regular blocks
 * and tess-aware indentation for gluTessCallback setup. Both accept a destination
 * buffer; repl_source_scope_cmd_indent_chars() returns the character count without
 * writing. Used by the formatter and code-panel renderer for visual indentation.
 */
#ifndef REPL_SOURCE_SCOPE_H
#define REPL_SOURCE_SCOPE_H

#include "repl_command.h"

/* Invalidate the prefix-depth cache. Called whenever the source command array
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

/* Format indentation for a command at pos. Writes spaces to buf (up to buf_sz)
 * representing the appropriate indentation level. repl_source_scope_cmd_tess_indent()
 * uses tess-aware depth for tessellation callback setup. Used by the formatter
 * and code-panel renderer. */
void repl_source_scope_cmd_indent(int pos, char *buf, int buf_sz);
void repl_source_scope_cmd_tess_indent(int pos, char *buf, int buf_sz);

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

#endif
