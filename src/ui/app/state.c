#include "ui/app/state.h"
#include "ui/support/cpuprof.h"
#include "ui/support/memprof.h"
#include "ui/app/layout.h"  /* CFG_DEFAULT_PANEL_FRAC */

#include <stddef.h>
#include <string.h>

/* Defaults preserve the behavior the pre-migration
 * src/repl/state_defaults.inc enshrined: pointer button starts at -1
 * (no button held); cursor starts visible so the renderer's blink
 * phase begins ON; camera faces the same orbit/distance the example
 * loader expects on a fresh session; other slices zeroed.
 *
 * variable_panel visibility lives on the variable_panel peer; callers
 * use variable_panel_view / variable_panel_state_mut directly. */
#define UI_STATE_INITIAL                                              \
    {                                                                 \
        .status = { .text = "", .ttl = 0, .kind = UI_STATUS_INFO },   \
        .help = { .visible = 0 },                                     \
        .gl_state_inspector = { .visible = 0, .source_line_idx = -1, \
                                .anchor_px = -1, .anchor_py = -1 },  \
        .command_description = { .visible = 0, .source_line_idx = -1, \
                                 .anchor_px = -1, .anchor_py = -1 }, \
        .profile_panel = { .mode = PROFILE_PANEL_OFF },               \
        .memory_panel  = { .mode = MEMORY_PANEL_OFF  },               \
        .viewport = { .window_w = 0, .window_h = 0 },                 \
        .pointer = { .mouse_x = 0, .mouse_y = 0, .mouse_button = -1 },\
        .code_panel = {                                               \
            .panel_frac     = CFG_DEFAULT_PANEL_FRAC,                 \
            .resizing_panel = 0,                                      \
            .scrollbar_drag = 0,                                      \
        },                                                            \
    }

static UiState g_ui_state = UI_STATE_INITIAL;
static const UiState g_ui_state_defaults = UI_STATE_INITIAL;

void ui_state_capture(UiState *snapshot) {
    if (!snapshot)
        return;
    *snapshot = g_ui_state;
}

void ui_state_restore(const UiState *snapshot) {
    if (!snapshot)
        return;
    g_ui_state = *snapshot;
}

void ui_state_reset(void) {
    g_ui_state = g_ui_state_defaults;
    /* Keep the fresh-session tree default in step with the profiler catalog:
     * the shared collapse-all transition derives its mask from the branches
     * that currently have children. User toggles remain untouched until the
     * next full UI reset. */
    g_ui_state.profile_panel.collapsed_sections =
        ui_profile_panel_toggle_mask(
            prof_section_set_empty(), UI_PROFILE_PANEL_TOGGLE_ALL);
}

UiStatusState ui_state_status(void) {
    return g_ui_state.status;
}

UiStatusState *ui_state_status_mut(void) {
    return &g_ui_state.status;
}

/* Push a message into the recent-message ring. Consecutive identical
 * (text + kind) messages collapse into the newest entry with an
 * incremented dup_count instead of flooding the ring. */
static void ui_state_status_history_push(const char *message, int kind) {
    UiStatusHistory *h = &g_ui_state.status_history;
    UiStatusEntry *e;

    if (h->count > 0) {
        int newest = ui_status_history_index(h, h->count - 1);
        UiStatusEntry *last = &h->entries[newest];
        if (last->kind == kind && strcmp(last->text, message) == 0) {
            if (last->dup_count < (unsigned int)-1)
                last->dup_count++;
            last->seq = h->next_seq++;
            return;
        }
    }

    e = &h->entries[h->head];
    strncpy(e->text, message, sizeof(e->text) - 1);
    e->text[sizeof(e->text) - 1] = '\0';
    e->kind = kind;
    e->dup_count = 1;
    e->seq = h->next_seq++;

    h->head = (h->head + 1) % UI_STATUS_HISTORY_CAP;
    if (h->count < UI_STATUS_HISTORY_CAP)
        h->count++;
}

static void ui_state_status_set_kind(const char *message, int kind) {
    int is_refresh;
    if (!message || !message[0])
        return;

    /* A re-emit of the message already on screen (same text + kind, still
     * live) is a refresh, not a new event: keep its animation age and its
     * single history entry, just renew the ttl. This keeps per-frame
     * re-emitters (e.g. tutorial COMMAND-step hints) from pinning the
     * telescope animation collapsed or flooding the history. */
    is_refresh = g_ui_state.status.ttl > 0 &&
                 g_ui_state.status.kind == kind &&
                 strcmp(g_ui_state.status.text, message) == 0;

    strncpy(g_ui_state.status.text, message,
            sizeof(g_ui_state.status.text) - 1);
    g_ui_state.status.text[sizeof(g_ui_state.status.text) - 1] = '\0';
    g_ui_state.status.ttl = UI_STATUS_MESSAGE_TTL;
    g_ui_state.status.kind = kind;

    if (!is_refresh) {
        g_ui_state.status.age = 0;
        ui_state_status_history_push(g_ui_state.status.text, kind);
    }
}

void ui_state_status_set(const char *message) {
    ui_state_status_set_kind(message, UI_STATUS_INFO);
}

void ui_state_status_set_error(const char *message) {
    ui_state_status_set_kind(message, UI_STATUS_ERROR);
}

void ui_state_status_set_music(const char *message) {
    ui_state_status_set_kind(message, UI_STATUS_MUSIC);
}

UiStatusHistory ui_state_status_history(void) {
    return g_ui_state.status_history;
}

void ui_state_status_history_set_open(int open) {
    g_ui_state.status_history.open = open ? 1 : 0;
}

void ui_state_status_history_toggle(void) {
    g_ui_state.status_history.open = !g_ui_state.status_history.open;
}


UiHelpState ui_state_help(void) {
    return g_ui_state.help;
}

UiHelpState *ui_state_help_mut(void) {
    return &g_ui_state.help;
}

UiGlStateInspectorState ui_state_gl_state_inspector(void) {
    return g_ui_state.gl_state_inspector;
}

void ui_state_gl_state_inspector_open(int source_line_idx,
                                      int anchor_px, int anchor_py) {
    g_ui_state.gl_state_inspector.visible = 1;
    g_ui_state.gl_state_inspector.source_line_idx = source_line_idx;
    g_ui_state.gl_state_inspector.anchor_px = anchor_px;
    g_ui_state.gl_state_inspector.anchor_py = anchor_py;
    g_ui_state.gl_state_inspector.scroll_rows = 0;
    g_ui_state.gl_state_inspector.details_expanded = 0;
    g_ui_state.gl_state_inspector.setup_expanded = 0;
}

void ui_state_gl_state_inspector_close(void) {
    g_ui_state.gl_state_inspector.visible = 0;
    g_ui_state.gl_state_inspector.source_line_idx = -1;
    g_ui_state.gl_state_inspector.anchor_px = -1;
    g_ui_state.gl_state_inspector.anchor_py = -1;
    g_ui_state.gl_state_inspector.scroll_rows = 0;
    g_ui_state.gl_state_inspector.details_expanded = 0;
    g_ui_state.gl_state_inspector.setup_expanded = 0;
}

void ui_state_gl_state_inspector_set_scroll(int scroll_rows) {
    g_ui_state.gl_state_inspector.scroll_rows =
        scroll_rows < 0 ? 0 : scroll_rows;
}

void ui_state_gl_state_inspector_toggle_setup(void) {
    g_ui_state.gl_state_inspector.setup_expanded =
        !g_ui_state.gl_state_inspector.setup_expanded;
    /* Row indices shift under the fold, so an old offset would land
     * arbitrarily; restart the window like a fresh open. */
    g_ui_state.gl_state_inspector.scroll_rows = 0;
}

void ui_state_gl_state_inspector_toggle_details(void) {
    g_ui_state.gl_state_inspector.details_expanded =
        !g_ui_state.gl_state_inspector.details_expanded;
}

UiCommandDescriptionState ui_state_command_description(void) {
    return g_ui_state.command_description;
}

void ui_state_command_description_open(int source_line_idx,
                                       int anchor_px, int anchor_py) {
    g_ui_state.command_description.visible = 1;
    g_ui_state.command_description.source_line_idx = source_line_idx;
    g_ui_state.command_description.anchor_px = anchor_px;
    g_ui_state.command_description.anchor_py = anchor_py;
}

void ui_state_command_description_close(void) {
    g_ui_state.command_description.visible = 0;
    g_ui_state.command_description.source_line_idx = -1;
    g_ui_state.command_description.anchor_px = -1;
    g_ui_state.command_description.anchor_py = -1;
}


/* Variable-panel accessors live on the variable_panel peer:
 * use `variable_panel_view` / `variable_panel_state_mut` directly. */

UiProfilePanelState ui_state_profile_panel(void) {
    return g_ui_state.profile_panel;
}

UiProfilePanelState *ui_state_profile_panel_mut(void) {
    return &g_ui_state.profile_panel;
}

UiMemoryPanelState ui_state_memory_panel(void) {
    return g_ui_state.memory_panel;
}

UiMemoryPanelState *ui_state_memory_panel_mut(void) {
    return &g_ui_state.memory_panel;
}

UiViewportState ui_state_viewport(void) {
    return g_ui_state.viewport;
}

UiViewportState *ui_state_viewport_mut(void) {
    return &g_ui_state.viewport;
}

void ui_state_viewport_set_size(int window_w, int window_h) {
    g_ui_state.viewport.window_w = window_w;
    g_ui_state.viewport.window_h = window_h;
}

UiPointerState ui_state_pointer(void) {
    return g_ui_state.pointer;
}

UiPointerState *ui_state_pointer_mut(void) {
    return &g_ui_state.pointer;
}

void ui_state_pointer_set(int mouse_x, int mouse_y, int mouse_button) {
    g_ui_state.pointer.mouse_x = mouse_x;
    g_ui_state.pointer.mouse_y = mouse_y;
    g_ui_state.pointer.mouse_button = mouse_button;
}

void ui_state_pointer_set_pos(int mouse_x, int mouse_y) {
    g_ui_state.pointer.mouse_x = mouse_x;
    g_ui_state.pointer.mouse_y = mouse_y;
}


UiCodePanelRuntimeState ui_state_code_panel(void) {
    return g_ui_state.code_panel;
}

UiCodePanelRuntimeState *ui_state_code_panel_mut(void) {
    return &g_ui_state.code_panel;
}


/* Camera accessors moved to glr_camera.c. Storage lives there too;
 * the UiState.camera field is gone (see src/ui/app/state.h). */

/* The `repl_state_*` forwarders for these UI slices are defined in
 * src/repl/state.c rather than here so the check-state-boundaries
 * guard's "no repl_state_*_mut from ui_*.c" rule keeps working. */
