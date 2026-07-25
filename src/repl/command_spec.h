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
 * they need semicolons plus their syntax-highlighting category.
 *
 * Usage: the parser uses these specs to validate argument counts and format
 * parameter hints (argument name, type). The autocomplete system uses specs to
 * populate parameter hints and completion suggestions. The code-panel renderer
 * uses format strings to display canonical command text.
 *
 * Enum tables: each enum-backed command declares an array of positional
 * ReplEnumArgSpec slots (args[]), each carrying its enum token table,
 * per-slot usage diagnostic, and ReplEnumSlotKind. Autocomplete and the
 * generalized N-arg parser both read args[slot]. For example,
 * glBlendFunc has two slots (source blend factors, destination blend
 * factors).
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

/* Maximum number of positional enum arguments a single enum-backed GL
 * command can declare. Sized to GLCmd.args[8] so the enum path and the
 * std path share one storage width and a future wider enum command does
 * not force another struct-shape change. */
#define MAX_ENUM_ARGS 8

/* How one positional enum argument resolves a typed token. The three
 * kinds are deliberately distinct so strict (non-bool) enum slots stay
 * behavior-neutral while boolean mask slots can opt into numeric 1/0
 * support without broadening every enum command:
 *
 *  - ENUM_ONLY: exact enum-token match only; numeric and expression
 *    input are rejected. This is byte-for-byte the historic behavior of
 *    every strict enum slot (glBegin, glEnable/Disable, glShadeModel,
 *    glFrontFace, glColorMaterial x2, glBlendFunc x2, glLightModeli
 *    slot 0).
 *  - ENUM_OR_CONST_VALUE: token first, else a well-formed numeric
 *    literal (strict — no trailing junk, no runtime vars) reverse-
 *    mapped to a value in this slot's table. The bool-slot policy:
 *    used by glDepthMask and all four glColorMask channels, so
 *    glDepthMask(1) / glColorMask(1, 0, 1, 0) canonicalize to
 *    GL_TRUE/GL_FALSE.
 *  - ENUM_OR_EXPR: token first, else a full expression (vars
 *    permitted); the typed source token is emitted verbatim. Only
 *    glLightModeli slot 1 uses this, reproducing its historic
 *    bool-token-or-int/expr param.
 *  - ENUM_BITFIELD: one or more tokens from this slot's table OR'd
 *    together with `|`. Numeric and expression input are rejected, as
 *    with ENUM_ONLY; the resolved mask is emitted in table order with
 *    duplicates dropped, so `GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT`
 *    canonicalizes to `GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT`. A slot
 *    may additionally name an all-bits alias; it resolves to the union of
 *    the table and is emitted whenever the resolved mask equals that union.
 *    A table wired to this kind must carry non-zero, single-bit values:
 *    ordinary emission tests each entry with `mask & value`, which a
 *    zero-valued entry (GL_FALSE, say) could never satisfy and a multi-bit
 *    entry would over-match. */
typedef enum {
    REPL_ENUM_SLOT_ENUM_ONLY = 0,       /* token only; reject numeric + expr */
    REPL_ENUM_SLOT_ENUM_OR_CONST_VALUE, /* token, else const value reverse-mapped */
    REPL_ENUM_SLOT_ENUM_OR_EXPR,        /* token first, else full expression */
    REPL_ENUM_SLOT_ENUM_BITFIELD        /* one or more tokens OR'd with `|` */
} ReplEnumSlotKind;

/* One positional enum-argument spec: the source-of-truth enum token
 * table (also drives autocomplete), the per-slot diagnostic shown when
 * resolution fails, the slot kind, and an optional all-bits alias for
 * bitfield slots. `enums` is non-null for all four kinds — the parser
 * branches on `kind`, never on whether `enums` is null. */
typedef struct {
    const ReplEnumEntry *enums;
    const char          *usage;
    ReplEnumSlotKind     kind;
    const char          *bitfield_all_alias;
} ReplEnumArgSpec;

#define MAX_FUNC_HINT_PARAMS 10

/* F1 help-overlay grouping for autocomplete entries. The renderer walks
 * the completion table once per group, emitting a section header and
 * one row per entry. NONE means "don't render in the command help"
 * (used by language-level entries like `func0 {`, `x = `, math
 * builtins, and the constants `PI` / `TAU` — those live in the
 * hand-written language sections of the overlay). */
typedef enum {
    REPL_HELP_GROUP_NONE = 0,
    /* The former single REPL_HELP_GROUP_TOP bucket, split into the same
     * contiguous runs banner-labeled in k_func_completions[]. */
    REPL_HELP_GROUP_GEOMETRY,    /* "Vertices, Normals & Color:" */
    REPL_HELP_GROUP_PRIMITIVE,   /* "Primitive Blocks:" */
    REPL_HELP_GROUP_STATE,       /* "Render State:" */
    REPL_HELP_GROUP_RASTER,      /* "Raster Position & Bitmap Text:" */
    REPL_HELP_GROUP_BLEND,       /* "Point Parameters & Blending:" */
    REPL_HELP_GROUP_TRANSFORM,   /* "Matrix Transforms:" */
    REPL_HELP_GROUP_DEPTH_MASK,  /* "Depth & Write-Mask State:" */
    REPL_HELP_GROUP_LIGHTING,    /* "Lighting / Material:" */
    REPL_HELP_GROUP_GLUT_SHAPES, /* "GLUT Solid Shapes:" */
    REPL_HELP_GROUP_GLU_TESS,    /* "GLU Tessellator (concave / complex polygons):" */
    REPL_HELP_GROUP_MATH,        /* "Math Functions:" */
    REPL_HELP_GROUP_COUNT
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
 * UI doesn't need to grow another switch case.
 *
 * Categories are the *visual* taxonomy (one color per category). For
 * *control-flow* predicates — "is this a transform?" / "does this emit a
 * vertex?" / "is this a block head?" — see the inline helpers in
 * src/repl/command.h (repl_cmd_is_transform, repl_cmd_emits_vertex,
 * repl_cmd_is_block_head, repl_cmd_is_block_end). The two axes are
 * intentionally separate: e.g. CMD_FOR_BEGIN / CMD_FUNC_DEF /
 * CMD_IF_BEGIN live in three distinct visual categories
 * (CMD_CAT_LOOP / CMD_CAT_FUNCTION / CMD_CAT_CONDITIONAL) but share a
 * single control-flow predicate (repl_cmd_is_block_head). Don't fold
 * the predicates through the spec table — that'd invert the header
 * layering (command.h is foundational, command_spec.h depends on it). */
typedef enum {
    CMD_CAT_DEFAULT = 0,    /* fallback gray; only hit for out-of-range types */
    CMD_CAT_PRIMITIVE,      /* glBegin / glEnd */
    CMD_CAT_VERTEX,         /* glVertex* / gluVertex */
    CMD_CAT_NORMAL,         /* glNormal3f / gluNormal */
    CMD_CAT_COLOR,          /* glColor* / glClearColor / glColorMaterial / glMaterialfv / gluColor */
    CMD_CAT_TRANSFORM,      /* glTranslate / glScale / glRotate / glPushMatrix / glPopMatrix */
    CMD_CAT_STATE,          /* glEnable / glDisable / glShadeModel / glLineWidth / glPointSize / glBlendFunc / glDepthMask / glEdgeFlag / glLightModeli / glFrontFace / glPointParameterfv */
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
 * a command type needs a trailing semicolon (e.g., float decl, assignment). The
 * category field drives the code-panel renderer's syntax highlighting.
 *
 * valid_in_begin: 1 if the command may appear between glBegin and glEnd. 0 marks
 * commands real GL would reject with GL_INVALID_OPERATION inside a begin block
 * (glPointSize, glRasterPos3f, GLUT solids, nested glBegin, etc.); the parser
 * rejects them at commit time so the executor doesn't have to defensively close
 * the active begin block. */
typedef struct {
    const char *name;          /* Symbolic CmdType name (e.g. "CMD_BEGIN");
                                * used for dumps, debug output, fallback. */
    const char *display_name;  /* User-facing GL name (e.g. "glBegin"); used
                                * in error messages. NULL falls back to `name`.
                                * Set only for cmds whose symbolic name doesn't
                                * read naturally to a user (most GL commands). */
    int needs_semicolon;
    CmdSyntaxCategory category;
    int valid_in_begin;
} ReplCommandTypeSpec;

/* Metadata for GL commands with enumerated arguments. Specifies the command name,
 * type, argument count, up to two enumeration tables (for multi-enum commands
 * like glBlendFunc which takes two separate enum lists), format string for
 * parameter hints, usage descriptions, and indentation type. Used by the parser
 * for validation and by autocomplete for parameter suggestions. */
typedef struct {
    const char *name;
    CmdType     type;
    /* Declared argument count. Positive: parsed by the generalized
     * N-arg enum loop. Negative (e.g. glMaterialfv -2): a custom parser
     * branch handles parsing; abs(num_args) is still the slot count for
     * autocomplete, which reads args[].enums. */
    int         num_args;
    const char *fmt;          /* canonical-text format ("%sName(%s, %s);") */
    int         indent_type;  /* 1 = glBegin-style begin-block indent */
    /* Positional enum-argument specs, one per declared argument. The
     * single source of truth for enum-token resolution + emission (the
     * generalized N-arg parser loop) and for slot-indexed autocomplete. */
    ReplEnumArgSpec args[MAX_ENUM_ARGS];
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
 * a spec describing the command's name and whether it needs a semicolon. Used
 * by the parser and code-panel UI. */
const ReplCommandTypeSpec *repl_command_type_spec(CmdType type);

/* Query the human-readable name for a command type (e.g., "glVertex3f",
 * "for", "if"). Used by error messages, autocomplete hints, and UI display. */
const char *repl_cmd_type_name(CmdType type);

/* Query the user-facing display name (e.g., "glBegin", "glPointSize"). Falls
 * back to repl_cmd_type_name when the spec doesn't carry a distinct display
 * name. Used in parser error messages where the symbolic CMD_* identifier
 * would read poorly. */
const char *repl_cmd_type_display_name(CmdType type);

/* Thin alias used by tests and the demo for the unprefixed identifier
 * (cmd_type_name(CmdType)). Forwards to repl_cmd_type_name. */
const char *cmd_type_name(CmdType type);

/* Query whether a command type requires a trailing semicolon. Used by the
 * formatter and commit validation. */
int repl_cmd_type_needs_semicolon(CmdType type);

/* Query the syntax-highlighting category for a command type. Used by the
 * code-panel renderer to pick the row color. Returns CMD_CAT_DEFAULT
 * for out-of-range types. */
CmdSyntaxCategory repl_cmd_type_category(CmdType type);

/* 1 if the command may appear between glBegin and glEnd, 0 otherwise.
 * The parser uses this to reject illegal-in-begin commands at commit time
 * (e.g. glPointSize, glRasterPos3f, GLUT solids, nested glBegin). Out-of-
 * range / missing types default to 1 so unknown commands aren't blocked. */
int repl_cmd_type_valid_in_begin(CmdType type);

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

/* Resolve the parameter signature (the completion table's display_text, e.g.
 * "glVertex3f(x, y, z)") for a bare command name (e.g. "glVertex3f"). Matches
 * either an exact display_text or a display_text of the form `<name>(...)`, so
 * "glClear" resolves to "glClear(mask)" but not "glClearColor(...)". Returns a
 * static string with process lifetime, or NULL when no entry names the command.
 * Used by the right-click command-help popup to show parameters in its title. */
const char *repl_func_signature_for_name(const char *name);

/* Query enumeration tables for GL constants used in command arguments. Provides
 * suggestions for glColorMaterial face parameter, glMaterialfv parameter names,
 * and glPointParameterfv pname values. Used by autocomplete to populate
 * parameter suggestions. */
const ReplEnumEntry *repl_face_type_entries(void);
const ReplEnumEntry *repl_depth_func_entries(void);
const ReplEnumEntry *repl_material_param_entries(void);
const ReplEnumEntry *repl_point_param_pname_entries(void);
const ReplEnumEntry *repl_clip_plane_entries(void);
const ReplEnumEntry *repl_fog_f_pname_entries(void);
const ReplEnumEntry *repl_fog_color_pname_entries(void);
const ReplEnumEntry *repl_fog_mode_entries(void);

/* The 10 GL_*_BIT attribute groups glPushAttrib()/glPopAttrib() support, in
 * canonical order (ascending GL value). Shared source of truth for the
 * ENUM_BITFIELD parse/emit, the mapping module (attrib_bits.c), and the
 * editor's per-bit cursor highlighting so they can't drift on which bit is
 * which. NULL-terminated. */
const ReplEnumEntry *repl_attrib_bit_entries(void);

/* Convert a GL_TRIANGLES, GL_QUADS, etc. constant to its string name (e.g.,
 * GL_TRIANGLES → "GL_TRIANGLES"). Used for display and diagnostic output. */
const char *repl_begin_mode_name(GLenum mode);

#endif
