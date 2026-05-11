/*
 * config.h - Project-wide compile-time configuration constants.
 *
 * Home for shared, namespace-agnostic limits and sizes used across
 * REPL / editor / UI / scene boundaries — anywhere a single number
 * needs to be visible to multiple subsystems without dragging in
 * domain-specific headers.
 *
 * Stays dependency-free on purpose: REPL-pipeline files (parse,
 * compile, apply, flatten, export) include this just for
 * REPL_STATUS_TEXT_MAX and shouldn't transitively pull in scene or
 * UI types. Defaults that REFERENCE scene/UI enums live in
 * `src/app/glr_defaults.h`; only callers of those defaults pay the include.
 *
 * NOTE: This file is for *compile-time* configuration. User-toggleable
 * runtime settings (wireframe / grid theme / etc.) live on
 * `repl_config.h` and the `g_cfg_items[]` descriptor table in
 * `src/app/glr_actions.c` — different concept, do not conflate.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* Font metrics for menu rendering. These are fixed-width bitmap fonts, so the
 * width and height are compile-time constants. Used by menu layout and rendering
 * code to compute positions and sizes. */
#define FONT_MONO       GLUT_BITMAP_9_BY_15
#define FONT_SMALL      GLUT_BITMAP_8_BY_13
#define FONT_W          9
#define FONT_H          15
#define FONT_SMALL_W    8
#define FONT_SMALL_H    13

/* Default code-panel fraction (panel width / total viewport width) on
 * startup and scene load. User-toggleable at runtime via the "Panel
 * Layout" config row, which cycles through a few presets that include
 * this default as the "Left" option. */
#define CFG_DEFAULT_PANEL_FRAC 0.45f

/* Max brightness (V in HSV) allowed for glClearColor channels.
 * Since max(r,g,b) == V, capping V caps all channels. */
#define CP_CLEAR_MAX_V 0.1f

/* Status / diagnostic text buffer size. Compile entries write into
 * `err[REPL_STATUS_TEXT_MAX]`; ReplCompiledChange.commit_message and
 * EditorCommitResult.diagnostic carry the same width; the visible
 * status bar in `ReplStatusState` mirrors the size end-to-end. */
#define REPL_STATUS_TEXT_MAX 256

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

#endif /* CONFIG_H */