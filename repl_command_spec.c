#include "repl_command_spec.h"

#define CMD_TYPE_SPEC(type_, semicolon_, block_indent_) \
    [type_] = { #type_, (semicolon_), (block_indent_) }

static const ReplCommandTypeSpec g_command_type_specs[CMD_TYPE_COUNT] = {
    CMD_TYPE_SPEC(CMD_BEGIN, 1, 1),
    CMD_TYPE_SPEC(CMD_END, 1, 1),
    CMD_TYPE_SPEC(CMD_VERTEX3F, 1, 1),
    CMD_TYPE_SPEC(CMD_VERTEX2F, 1, 1),
    CMD_TYPE_SPEC(CMD_NORMAL3F, 1, 1),
    CMD_TYPE_SPEC(CMD_COLOR3F, 1, 1),
    CMD_TYPE_SPEC(CMD_COLOR4F, 1, 1),
    CMD_TYPE_SPEC(CMD_ENABLE, 1, 1),
    CMD_TYPE_SPEC(CMD_DISABLE, 1, 1),
    CMD_TYPE_SPEC(CMD_SHADE_MODEL, 1, 1),
    CMD_TYPE_SPEC(CMD_TRANSLATE3F, 1, 1),
    CMD_TYPE_SPEC(CMD_SCALEF, 1, 1),
    CMD_TYPE_SPEC(CMD_ROTATEF, 1, 1),
    CMD_TYPE_SPEC(CMD_PUSH_MATRIX, 1, 1),
    CMD_TYPE_SPEC(CMD_POP_MATRIX, 1, 1),
    CMD_TYPE_SPEC(CMD_COLOR_MATERIAL, 1, 1),
    CMD_TYPE_SPEC(CMD_LIGHT_MODEL_I, 1, 1),
    CMD_TYPE_SPEC(CMD_FRONT_FACE, 1, 1),
    CMD_TYPE_SPEC(CMD_FOR_BEGIN, 1, 0),
    CMD_TYPE_SPEC(CMD_FOR_END, 1, 0),
    CMD_TYPE_SPEC(CMD_FUNC_DEF, 1, 0),
    CMD_TYPE_SPEC(CMD_FUNC_END, 1, 0),
    CMD_TYPE_SPEC(CMD_CALL, 1, 0),
    CMD_TYPE_SPEC(CMD_IF_BEGIN, 1, 0),
    CMD_TYPE_SPEC(CMD_IF_END, 1, 0),
    CMD_TYPE_SPEC(CMD_COMMENT, 0, 0),
    CMD_TYPE_SPEC(CMD_VAR_ASSIGN, 1, 0),
    CMD_TYPE_SPEC(CMD_VAR_DECLARE, 0, 0),
    CMD_TYPE_SPEC(CMD_LABEL, 0, 0),
    CMD_TYPE_SPEC(CMD_GOTO, 1, 0),
    CMD_TYPE_SPEC(CMD_GLU_SPHERE, 1, 1),
    CMD_TYPE_SPEC(CMD_GLU_CYLINDER, 1, 1),
    CMD_TYPE_SPEC(CMD_GLU_DISK, 1, 1),
    CMD_TYPE_SPEC(CMD_GLU_PARTIAL_DISK, 1, 1),
    CMD_TYPE_SPEC(CMD_GLUT_TORUS, 1, 1),
    CMD_TYPE_SPEC(CMD_TESS_BEGIN_POLYGON, 1, 1),
    CMD_TYPE_SPEC(CMD_TESS_BEGIN_CONTOUR, 1, 1),
    CMD_TYPE_SPEC(CMD_TESS_END, 1, 1),
    CMD_TYPE_SPEC(CMD_TESS_NORMAL, 1, 1),
    CMD_TYPE_SPEC(CMD_TESS_COLOR, 1, 1),
    CMD_TYPE_SPEC(CMD_TESS_VERTEX, 1, 1),
    CMD_TYPE_SPEC(CMD_MATERIALF, 1, 1),
    CMD_TYPE_SPEC(CMD_POINT_SIZE, 1, 1),
    CMD_TYPE_SPEC(CMD_POINT_PARAMETER_FV, 1, 1),
    CMD_TYPE_SPEC(CMD_BLEND_FUNC, 1, 1),
    CMD_TYPE_SPEC(CMD_CLEAR_COLOR, 1, 1),
};

const ReplCommandTypeSpec *repl_command_type_spec(CmdType type) {
    if (type < 0 || type >= CMD_TYPE_COUNT)
        return NULL;
    if (!g_command_type_specs[type].name)
        return NULL;
    return &g_command_type_specs[type];
}

const char *repl_cmd_type_name(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    return spec ? spec->name : "CMD_UNKNOWN";
}

int repl_cmd_type_needs_semicolon(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    return spec ? spec->needs_semicolon : 1;
}

int repl_cmd_type_needs_block_indent(CmdType type) {
    const ReplCommandTypeSpec *spec = repl_command_type_spec(type);
    return spec ? spec->needs_block_indent : 1;
}

