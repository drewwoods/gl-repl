/*
 * repl_source_scope.h - Source-command scope/depth queries.
 */
#ifndef REPL_SOURCE_SCOPE_H
#define REPL_SOURCE_SCOPE_H

#include "sample.h"

void repl_source_scope_depth_cache_invalidate(void);

int  repl_source_scope_in_begin_block_at(int line_idx);
int  repl_source_scope_in_begin_block(void);
int  repl_source_scope_block_depth_at(int pos);
int  repl_source_scope_tess_scope_depth_at(int pos);
void repl_source_scope_cmd_indent(int pos, char *buf, int buf_sz);
void repl_source_scope_cmd_tess_indent(int pos, char *buf, int buf_sz);
int  repl_source_scope_cmd_indent_chars(int pos);

int     repl_source_scope_find_block_end(int begin_idx);
CmdType repl_source_scope_nearest_open_block_at(int pos);

#endif
