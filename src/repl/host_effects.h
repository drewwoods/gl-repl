/*
 * src/repl/host_effects.h - Host-installed side-effect bridge.
 *
 * REPL pipeline code calls through this bridge for status, cursor, input,
 * completion, and tutorial effects owned above src/repl. Uninstalled callbacks
 * are no-ops, which keeps pure REPL tests and the standalone demo simple.
 */
#ifndef REPL_HOST_EFFECTS_H
#define REPL_HOST_EFFECTS_H

/* Shared status/document helpers surfaced outside src/repl. */
void repl_set_status(const char *msg);
/* Error variant: dispatches through `status_error` so the UI renders
 * the status bar in red. Falls back to `status` when the host bridge
 * does not provide an error hook (so demo / test harnesses still see
 * the text). */
void repl_set_status_error(const char *msg);

/* Controller-installed side-effect hooks for pipeline-only code paths.
 *
 * Loader, scene-switch, snippet-import, and replay code in src/repl call
 * through this table when they need a host action such as clearing editor
 * input, scrolling the code panel, tearing down tutorial state before source
 * replacement, or resetting example presentation state.
 * Most pure REPL tests leave the table unset; the standalone demo installs
 * only its edit-line hooks. Any unset callback makes the matching dispatcher
 * a no-op.
 *
 * This mirrors the export cfg/camera bridges: the REPL asks for an effect by
 * purpose, without naming editor/UI implementation details. Phases 3 + 6 of
 * feature/source-document-port.md consolidated the older per-effect installers
 * into this single struct.
 *
 * Insert-mode QUERY (for ReplCompileContext.insert_mode) is the
 * caller's responsibility - repl_compile_context_from_live() defaults
 * to 0 (overwrite, the safe load-path value); editor-side callers
 * overwrite the field with editor_insert_mode() before compiling. */
typedef struct {
    /* Surface a diagnostic/status message. The controller routes this
     * to UiState; pipeline TUs call repl_set_status() unchanged. */
    void (*status)(const char *msg);
    /* Same as `status` but tags the message as an error so the
     * controller can render it in the red-palette status bar. NULL is
     * permitted - `repl_set_status_error()` then falls back to the
     * info path so test harnesses without a host bridge still see the
     * message text. */
    void (*status_error)(const char *msg);
    /* Refresh scene-bound presentation defaults (wireframe, grid,
     * axes, vertex overlays, backdrop) before each example load. The
     * `presentation` slice lives on glr_state, out of the REPL
     * pipeline's reach.
     *
     * `tag_mask` is the bitmask of REPL_EXAMPLE_TAG_* bits for the
     * example being loaded (0 for non-example loads such as the test
     * line-feed entrypoint). The controller applies the global
     * defaults first, then layers tag-specific `@cfg` overrides on
     * top before the example's own leading `@cfg` metadata runs.
     * Order of precedence: example `@cfg` > tag default > global
     * default. */
    void (*example_presentation_reset)(unsigned int tag_mask);
    /* Same reset for a tutorial start, plus the two things an example
     * load deliberately inherits: the camera pose (eased back to the
     * built-in default) and the grid extent (narrowed to
     * CFG_DEFAULT_TUTORIAL_GRID_EXTENT_IDX). A tutorial is a fresh
     * transient scene with unit-scale geometry, so it starts from the
     * app's out-of-the-box view rather than wherever the previous scene
     * left the camera. The tutorial's own leading `@cfg` still runs
     * after this and wins. NULL = no-op (pure REPL tests, demo). */
    void (*tutorial_presentation_reset)(void);
    /* Clear the editor input buffer. */
    void (*input_reset)(void);
    /* Force the editor out of insert mode. */
    void (*insert_mode_off)(void);
    /* Scroll the code panel so source line `target` is visible. */
    void (*scroll_to_line)(int target);
    /* Tear down any active tutorial before wholesale source replacement.
     * The tutorial runner is a peer subsystem; routing this through the
     * host bridge keeps src/repl from linking tutorial.c just to request
     * its idempotent cleanup. NULL = no tutorial host, so no-op. */
    void (*tutorial_teardown)(void);
    /* Read/write the edit-line cursor. Cursor storage moved
     * to EditorState; REPL code that previously called
     * repl_state_edit_line() / _set() goes through these hooks so
     * the beta invariant (REPL -> editor symbol references forbidden)
     * stays intact. Higher-level pipeline entry points (scenes.c)
     * use this; the parse / compile / flatten / load layers take
     * cursor as an explicit parameter instead (see phase 3.6.x;
     * implemented in phase 4 of docs/plans/done/edit-line-ownership.md).
     *
     * Default behavior when the hook is NULL: edit_line_get
     * returns 0; edit_line_set is a no-op. The demo / pure REPL
     * tests run unchanged. */
    int  (*edit_line_get)(void);
    void (*edit_line_set)(int line);
    /* Decoupled editor/completion mutations and reads for subsystems. */
    void (*host_cursor_park)(int line, int insert_mode);
    void (*completion_clear)(void);
    void (*completion_update)(void);
    const char *(*host_input_get)(void);
    void (*set_time_playing)(int playing);
} ReplHostEffects;

void repl_install_host_effects(const ReplHostEffects *effects);

/* Pipeline-side dispatchers - invoked by the loader / scene-switch /
 * snippet-import / replay paths. Each is a no-op when the bridge is
 * unset or the matching callback is NULL. */
void        repl_dispatch_example_presentation_reset(unsigned int tag_mask);
void        repl_dispatch_tutorial_presentation_reset(void);
void        repl_dispatch_input_reset(void);
void        repl_dispatch_insert_mode_off(void);
void        repl_dispatch_scroll_to_line(int target);
void        repl_dispatch_tutorial_teardown(void);
int         repl_dispatch_edit_line_get(void);
void        repl_dispatch_edit_line_set(int line);
void        repl_dispatch_host_cursor_park(int line, int insert_mode);
void        repl_dispatch_completion_clear(void);
void        repl_dispatch_completion_update(void);
const char *repl_dispatch_host_input_get(void);
void        repl_dispatch_set_time_playing(int playing);

#endif /* REPL_HOST_EFFECTS_H */
