/*
 * glr_config.h - Config item descriptors and keyed state access.
 *
 * Defines the stable config-key vocabulary used across the app shell for
 * toggles and cycles such as grid theme, overlays, replay options, panel
 * layout, audio mode, and render features. The descriptor table itself
 * lives in glr_actions.c; this header exposes the keyed API the rest of
 * the program uses to inspect, set, and cycle those items.
 *
 * Export/import persists config by GlrConfigKey-backed names in `@cfg`
 * headers rather than table indices, so examples and saved scenes survive
 * descriptor reordering. Keyboard shortcuts also hang off the same
 * descriptors: ASCII control keys and GLUT special keys map to config rows
 * through the fields on GlrConfigItem.
 *
 * When adding an item, extend GlrConfigKey and add the matching
 * GlrConfigItem row in glr_actions.c. Keep the descriptor table as the
 * single source of truth for slugs, labels, shortcuts, and state names.
 */
#ifndef GLR_CONFIG_H
#define GLR_CONFIG_H

#include <stddef.h>

#include "render3d/themes.h"

/* Stable config item identifiers. Used by menu dispatch, export/import,
 * help overlay, and tests to refer to config items by name rather than table
 * index. Items are in two groups: rendering features (MSAA, wireframe, etc.)
 * and visual overlays (grid, axes, vertices, normals, etc.). */
typedef enum GlrConfigKey {
    GLR_CONFIG_NONE = 0,
    GLR_CONFIG_ACCUM_EFFECT,
    GLR_CONFIG_ACCUM_PASSES,
    GLR_CONFIG_AUDIO_MODE,
    GLR_CONFIG_AUTO_NORMALS,
    GLR_CONFIG_AUTO_TIME,
    GLR_CONFIG_AXES_THEME,
    GLR_CONFIG_BACKDROP,
    GLR_CONFIG_CAMERA_ROTATE,
    GLR_CONFIG_CODE_PANEL_LAYOUT,
    GLR_CONFIG_CPU_PROFILE,
    GLR_CONFIG_DEPTH_VIZ,
    GLR_CONFIG_FOCUS_ORIGIN,   /* action row: no state; eases target to origin */
    GLR_CONFIG_GRID_BRIGHTNESS,
    GLR_CONFIG_GRID_EXTENT,
    GLR_CONFIG_GRID_MAJOR,
    GLR_CONFIG_GRID_THEME,
    GLR_CONFIG_LIGHT_INDICATORS,
    GLR_CONFIG_LIGHT_THEME,
    GLR_CONFIG_LINE_SMOOTH,
    GLR_CONFIG_LOOK_DOWN_Z,    /* action row: no state; eases orbit+pan to zero, keeps dist */
    GLR_CONFIG_MEMORY_PROFILE,
    GLR_CONFIG_MSAA,
    GLR_CONFIG_NORMAL_VECTORS,
    GLR_CONFIG_ORTHO_MODE,
    GLR_CONFIG_OVERLAY_SCOPE,
    GLR_CONFIG_PAREN_MATCH,
    GLR_CONFIG_PAREN_SCOPE,
    GLR_CONFIG_POINT_ATTENUATION,
    GLR_CONFIG_POLY_HIGHLIGHT,
    GLR_CONFIG_POST_FX_EFFECT,
    GLR_CONFIG_POST_FX_SCOPE,
    GLR_CONFIG_PROJECTION,     /* perspective/ortho projection, free camera (independent of ORTHO_MODE) */
    GLR_CONFIG_REPLAY,
    GLR_CONFIG_REPLAY_EXPAND,
    GLR_CONFIG_REPLAY_MODE,
    GLR_CONFIG_RESET_CAMERA,   /* action row: no state; eases camera to default */
    GLR_CONFIG_STENCIL_VIZ,
    GLR_CONFIG_SYNTAX_HIGHLIGHT,
    GLR_CONFIG_VARIABLE_PANEL,
    GLR_CONFIG_VERTEX_LABELS,
    GLR_CONFIG_VERTEX_LABEL_PLACEMENT,
    GLR_CONFIG_VERTEX_OUTLINES,
    GLR_CONFIG_VERTEX_OUTLINE_STYLE,
    GLR_CONFIG_VERTEX_POINTS,
    GLR_CONFIG_WINDING_VIEW,
    GLR_CONFIG_WIREFRAME,
    GLR_CONFIG_WRAP_AT_COMMA,
    GLR_CONFIG_XFORM_GUIDE_MODE,
    GLR_CONFIG_COUNT
} GlrConfigKey;

/* Descriptor for a single config item (toggle or cycle). Specifies the menu
 * label, keyboard shortcut (ASCII key code or GLUT_KEY_* with is_special flag),
 * the stable GlrConfigKey, state count (2 for toggle, >2 for cycle with named
 * states), state name labels, and whether this item is a section header/separator.
 * Registered in g_cfg_items[] in glr_actions.c; count auto-computes via sizeof. */
typedef struct {
    const char  *label;
    /* Stable persistence/tooling identifier. Keep this independent of the
     * presentation label: labels may be retitled without changing @cfg
     * headers in saved scenes, tutorials, or workspaces. */
    const char  *slug;
    int          key_code;      /* ASCII ctrl key or GLUT_KEY_* */
    int          is_special;     /* 1 for GLUT special keys */
    /* GLUT_ACTIVE_* modifier bitmask the binding requires (0 = none).
     * Drives the displayed shortcut text (e.g. GLUT_ACTIVE_SHIFT ->
     * "Ctrl+Shift+V") and exact dispatch in
     * glr_cfg_handle_ascii_shortcut. */
    int          modifiers;
    GlrConfigKey key;
    int          state_count;    /* 2 = toggle; >2 = cycle */
    const char **state_names;
    int          section_header; /* 1 for separator/header rows */
    /* Optional runtime-owned label slot used only for display. Callers that
     * render user-facing text should go through
     * glr_config_item_display_label(). */
    const char **display_label_override;
} GlrConfigItem;

/* The "Accum passes" ladder: the fixed set of accumulation sample counts
 * the cycle steps through, low to high. Single source of truth - the
 * display-name array (glr_actions.c), the int step table (glr_config.c),
 * and the GLR_ACCUM_PASSES startup validator (glr_ctrl.c) all derive
 * from this one list via the X-macro idiom, so the ladder is edited in
 * exactly one place and the three views can never drift. Each consumer
 * feeds one of the entry macros below to GLR_ACCUM_PASS_LADDER.
 *
 * render3d is deliberately NOT a view of this list (it must not depend on
 * src/app): its config validator only range-checks accum_passes against
 * MAX_ACCUM_SAMPLES, so steps may be added freely - but none may exceed it. */
#define GLR_ACCUM_PASS_LADDER(X) \
    X(1)  \
    X(2)  \
    X(4)  \
    X(6)  \
    X(8)  \
    X(10) \
    X(12) \
    X(14) \
    X(16)
#define GLR_ACCUM_PASS_NAME_ENTRY(n) #n,   /* -> "1", "2", ...  (menu labels) */
#define GLR_ACCUM_PASS_STEP_ENTRY(n) n,    /* -> 1, 2, ...      (sample counts) */

/* Exported config item table and count. g_cfg_items[] lives in
 * glr_actions.c; CFG_ITEM_COUNT is the count (auto-computed via sizeof). Used
 * by the menu UI and config accessor functions to iterate items. */
extern const GlrConfigItem g_cfg_items[];
extern const int CFG_ITEM_COUNT;

/* Query all config items. Outputs the item array and count. Used by the menu
 * UI to render the Config dropdown and by help overlay to list all toggles. */
const GlrConfigItem *glr_config_items(int *count);

/* Query a specific config item by index (0 to CFG_ITEM_COUNT-1). Returns the
 * item descriptor or NULL if out of bounds. Used by menu rendering. */
const GlrConfigItem *glr_config_item_at(int idx);

/* User-facing display label for an item. Uses the authored label unless
 * the row has a runtime override (currently the MSAA row). */
const char *glr_config_item_display_label(const GlrConfigItem *item);

/* Stable export/import slug for an actionable item. Returns the explicit
 * descriptor slug, or NULL for NULL/non-action rows. */
const char *glr_config_item_slug(const GlrConfigItem *item);

/* Validate the descriptor table's persistence identifiers. Returns 1 when
 * every actionable row has a unique, bounded lowercase [a-z0-9_] slug and
 * structural rows have no slug. On failure, writes one concise diagnostic to
 * `err` when supplied. */
int glr_config_validate(char *err, size_t err_sz);

/* Get the current state value of a config item. Returns 0/1 for toggles, 0-N
 * for cycles (index into state_names[]). Used by rendering (to highlight the
 * active state) and by logic that checks feature flags. */
int  glr_config_get(GlrConfigKey key);

/* Query the number of states for a config item. Returns 2 for toggles, >2 for
 * cycles. Used by UI to determine whether to render a toggle or cycle menu. */
int  glr_config_state_count(GlrConfigKey key);

/* Get the display name for a specific state value. Returns the string label
 * for state (index into state_names[]). Used by menu rendering and export
 * (to convert state value back to name for @cfg headers). */
const char *glr_config_state_name(GlrConfigKey key, int value);

/* Set a config item to a specific state. Applies the change immediately (e.g.,
 * enables GL_LIGHTING, changes grid theme, etc.). Called by menu dispatch and
 * keyboard shortcuts. */
void glr_config_set(GlrConfigKey key, int value);

/* Cycle a config item by delta steps (delta +1 = next state, -1 = prev state).
 * Wraps around at boundaries. Returns the new state value. Called by keyboard
 * shortcuts and config menu cycling. */
int  glr_config_cycle(GlrConfigKey key, int delta);

/* Backdrop/grid pairing policy. Some backdrops own a matching grid while
 * active; paired grid targets stay valid config values but are hidden from
 * direct user cycling/menu activation. */
int  glr_config_backdrop_forces_grid(Render3dBackdropMode backdrop,
                                     Render3dGridTheme *grid_out);
int  glr_config_grid_user_selectable(Render3dGridTheme grid);

/* ---- Section model over g_cfg_items[] -------------------------------- *
 *
 * Groups the flat descriptor table by its "### " header rows so the
 * Config menu can present each section as a flyout submenu (mirroring
 * the Scene menu's example-tag submenus).
 *
 * Data-faithful: this API counts ONLY real "### " header rows. The
 * synthetic "All" view (the full flat list, chrome included) is a
 * menu-layer concern and is deliberately NOT counted here - single
 * ownership, no double-count. "---" separator rows are excluded from
 * every named section's item range. */

/* Number of "### " section headers present in g_cfg_items[]. */
int  glr_config_section_count(void);

/* Raw label for section `s` (0-based), with the "### " marker
 * stripped. UPPERCASE as authored in g_cfg_items[]. NULL if out of
 * range. */
const char *glr_config_section_label(int section);

/* Sentence-cased display label: "RENDERING" -> "Rendering",
 * "TIME & REPLAY" -> "Time & replay". Pre-computed once; the UI layer
 * renders this directly without per-frame string work. NULL if out of
 * range. */
const char *glr_config_section_display_label(int section);

/* Resolve section `s` to its contiguous run of actionable item rows in
 * g_cfg_items[]: *start = index of the first item, *count = number of
 * items (header and trailing "---" excluded). Returns 1 on success, 0
 * if `section` is out of range. */
int  glr_config_section_range(int section, int *start, int *count);

/* Row classification for g_cfg_items[idx], used by the generic submenu
 * provider so the "All" flyout (which spans the whole table) keeps
 * header/separator rows as inert chrome. */
typedef enum GlrConfigRowKind {
    GLR_CFG_ROW_ITEM = 0,   /* an actionable toggle/cycle row */
    GLR_CFG_ROW_HEADER,     /* a "### " section header */
    GLR_CFG_ROW_SEPARATOR   /* a "---" separator */
} GlrConfigRowKind;

GlrConfigRowKind glr_config_row_kind(int idx);

#endif /* GLR_CONFIG_H */
