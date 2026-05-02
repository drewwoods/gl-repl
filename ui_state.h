#ifndef UI_STATE_H
#define UI_STATE_H

/* UiState owns transient chrome, viewport, pointer, status text,
 * visibility flags, cursor blink, and the 3D-viewport session per the
 * three-layer ownership contract in MODULES.md and
 * feature/editor-owns-text-completion.md.
 *
 * Phase 1 scaffold (commit 3): the struct is intentionally a placeholder
 * until commit 7 lands the first real slice (status / help / panel
 * visibility / viewport / pointer). Camera pose moves here too once
 * Phase 1 reaches its tail; mouse-driven nav routes through
 * UI_ACTION_CAMERA_* in Phase 4.
 *
 * capture/restore/reset symmetry mirrors repl_state_capture / restore /
 * reset_all and editor_state_capture / restore / reset.
 * repl_state_reset_all() drains all three structs so every test reset
 * clears the full app state.
 */

typedef struct {
    /* Placeholder field. Removed in commit 7 when the first UI slice
     * (status / help / panel visibility / viewport / pointer) lands.
     * C disallows zero-sized aggregates in standard mode, and using a
     * placeholder keeps the layout stable while the struct is empty. */
    int _phase1_scaffold_placeholder;
} UiState;

void ui_state_capture(UiState *snapshot);
void ui_state_restore(const UiState *snapshot);
void ui_state_reset(void);

#endif /* UI_STATE_H */
