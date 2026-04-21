#ifndef REPL_COMMAND_SPEC_H
#define REPL_COMMAND_SPEC_H

#include "sample.h"

typedef struct {
    const char *name;
    int needs_semicolon;
    int needs_block_indent;
} ReplCommandTypeSpec;

const ReplCommandTypeSpec *repl_command_type_spec(CmdType type);
const char *repl_cmd_type_name(CmdType type);
int repl_cmd_type_needs_semicolon(CmdType type);
int repl_cmd_type_needs_block_indent(CmdType type);

#endif
