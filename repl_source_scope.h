/*
 * repl_source_scope.h — Source-command scope/depth queries.
 */
#ifndef REPL_SOURCE_SCOPE_H
#define REPL_SOURCE_SCOPE_H

#include "sample.h"

void depth_cache_invalidate(void);

int  in_begin_block_at(int line_idx);
int  in_begin_block(void);
int  block_depth_at(int pos);
int  tess_scope_depth_at(int pos);
void cmd_indent(int pos, char *buf, int buf_sz);
void cmd_tess_indent(int pos, char *buf, int buf_sz);
int  cmd_indent_chars(int pos);

int     find_block_end(int begin_idx);
CmdType nearest_open_block_at(int pos);

#endif
