/*
 * src/repl/command_spec.h - Command type metadata and specifications.
 *
 * Metadata registry for GL commands, control structures, and built-in functions.
 * Describes parsing, formatting, and completion requirements for each command
 * type in a declarative way (name, argument count, format string, usage hints).
 *
 * Structure: two command spec arrays — enum-based specs (GL_BLEND, GL_LIGHTING,
 * etc. with enumeration tables) and standard specs (vertex/normal/color/transform
 * commands with simple float arguments). Control structures (for, func, if, close
 * block) are represented as ReplCommandTypeSpec entries: metadata about whether
 * they need semicolons and block indentation.
 *
 * Usage: the parser uses these specs to validate argument counts and format
 * parameter hints (argument name, type). The autocomplete system uses specs to
 * populate parameter hints and completion suggestions. The formatter uses
 * needs_block_indent to determine how to indent closing braces. The code-panel
 * renderer uses format strings to display canonical command text.
 *
 * Enum tables: each glEnable/glDisable/glBlendFunc command may reference up to
 * two enumeration tables (enums1, enums2), allowing autocomplete to suggest valid
 * parameter values at argument positions. For example, glBlendFunc has two enum
 * tables (source blend factors and destination blend factors).
 *
 * Built-in function completions: repl_func_completions() returns a table of
 * math functions (sin, cos, sqrt, etc.) for autocomplete. Enumeration entry
 * lookups (face, material params, point params) provide GL constant names.
 */
#ifndef REPL_COMMAND_SPEC_H
#define REPL_COMMAND_SPEC_H

#include "repl/command.h"

typedef struct {
    const char *name;
    GLenum      value;
} ReplEnumEntry;

#define MAX_FUNC_HINT_PARAMS 10

/* F1 help-overlay grouping for autocomplete entries. The renderer walks
 * the completion table once per group, emitting a section header and
 * one row per entry. NONE means "don't render in the command help"
 * (used by language-level entries like `func0 {`, `x = `, math
 * builtins, and the constants `PI` / `TAU` — those live in the
 * hand-written language sections of the overlay). */
typedef enum {
    REPL_HELP_GROUP_NONE = 0,
    REPL_HELP_GROUP_TOP,         /* "Supported Commands (type + ;):" */
    REPL_HELP_GROUP_LIGHTING,    /* "Lighting / Material:" */
    REPL_HELP_GROUP_GLUT_SHAPES, /* "GLUT Solid Shapes:" */
    REPL_HELP_GROUP_GLU_TESS,    /* "GLU Tessellator (concave / complex polygons):" */
    REPL_HELP_GROUP_MATH,        /* "Math Functions:" */
} ReplHelpGroup;

typedef struct {
    const char *insert_text;
    const char *display_text;
    int         param_count;
    const char *params[MAX_FUNC_HINT_PARAMS];
    /* Help-overlay description. NULL => entry is skipped in the
     * Commands tab (covered by a hand-written language section, or
     * just an autocomplete-only entry like `glPushMatrix()`). May
     * include `\n` to emit indented continuation rows under the
     * command's signature line. */
    const char   *help_desc;
    ReplHelpGroup help_group;
} ReplFuncCompletion;

/* Syntax-highlighting category for code-panel rendering. Each CmdType maps
 * to exactly one category; the renderer (ui_panels.c) translates the
 * category into an RGB color via a category→color palette. Living in the
 * spec means a new CmdType picks up the right highlight automatically — the
 * UI doesn't need to grow another switch case. */
typedef enum {
    CMD_CAT_DEFAULT = 0,    /* fallback gray; only hit for out-of-range types */
    CMD_CAT_PRIMITIVE,      /* glBegin / glEnd */
    CMD_CAT_VERTEX,         /* glVertex* / gluVertex */
    CMD_CAT_NORMAL,         /* glNormal3f / gluNormal */
    CMD_CAT_COLOR,          /* glColor* / glClearColor / glColorMaterial / glMaterialf / gluColor */
    CMD_CAT_TRANSFORM,      /* glTranslate / glScale / glRotate / glPushMatrix / glPopMatrix */
    CMD_CAT_STATE,          /* glEnable / glDisable / glShadeModel / glLineWidth / glPointSize / glBlendFunc / glDepthMask / glLightModeli / glFrontFace / glPointParameterfv */
    CMD_CAT_LOOP,           /* for { ... } */
    CMD_CAT_FUNCTION,       /* funcN { ... } / call */
    CMD_CAT_VARIABLE,       /* float decl / assignment */
    CMD_CAT_CONDITIONAL,    /* if { ... } */
    CMD_CAT_LABEL,          /* :label / goto */
    CMD_CAT_COMMENT,        /* // comment */
    CMD_CAT_GLUT_SHAPE,     /* glutSolidTorus / Cube / Sphere / Teapot / Cone */
    CMD_CAT_TESS_BLOCK,     /* gluTessBeginPolygon / Contour / End */
    CMD_CAT_COUNT
} CmdSyntaxCategory;

/* Metadata for control structures and command-type properties. Describes whether
 * a command type needs a trailing semicolon (e.g., float decl, assignment) and
 * whether it needs block-based indentation (for, func, if blocks). The category
 * field drives the code-panel renderer's syntax highlighting. */
typedef struct {
    const char *name;
    int needs_semicolon;
    int needs_block_indent;
    CmdSyntaxCategory category;
} ReplCommandTypeSpec;

/* Metadata for GL commands with enumerated arguments. Specifies the command name,
 * type, argument count, up to two enumeration tables (for multi-enum commands
 * like glBlendFunc which takes two separate enum lists), format string for
 * parameter hints, usage descriptions, and indentation type. Used by the parser
 * for validation and by autocomplete for parameter suggestions. */
typedef struct {
    const char *name;
    CmdType     type;
    int         num_args;
    const ReplEnumEntry *enums1;
    const ReplEnumEntry *enums2;
    const char *fmt;
    const char *usage1;
    const char *usage2;
    int         indent_type;
} ReplEnumCommandSpec;

/* Metadata for standard GL commands with float arguments (vertex, normal, color,
 * transform). Specifies the command name, type, argument count, format string
 * for parameter hints, usage description, and whether the command is a
 * tessellation (gluTessCallback) command. Used by the parser and autocomplete
 * for validation and suggestions. */
typedef struct {
    const char *name;
    CmdType     type;
    int         num_args;
    const char *fmt;
    const char *usage;
    int         is_tess;
} ReplStdCommandSpec;

/* Query metadata for a command type (control structure or GL command). Returns
 * a spec describing the command's name, whether it needs a semicolon, and
 * whether it needs block indentation. Used by the formatter and code-panel UI. */
const ReplCommandTypeSpec *repl_command_type_spec(CmdType type);

/* Query the human-readable name for a command type (e.g., "glVertex3f",
 * "for", "if"). Used by error messages, autocomplete hints, and UI display. */
const char *repl_cmd_type_name(CmdType type);

/* Thin alias used by tests and the demo for the unprefixed identifier
 * (cmd_type_name(CmdType)). Forwards to repl_cmd_type_name. */
const char *cmd_type_name(CmdType type);

/* Query whether a command type requires a trailing semicolon. Used by the
 * formatter and commit validation. */
int repl_cmd_type_needs_semicolon(CmdType type);

/* Query whether a command type requires block-based indentation (e.g., for/func/if
 * blocks). Used by the formatter to determine indentation rules. */
int repl_cmd_type_needs_block_indent(CmdType type);

/* Query the syntax-highlighting category for a command type. Used by the
 * code-panel renderer to pick the row color. Returns CMD_CAT_DEFAULT
 * for out-of-range types. */
CmdSyntaxCategory repl_cmd_type_category(CmdType type);

/* Query the full array of enum-backed GL command specs (glBegin, glEnable,
 * glShadeModel, etc.). Terminated by an entry with a null name. Used by the
 * parser and autocomplete to look up command metadata by type or name. */
const ReplEnumCommandSpec *repl_enum_command_specs(void);

/* Query the full array of standard float-arg GL command specs (vertex, normal,
 * color, transform, etc.). Terminated by an entry with a null name. Used by the
 * parser and autocomplete to look up command metadata by type or name. */
const ReplStdCommandSpec *repl_std_command_specs(void);

/* Query completion suggestions for built-in math functions. Used by the
 * autocomplete system to populate function name completions (sin, cos, sqrt, etc.). */
const ReplFuncCompletion *repl_func_completions(void);

/* Query enumeration tables for GL constants used in command arguments. Provides
 * suggestions for glColorMaterial face parameter, glMaterialf parameter names,
 * and glPointParameterfv pname values. Used by autocomplete to populate
 * parameter suggestions. */
const ReplEnumEntry *repl_face_type_entries(void);
const ReplEnumEntry *repl_material_param_entries(void);
const ReplEnumEntry *repl_point_param_pname_entries(void);

/* Convert a GL_TRIANGLES, GL_QUADS, etc. constant to its string name (e.g.,
 * GL_TRIANGLES → "GL_TRIANGLES"). Used for display and diagnostic output. */
const char *repl_begin_mode_name(GLenum mode);

#endif
