#ifndef REPL_COMMAND_SPEC_H
#define REPL_COMMAND_SPEC_H

#include "sample.h"

typedef struct {
    const char *name;
    int needs_semicolon;
    int needs_block_indent;
} ReplCommandTypeSpec;

typedef struct {
    const char *name;
    CmdType     type;
    int         num_args;
    const EnumEntry *enums1;
    const EnumEntry *enums2;
    const char *fmt;
    const char *usage1;
    const char *usage2;
    int         indent_type;
} ReplEnumCommandSpec;

typedef struct {
    const char *name;
    CmdType     type;
    int         num_args;
    const char *fmt;
    const char *usage;
    int         is_tess;
} ReplStdCommandSpec;

const ReplCommandTypeSpec *repl_command_type_spec(CmdType type);
const char *repl_cmd_type_name(CmdType type);
int repl_cmd_type_needs_semicolon(CmdType type);
int repl_cmd_type_needs_block_indent(CmdType type);

const ReplEnumCommandSpec *repl_enum_command_specs(void);
const ReplStdCommandSpec *repl_std_command_specs(void);
const FuncCompletion *repl_func_completions(void);
const EnumEntry *repl_face_type_entries(void);
const EnumEntry *repl_material_param_entries(void);
const EnumEntry *repl_point_param_pname_entries(void);
const char *repl_begin_mode_name(GLenum mode);

#endif
