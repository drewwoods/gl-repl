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

/* Controller-installed side-effect hooks for REPL pipeline code and the
 * tutorial/app subsystems that operate on REPL-owned documents.
 *
 * Loader, scene-switch, snippet-import, and replay code in src/repl use the
 * first group when they need a host action such as clearing editor input,
 * scrolling the code panel, tearing down tutorial state before source
 * replacement, or resetting example presentation state. Tutorial/app peers
 * use the second group for editor/completion mutations and clock ownership.
 * Most pure REPL tests leave the table unset; the standalone demo installs
 * only its edit-line hooks. Any unset callback makes the matching dispatcher
 * a no-op.
 *
 * This mirrors the export cfg/camera bridges: the REPL asks for an effect by
 * purpose, without naming editor/UI implementation details. The callbacks
 * are grouped here so the pipeline has one host boundary to install.
 *
 * Insert-mode QUERY (for ReplCompileContext.insert_mode) is the
 * caller's responsibility - repl_compile_context_from_live() defaults
 * to 0 (overwrite, the safe load-path value); editor-side callers
 * overwrite the field with editor_insert_mode() before compiling. */
typedef struct {
    /* REPL pipeline callers. */
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
     * `example_idx` is the catalog index of the example being loaded.
     * The controller resolves its tags itself (repl_example_has_tag),
     * applies the global defaults first, then layers tag-specific
     * `@cfg` overrides on top before the example's own leading `@cfg`
     * metadata runs. Order of precedence: example `@cfg` > tag default
     * > global default.
     *
     * **-1, not 0, is the "no example" sentinel** - passed by callers
     * that hold only scene text (repl_load_example_lines: tests, bench,
     * repl_live_demo) and by the tutorial reset. 0 is a valid catalog
     * index, so passing it means "apply example 0's tag defaults". */
    void (*example_presentation_reset)(int example_idx);
    /* Clear the editor input buffer. */
    void (*input_reset)(void);
    /* Force the editor out of insert mode. */
    void (*insert_mode_off)(void);
    /* Scroll the code panel so source line `target` is visible. */
    void (*scroll_to_line)(int target);
    /* Scroll so the generated `display() {` frame sits at the top of the
     * code panel. Symbolic rather than a row number: the frame is spliced
     * after the file-scope prologue, and only the host can measure how
     * many wrapped panel rows that prologue occupies. */
    void (*scroll_to_display_open)(void);
    /* Tear down any active tutorial before wholesale source replacement.
     * The tutorial runner is a peer subsystem; routing this through the
     * host bridge keeps src/repl from linking tutorial.c just to request
     * its idempotent cleanup. NULL = no tutorial host, so no-op. */
    void (*tutorial_teardown)(void);
    /* Read/write the edit-line cursor. Cursor storage moved
     * to EditorState; REPL code that previously called
     * repl_state_edit_line() / _set() goes through these hooks so
     * stays intact. Higher-level pipeline entry points (scenes.c) use this;
     * the parse, compile, flatten, and load layers take the cursor as an
     * explicit parameter instead.
     *
     * Default behavior when the hook is NULL: edit_line_get
     * returns 0; edit_line_set is a no-op. The demo / pure REPL
     * tests run unchanged. */
    int  (*edit_line_get)(void);
    void (*edit_line_set)(int line);

    /* Tutorial/app peer callers. */
    /* Reset for a tutorial start, plus the two things an example load
     * deliberately inherits: the camera pose (eased back to the built-in
     * default) and the grid extent (narrowed to
     * CFG_DEFAULT_TUTORIAL_GRID_EXTENT_IDX). A tutorial is a fresh transient
     * scene with unit-scale geometry, so it starts from the app's out-of-box
     * view rather than wherever the previous scene left the camera. Its own
     * leading `@cfg` still runs after this and wins. NULL = no-op. */
    void (*tutorial_presentation_reset)(void);
    void (*host_cursor_park)(int line, int insert_mode);
    /* Put the cursor ON an existing committed row: park it there AND load
     * that row's text into the input buffer, the way arrowing onto the line
     * does, then follow-scroll it into view. host_cursor_park alone is not
     * this - it leaves the input buffer empty, and the code panel renders
     * that empty buffer in place of the row (insert mode adds a blank line
     * above it; overwrite mode blanks the row itself). The tutorial runner's
     * LOOK step needs the row visible with the cursor on it, so that
     * cursor-scoped overlays describe that row. Callers must
     * repl_dispatch_input_reset() when they leave, or the loaded text
     * follows the cursor to wherever it parks next. */
    void (*host_focus_line)(int line);
    void (*completion_clear)(void);
    void (*completion_update)(void);
    const char *(*host_input_get)(void);
    void (*set_time_playing)(int playing);
} ReplHostEffects;

void repl_install_host_effects(const ReplHostEffects *effects);

/* Bridge dispatchers for pipeline and tutorial/app-peer callers. Each is a
 * no-op when the bridge is unset or the matching callback is NULL. */
void        repl_dispatch_example_presentation_reset(int example_idx);
void        repl_dispatch_tutorial_presentation_reset(void);
void        repl_dispatch_input_reset(void);
void        repl_dispatch_insert_mode_off(void);
void        repl_dispatch_scroll_to_line(int target);
void        repl_dispatch_scroll_to_display_open(void);
void        repl_dispatch_tutorial_teardown(void);
int         repl_dispatch_edit_line_get(void);
void        repl_dispatch_edit_line_set(int line);
void        repl_dispatch_host_cursor_park(int line, int insert_mode);
void        repl_dispatch_host_focus_line(int line);
void        repl_dispatch_completion_clear(void);
void        repl_dispatch_completion_update(void);
const char *repl_dispatch_host_input_get(void);
void        repl_dispatch_set_time_playing(int playing);

#endif /* REPL_HOST_EFFECTS_H */
