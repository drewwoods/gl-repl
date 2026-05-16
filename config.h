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

/* How long a status-bar message stays visible, in frames (~60 fps, so
 * 360 ≈ 6 s). Interim value bumped from 240 so messages linger longer
 * until the recent-messages viewer lands
 * (plans/in-review/status-message-history.md). */
#define REPL_STATUS_MESSAGE_TTL 360

/* Grid/axes show/hide transitions (see
 * plans/done/grid-axes-transitions.md). The two overlays are fully
 * independent: each has its own in/out durations (seconds; the
 * transition machine ticks on dt seconds) and its own visual style.
 *
 * Style selects how the controller-owned opacity becomes a visual,
 * resolved at compile time in src/scene/grid.c / src/scene/axes.c:
 *   GRID_AXES_XN_FADE  plain alpha fade (default; original behavior)
 *   GRID_AXES_XN_FOG   recede into clear-color fog on the way out /
 *                      emerge from it on the way in, with an alpha
 *                      knee near opacity 0 so even near-origin
 *                      geometry (axis lines, grid origin) fully
 *                      vanishes. Fog is distance-based, so this is a
 *                      strong look for the grid and a subtle haze for
 *                      the near-origin axes. */
#define GRID_AXES_XN_FADE 0
#define GRID_AXES_XN_FOG  1

/* Each selector is overridable from the build (e.g.
 * `make sample CPPFLAGS=-DGRID_XN_STYLE=GRID_AXES_XN_FOG`) without
 * editing this file. */
#ifndef GRID_FADE_IN_SECS
#define GRID_FADE_IN_SECS  0.55f
#endif
#ifndef GRID_FADE_OUT_SECS
#define GRID_FADE_OUT_SECS 0.50f
#endif
#ifndef GRID_XN_STYLE
#define GRID_XN_STYLE      GRID_AXES_XN_FOG
#endif

#ifndef AXES_FADE_IN_SECS
#define AXES_FADE_IN_SECS  0.15f
#endif
#ifndef AXES_FADE_OUT_SECS
#define AXES_FADE_OUT_SECS 0.10f
#endif
#ifndef AXES_XN_STYLE
#define AXES_XN_STYLE      GRID_AXES_XN_FADE
#endif

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
