/*
 * config.h - Project-wide compile-time configuration constants.
 *
 * Home for shared, dependency-free limits and the small set of app-wide
 * compile-time defaults that intentionally cross REPL / editor / UI /
 * scene boundaries without dragging in domain-specific headers.
 *
 * Stays dependency-free on purpose: REPL-pipeline files (parse,
 * compile, apply, flatten, export) include this just for
 * REPL_STATUS_TEXT_MAX and shouldn't transitively pull in scene or
 * UI types. Defaults that REFERENCE scene/UI enums live in
 * `src/app/glr_defaults.h`; only callers of those defaults pay the include.
 *
 * NOTE: This file is for *compile-time* configuration. User-toggleable
 * runtime settings (wireframe / grid theme / etc.) live on
 * `src/app/glr_config.h` and the `g_cfg_items[]` descriptor table in
 * `src/app/glr_actions.c` — different concept, do not conflate.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* Keyboard shortcut bindings. Pulled in here so the GLR_*_KEY / _MODS
 * macros reach every TU (config.h is force-included project-wide) without
 * each consumer adding an include. Safe despite this file's
 * dependency-free contract: keymap.h only references GLUT_KEY_* / Ctrl
 * codes inside macro BODIES (same lazy-expansion pattern as the FONT_*
 * macros below), so nothing resolves until a dispatch site that already
 * has the GLUT headers expands it. */
#include "keymap.h"

/* ---- Shared UI primitives --------------------------------------------- */

/* Shared GLUT bitmap fonts. The fixed-width fonts have compile-time metrics
 * below for menu/layout code; proportional fonts are render-only. */
#define FONT_TINY       GLUT_BITMAP_HELVETICA_10
#define FONT_MONO       GLUT_BITMAP_9_BY_15
#define FONT_SMALL      GLUT_BITMAP_8_BY_13
#define FONT_W          9
#define FONT_H          15
#define FONT_SMALL_W    8
#define FONT_SMALL_H    13

/* ---- App-wide defaults retained in config.h --------------------------- */

/* Default code-panel fraction (panel width / total viewport width) on
 * startup and scene load. User-toggleable at runtime via the "Panel
 * Layout" config row, which cycles through a few presets that include
 * this default as the "Left" option. */
#define CFG_DEFAULT_PANEL_FRAC 0.45f

/* Code-panel divider drag clamps the panel fraction to this range so
 * neither the panel nor the scene can be collapsed to nothing. */
#define CFG_PANEL_FRAC_MIN 0.1f
#define CFG_PANEL_FRAC_MAX 0.9f

/* ---- Text and diagnostics --------------------------------------------- */

/* Status / diagnostic text buffer size. Compile entries write into
 * `err[REPL_STATUS_TEXT_MAX]`; ReplCompiledChange.commit_message and
 * EditorCommitResult.diagnostic carry the same width; the visible
 * status bar in `ReplStatusState` mirrors the size end-to-end. */
#define REPL_STATUS_TEXT_MAX 256

/* Intermediate diagnostic text buffer width — used by validation
 * helpers (verr), arg splitters (split_err), and parse/flatten error
 * messages that get forwarded into the final REPL_STATUS_TEXT_MAX
 * buffer with extra context wrapped around them. Half the size of the
 * visible status leaves room for the wrapping prefix at the callsite.
 * Also the on-the-wire width of ReplFlattenResult.status. */
#define REPL_DIAG_TEXT_MAX 128

/* ---- Build-time UI selection ------------------------------------------ */

/* Active UI color scheme, resolved at compile time. A bare integer
 * index into the UiTheme table in src/ui/theme.h (kept type-free here
 * so config.h stays clear of UI types per the dependency note above):
 *   0 green (default)  1 warm  2 cyan  3 amber  4 violet  5 mono
 * Overridable from the build, e.g.
 * `make sample CPPFLAGS=-DUI_THEME_DEFAULT=1`. theme.h STATIC_ASSERTs
 * the value is in range against the UiTheme enum. */
#ifndef UI_THEME_DEFAULT
#define UI_THEME_DEFAULT 0
#endif

/* ---- Shared storage capacities ---------------------------------------- */

/* Storage capacity of the source command document and the matching
 * editor buffer. Surfaces here (not in src/repl/command.h) so neutral
 * boundary headers — source_document.h in particular — can size their
 * structs without including REPL grammar types. */
#ifndef MAX_COMMANDS
#define MAX_COMMANDS 4096
#endif

/* Maximum characters in a single canonical source line, including the
 * trailing NUL. Same neutrality argument as MAX_COMMANDS. */
#ifndef MAX_LINE_LEN
#define MAX_LINE_LEN 256
#endif

/* Maximum source-command rows touched by a single compiled change —
 * insert_many block batches, comment toggles, etc. The neutral
 * SourceTextChange in source_document.h sizes its text[] array by this
 * constant so the translator from ReplCompiledChange.text[] copies
 * 1:1 without a bound mismatch. */
#ifndef MAX_COMMIT_CMDS
#define MAX_COMMIT_CMDS 16
#endif

/* Maximum length of a predefined variable name (including NUL).
 * Sizes ExprVar.name, undo snapshot name arrays, scene slot snapshots,
 * compiled-change predef ops, variable-panel drag state, and every
 * local buffer that holds a parsed identifier destined for the predef
 * table. Lives here (not in eval.h) so lightweight subsystem headers
 * can reference it without pulling in REPL eval types. */
#ifndef REPL_PREDEF_NAME_MAX
#define REPL_PREDEF_NAME_MAX 16
#endif

/*
 * MAX_PREDEF_VARS - global user-declared REPL variables
 * MAX_EXPR_VARS   - local lexical scope at parse/eval time
 *
 * They are independent limits: MAX_PREDEF_VARS caps persistent `float x;`
 * slots (one is reserved for built-in `t`), while MAX_EXPR_VARS caps a
 * single expression's visible identifier snapshot, including locals and
 * visible predef vars. Keep MAX_EXPR_VARS >= MAX_PREDEF_VARS so parsing
 * does not silently truncate the global variable table out of scope.
 */
#ifndef MAX_EXPR_VARS
#define MAX_EXPR_VARS 32
#endif
#ifndef MAX_PREDEF_VARS
#define MAX_PREDEF_VARS 24
#endif
#if MAX_PREDEF_VARS > MAX_EXPR_VARS
#error "MAX_PREDEF_VARS must be <= MAX_EXPR_VARS."
#endif

/* Capacity (including NUL) of a stack-allocated indent string buffer.
 * Sized to comfortably hold any indent produced by the source-scope and
 * format helpers (2 + 2*tess_depth + 2*begin_depth + per-block-nesting)
 * for the deepest realistic REPL nesting. Lives here (not in
 * src/repl/source_scope.h) so editor / UI code can size indent buffers
 * without taking a REPL include just for the constant. */
#ifndef REPL_INDENT_TEXT_MAX
#define REPL_INDENT_TEXT_MAX 32
#endif

/* ---- App-wide file/render/timing defaults ----------------------------- */

/* Default filename suggested by the "Load Scene…" file prompt
 * (File menu → Load Scene, src/app/glr_actions.c). Also the seed value
 * the prompt's inline-file-prompt module documents in its header. */
#ifndef DEFAULT_SCENE_FILE
#define DEFAULT_SCENE_FILE "my_scene.c"
#endif

/* Recovery filename. On Ctrl+Q (or SIGINT), and before Load Workspace
 * discards the current in-memory scene, the controller writes a recovery
 * copy of the live scene here so an unintended exit / workspace load never
 * silently clobbers the user's real scene. Reload with
 * `./sample recovery.c`. See src/app/glr_ctrl.c. */
#ifndef QUIT_RECOVERY_FILE
#define QUIT_RECOVERY_FILE "recovery.c"
#endif

/* Polygon-offset depth-bias constants for the outline pass. Shared by
 * the live app controller (src/app/glr_ctrl.c) and the export path
 * (src/repl/export.c) so both rendering modes use the same depth bias
 * when drawing wire outlines over the filled pass. Dependency-free
 * compile-time constants — folded here from the former
 * outline_offset.h, matching this header's cross-layer-constant
 * contract. */
#ifndef REPL_OUTLINE_POLYGON_OFFSET_FACTOR
#define REPL_OUTLINE_POLYGON_OFFSET_FACTOR (-0.01f)
#endif
#ifndef REPL_OUTLINE_POLYGON_OFFSET_UNITS
#define REPL_OUTLINE_POLYGON_OFFSET_UNITS  (-100.0f)
#endif

/* Shared frame clock delta-time (seconds, representing ~60 fps).
 * Shared by the live app controller (src/app/glr_ctrl.c) and the replay peer
 * (src/subsystems/replay/replay.c) for visual animation ticks. */
#ifndef GLR_FRAME_DT_SECS
#define GLR_FRAME_DT_SECS 0.016f
#endif

/* Standard status message buffer size for the replay visualizer. */
#ifndef REPLAY_STATUS_MSG_LEN
#define REPLAY_STATUS_MSG_LEN 64
#endif

/* ---- REPL parser limits ------------------------------------------------ */

/* Maximum length of a goto label (including NUL). */
#ifndef REPL_GOTO_LABEL_MAX
#define REPL_GOTO_LABEL_MAX 64
#endif

/* Maximum nesting depth of begin/end blocks. */
#ifndef REPL_MAX_BLOCK_NEST_DEPTH
#define REPL_MAX_BLOCK_NEST_DEPTH 64
#endif

/* Maximum number of enum arguments. */
#ifndef REPL_ENUM_ARG_MAX
#define REPL_ENUM_ARG_MAX 64
#endif

#endif /* CONFIG_H */
