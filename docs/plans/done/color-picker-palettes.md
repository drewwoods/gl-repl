# Color Picker - Palettes (Basic / Full / Harmony)

Status: **not-started** - design ready, awaiting implementation slot.

## Context

The floating color picker (`src/subsystems/color_picker/color_picker_state.c`
peer + `src/ui/subsystems/color_picker.c` renderer + the
`src/app/glr_color_picker_bridge.c` host) currently exposes an SV
square, a hue bar, and an optional alpha bar with a preview strip. It
edits a single constant color command (`glColor3f` / `glColor4f` /
`gluColor` / `glClearColor`) by writing the resolved RGBA back through
the editor commit pipeline.

This adds **palettes** beside the freeform sliders so the user can drop
a known-good color in one click:

1. **Basic** - one row of ~10 common named colors.
2. **Full** - a hue × tint/shade grid plus a greyscale ramp (~56 cells).
3. **Harmony** - the chosen color plus 3 *derived* swatches. User-chosen
   scheme: **tetradic / square** (hues at +90°, +180°, +270°, keeping the
   chosen S and V).

The three palettes share a **tab strip** below the preview swatch; one
palette is visible at a time (chosen so the tall Full grid only costs
height when selected).

## User-confirmed design choices

- **Layout:** tab strip (`[Basic] Full Harmony`) below the preview
  swatch; active palette grid renders under it. Popup width unchanged.
- **Harmony scheme:** tetradic / square - base color at hue `h`, three
  derived at `h+0.25`, `h+0.5`, `h+0.75` (wrapping), all sharing the
  base S and V. Swatch 0 is always the base.
- **Sticky harmony key:** the tetrad's base ("key") is *persistent
  peer-global session state*, separate from the active command's color,
  so a coordinated set can be applied across several color commands
  (open picker on command A → apply swatch 0; reopen on B → apply swatch
  1; etc.). The key survives close/reopen and does NOT move when a
  derived swatch is applied. It auto-seeds from the current color the
  first time the Harmony tab is entered, and re-anchors via a "Set key
  from current" button in the Harmony pane. Cleared by
  `color_picker_state_reset`. (No copy/paste-color clipboard - explicitly
  out of scope.)
- **Hex readout (general):** the picker exposes the current color as a
  32-bit `#RRGGBBAA` string (alpha `FF` for RGB-only commands), rendered
  on the preview strip so any picked color is reproducible later.
  Read-only for now; editable hex entry is a possible follow-up.

## Where things live (no new module, no new prefix)

Everything stays inside the existing three seams so
`check-color-picker-ui-isolation.sh` and `check-color-picker-demo-isolation`
keep passing, and `color_picker_demo` inherits palettes for free:

- **Peer** (`src/subsystems/color_picker/color_picker_state.{c,h}`) owns
  the palette tables, the active tab, the tetradic math, geometry, and
  the tab/swatch click handling. Palette colors are picker *data*, so
  static tables in the peer keep the demo self-contained.
- **UI** (`src/ui/subsystems/color_picker.{c,h}`) draws the tab strip +
  active grid from the `ColorPickerView` and extends the hit-test -
  still a pure renderer/hit-test over the view.
- **Bridge / controller / router** unchanged: palette clicks resolve to
  RGBA and flow through the same `color_picker_write_cmd()` →
  `glr_cp_write_color()` → `editor_commit_apply_external_change()` path,
  landing as one undo entry per session like a slider drag.

## Peer changes (`color_picker_state.c/.h`)

```c
typedef enum {
    CP_TAB_BASIC = 0,
    CP_TAB_FULL,
    CP_TAB_HARMONY,
    CP_TAB_COUNT
} CpPaletteTab;
```

- `static CpPaletteTab g_cp_tab` - new session state; reset in
  `color_picker_state_reset` / `color_picker_stop` is *not* required
  (tab choice can persist across opens; reset only on full state reset).
- `static const float CP_BASIC[10][3]` - black, white, mid-grey, red,
  orange, yellow, green, cyan, blue, magenta.
- `static const float CP_FULL[CP_FULL_COUNT][3]` - 8 hue columns × 6
  tint→shade rows + an 8-cell greyscale row (56). Either a literal table
  or generated from HSV at first use.
- Tetradic harmony computed at view time next to `color_picker_hsv_to_rgb`.

### `ColorPickerView` additions

```c
int   palette_tab;                  /* active CpPaletteTab */
int   tab_x, tab_y, tab_w, tab_h;   /* tab strip rect (y-up) */
int   pal_x, pal_y;                 /* palette grid origin (top-left, y-up) */
int   pal_cols, pal_rows;           /* grid shape of the active palette */
int   pal_cell, pal_cellgap;        /* cell size + inter-cell gap */
int   swatch_count;
float swatches[CP_MAX_SWATCHES][4]; /* peer-resolved RGBA (<=64) for active tab */
int   key_set;                      /* harmony key captured this session */
int   key_btn_x, key_btn_y, key_btn_w, key_btn_h; /* "Set key" button (Harmony) */
char  hex[12];                      /* "#RRGGBBAA" of the current color */
```

Embedding the resolved swatch array (≤64×4 floats ≈ 1 KB by value) keeps
the renderer pure and gives fixed *and* harmony colors one draw path.

### Geometry

`cp_compute_rects` (or a sibling) gains the tab strip + palette grid,
stacked below the preview swatch. New peer constants:
`CP_TAB_H`, `CP_SWATCH`, `CP_SWATCH_GAP`, `CP_MAX_SWATCHES`.

`color_picker_start` adds the active tab's grid height to `ph` before the
on-screen vertical clamp. The tab-switch press branch re-runs the clamp
so switching to the taller Full grid doesn't run off the bottom edge.

### Input - `color_picker_handle_press`

Insert two region checks before the existing "inside-popup no-op" and
"outside → dismiss" fallbacks:

- **Tab strip:** set `g_cp_tab`, re-clamp anchor, return
  `{consumed=1, changed=0}`. No writeback.
- **Swatch:** resolve RGBA for the hit cell (harmony from the live
  tetradic set), `cp_rgb_to_hsv` to sync SV/hue, clamp `val` to
  `g_cp_value_max` (so `glClearColor` stays ≤ `CP_CLEAR_MAX_V`), keep the
  current alpha for RGB-only swatches, then `color_picker_write_cmd()`.
  Return `{consumed=1, changed=<writeback result>}`.

The "inside popup bounds → consumed no-op" rectangle grows to cover the
tab + palette area so padding clicks don't dismiss the picker.

## UI changes (`src/ui/subsystems/color_picker.c`)

- Draw the tab strip: active tab `UI_TOK_MENU_LABEL_ACTIVE_BG`, inactive
  `UI_TOK_RAISED`, labels `UI_TOK_TEXT_PRIMARY` / `UI_TOK_TEXT_MUTED`,
  border `UI_TOK_BORDER`.
- Draw the active palette grid from `view->swatches` (`view->swatch_count`,
  `pal_cols/rows/cell`). Swatch borders use `UI_TOK_BORDER`; fills are
  picker data (not theme tokens), matching the SV-gradient convention.
- `ui_color_picker_hit_test` reports `UI_HIT_COLOR_SWATCH` across the
  expanded bounds (tab + palette) so the router keeps treating picker
  clicks as consumed. Remains a pure read of `view`.

## Tests

- `tests/test_ui.c`: hit-test cases for the tab strip and a swatch cell;
  render-with-palette smoke (asserts extra quads draw).
- Peer coverage (in-memory host, demo-style): tetradic derivation values,
  swatch click emits the expected `glColor3f(...)` source line, and a
  too-bright swatch on `CMD_CLEAR_COLOR` clamps to `CP_CLEAR_MAX_V`.

## Guards / build

- `make check-state-ownership` (includes `check-color-picker-ui-isolation`
  + `check-color-picker-demo-isolation`).
- `make test`, `make color_picker_demo`, `make check-c99`.

## Out of scope

- Persisting the active tab or a custom/recent-colors palette across
  sessions.
- Editing palette contents from the UI.
