# Tagged Example Submenus

**Summary**
Add Windows-2000-style Scene submenus for built-in examples. Examples keep
their existing flat load identity for F12 cycling and import/export behavior,
but each registry entry gains one or more curated tags. The Scene menu shows
tag category rows; hovering a tag opens a submenu of the matching examples.

The implementation should preserve the current ownership split:

- `src/repl/examples.*` owns example metadata and flat example identity.
- `src/ui/menu_bar.*` owns menu geometry, rendering, hover state, and hit-test
  classification only.
- `src/app/glr_ctrl.c` routes `UiHit` payloads.
- `src/app/glr_actions.c` performs scene/example actions.

## Current Contracts To Preserve

- `src/repl/examples.c` currently uses parallel arrays:
  `g_examples[]` for line arrays and `g_example_names[]` for labels. The public
  low-level API is `repl_examples_count()`, `repl_examples_name()`, and
  `repl_examples_lines()`.
- `src/repl/example_loader.c` wraps those APIs as `repl_example_count()`,
  `repl_example_name()`, and `repl_load_example()`. F12 cycling in
  `src/app/glr_ctrl.c::cycle_example_or_user_scene()` uses this flat count and
  must remain unchanged.
- The Scene dropdown row math is duplicated across `src/ui/menu_bar.c` and
  `src/app/glr_actions.c`. Today the fixed scene rows are offset by the number
  of examples:
  - `0`: `### EXAMPLES`
  - `1..example_count`: example rows
  - `example_count + GLR_SCENE_OFF_*`: divider, `### SCENE`, actions, user
    scenes
- `ui_menu_bar_hit_test()` returns only passive `UiHit` values. The controller
  routes `UI_HIT_MENU_ITEM` to `glr_action_menu_item_activate()`. The hard
  guard `scripts/check-ui-returns-hits-only.sh` covers this contract for
  `ui_menu_bar.c`; the new submenu hit-test must also return `UiHit` values
  only, with no input forwarding bodies.
- `ui_menu_bar_render_example_dropdown()` is legacy-named, but currently renders
  every top-level dropdown, not only the F12/example list. This feature keeps
  the name and lets the function grow a submenu render side-effect when
  `g_open_menu == MENU_SCENE`; the rename is out of scope.
- F12 cycling does not open the Scene menu. `cycle_example_or_user_scene()`
  calls `repl_load_example(idx)` directly with a flat registry index, so the
  submenu/hover state in this feature has no effect on F12 behavior.
- `g_menu_item_hover` is already mutated during render in
  `ui_menu_bar_render_example_dropdown()` (line ~773); the new submenu state
  (`g_scene_open_tag`, `g_scene_submenu_open_time`) follows that same
  "hover state updated during render" pattern, so `ui_menu_bar.c` stays
  hit-test-only by the project's boundary contract.

## Target Behavior

- Scene menu parent layout:
  - `### EXAMPLES`
  - one row per tag, with a right-facing submenu indicator
  - divider
  - `### SCENE`
  - scene actions
  - user scenes
- Hovering a tag row opens a submenu next to the parent Scene dropdown.
- Moving between tag rows switches the submenu and starts a fresh submenu fade.
- Moving from the parent tag row into the submenu keeps that submenu open.
- Clicking an example inside the submenu loads that example by its global flat
  example index.
- Multi-tag examples appear in every matching tag submenu.
- The active loaded example is highlighted in any submenu where it appears.
- If the submenu would exceed the right edge of the window, place it to the
  left of the parent dropdown. Clamp vertically if needed only after preserving
  the top alignment with the hovered tag row where practical.
- F12 cycling remains flat registry order:
  `examples[0..N-1] -> user scenes -> examples[0]`.
- The existing F12/open-menu example dropdown behavior should remain flat unless
  it is explicitly redesigned later. This feature is for Scene menu submenus.

## Step 1: Replace Parallel Example Arrays With A Registry

Update `src/repl/examples.c` to introduce a structured entry type near the
bottom of the file:

```c
typedef unsigned int ReplExampleTagMask;

typedef struct {
    const char *name;
    const char *const *lines;
    ReplExampleTagMask tags;
} ReplExampleEntry;
```

Replace `g_examples[]` and `g_example_names[]` with one `g_example_entries[]`
array. Preserve the current ordering exactly so flat example indices and F12
behavior do not change.

Initial tag table:

```c
enum {
    REPL_EXAMPLE_TAG_2D = 0,
    REPL_EXAMPLE_TAG_3D,
    REPL_EXAMPLE_TAG_POLYGONS,
    REPL_EXAMPLE_TAG_LINES,
    REPL_EXAMPLE_TAG_COUNT
};
```

Use a mask helper. Keep this file-local for the registry's own initializer
table; the public mask helper for tests and other call sites is
`repl_example_tag_bit()` in `examples.h` (added in Step 2):

```c
#define EXAMPLE_TAG_BIT(tag) (1u << (tag))
```

Assign at least one tag to every example. Suggested first pass:

- `2D`: assignment sketch, spirograph curve, Bezier curve, scratch arrays, most
  flat function demos.
- `3D`: cube, torus, terrain, waves, particles, glow particles, snowfall,
  transform stress, stress scene, annotated orbit plot.
- `Polygons`: cube, function polygon demos, conditional quads, tessellator
  demos, torus/surfaces if desired.
- `Lines`: ring, recursive triangle tree, spirograph, ripple ring, Bezier,
  transform guides/stress if the example primarily demonstrates line geometry.

Do not put tag metadata into example source text or `// @cfg` headers. Tags are
registry metadata only.

## Step 2: Add Example Tag Query API

Extend `src/repl/examples.h` and `src/repl/examples.c` with low-level metadata
queries. Add `<limits.h>` to `examples.h` for `CHAR_BIT`:

```c
int repl_example_tag_count(void);
const char *repl_example_tag_label(int tag_idx);
unsigned int repl_example_tag_mask(int example_idx);
int repl_example_has_tag(int example_idx, int tag_idx);
int repl_example_count_for_tag(int tag_idx);
int repl_example_index_for_tag(int tag_idx, int ordinal);

/* Visible-tag layer: tags whose `repl_example_count_for_tag(tag) > 0`.
 * Both UI offset math and action offset math MUST use these helpers so
 * empty-tag skipping cannot desynchronize the two sides. Mirrors the
 * dense-user-scene helper pattern in src/app/glr_actions.h. */
int repl_example_visible_tag_count(void);
int repl_example_visible_tag_at(int dense_idx);

/* Public mask helper. The bit position equals the tag index, but exposing
 * the helper keeps test code from assuming the bit layout. Invalid or
 * unrepresentable tag indices return 0 before shifting. */
static inline unsigned int repl_example_tag_bit(int tag_idx) {
    if (tag_idx < 0 || tag_idx >= repl_example_tag_count() ||
        tag_idx >= (int)(sizeof(unsigned int) * CHAR_BIT))
        return 0u;
    return 1u << (unsigned int)tag_idx;
}
```

Behavior:

- Invalid `tag_idx` returns neutral values (`NULL`, `0`, or `-1` as
  appropriate). This includes `repl_example_tag_bit(tag_idx)`, which returns
  `0` for negative, out-of-range, or unrepresentable shift counts.
- Invalid `example_idx` returns neutral values.
- `repl_example_index_for_tag(tag_idx, ordinal)` walks the flat registry and
  returns the global example index for the Nth matching example.
- `repl_example_visible_tag_count()` returns the count of tags whose
  `repl_example_count_for_tag()` is > 0. `repl_example_visible_tag_at(d)`
  maps a dense visible-tag index `[0, visible_tag_count)` to the underlying
  full tag index; returns -1 for out-of-range `d`.
- Avoid storing per-tag generated arrays unless profiling shows a need. The
  example count is small, and walking the registry keeps tag metadata single
  sourced.

Keep the existing public functions compatible:

```c
int repl_examples_count(void);
const char *repl_examples_name(int idx);
const char *const *repl_examples_lines(int idx);
```

After the registry change, these should read from `g_example_entries[idx]`.

## Step 3: Add Loader/Action Surface For Global Example Indices

The submenu click should not encode fake parent-menu row indices. Add a small
action helper in `src/app/glr_actions.c` and declare it in
`src/app/glr_actions.h`:

```c
int glr_action_load_example(int example_idx);
```

Implementation should reuse the exact transient reset path currently duplicated
inside `glr_action_menu_item_activate()` for Scene example rows:

```c
editor_reset_transients();
repl_load_example(example_idx);
return 1;
```

Then change the current parent Scene example-row branch, while it still exists
during migration, to call this helper. This keeps direct menu rows and future
submenu rows behavior-identical.

## Step 4: Change Scene Parent Row Math From Examples To Tags

In `src/ui/menu_bar.c`, define the Scene parent section in terms of *visible*
tag rows (tags with at least one matching example):

- `0`: `### EXAMPLES`
- `1..visible_tag_count`: tag rows
- `visible_tag_count + SCENE_OFF_DIVIDER`: divider
- `visible_tag_count + SCENE_OFF_HDR`: `### SCENE`
- `visible_tag_count + SCENE_OFF_NEW`: `New empty scene`
- `visible_tag_count + SCENE_OFF_SAVE`: `Save to output.c`
- `visible_tag_count + SCENE_OFF_RENAME`: `Rename active scene`
- `visible_tag_count + SCENE_OFF_SCENES + dense_scene_idx`: user scenes

Update these helpers together:

- `menu_item_count(MENU_SCENE)`
- `menu_item_label(MENU_SCENE, i)`
- `menu_item_shortcut(MENU_SCENE, i)`
- any active example/scene highlight logic in the parent dropdown renderer
- hit-test exclusions for header/divider rows

Important: `GLR_SCENE_OFF_*` values can stay as-is because they are relative to
the start of the section after the dynamic examples block. Only the dynamic
block changes from `example_count` to `visible_tag_count`.

Empty-tag handling: if a tag has zero matching examples, both UI and action
sides MUST skip it. Use `repl_example_visible_tag_count()` (added in Step 2)
on both sides; never reach for raw `repl_example_tag_count()` inside menu-row
arithmetic. If raw and visible counts diverge, action offsets desynchronize
from UI offsets and the user's New/Save/Rename/user-scene clicks land on the
wrong action. Treat `repl_example_visible_tag_count()` as the load-bearing
identity for Scene-menu row math.

In `src/app/glr_actions.c::glr_action_menu_item_activate()`, mirror the same
offset migration using the same visible-tag count:

```c
int tag_count = repl_example_visible_tag_count();
```

Then use `tag_count + GLR_SCENE_OFF_*` for New/Save/Rename/user-scene rows.
Parent tag rows are intentional no-ops: detect them with
`item_idx >= 1 && item_idx <= tag_count` and `return 0` (menu stays open) so
the user can hover the tag to see the submenu, then click an example inside it.
Returning `1` would close the menu and defeat the submenu workflow.

Mirroring rule for hit-test exclusion: `ui_menu_bar_dropdown_item_hit()`
already returns `-1` for `###` headers and `---` dividers. Tag rows must
remain hit-testable (clicks are no-ops but hover must register), so they
should pass through the existing `-1` filter unchanged.

## Step 5: Add Submenu UI State And Geometry

Add file-private state in `src/ui/menu_bar.c`:

```c
static int g_scene_open_tag = -1;
static float g_scene_submenu_open_time = -1.0f;
```

Reset this state in:

- `ui_menu_bar_close()`
- `ui_menu_bar_set_open_menu()` when opening anything other than Scene
- `ui_menu_bar_set_open_menu()` when opening Scene fresh

Mutation contract: `g_scene_open_tag` and `g_scene_submenu_open_time` are
mutated **only inside the render path**
(`ui_menu_bar_render_example_dropdown()`), mirroring the existing
`g_menu_item_hover` pattern. `ui_menu_bar_hit_test()` must stay passive — it
reads `g_scene_open_tag` to know which submenu rect to test against, but
never writes either field. This is what keeps the new submenu code inside
the `scripts/check-ui-returns-hits-only.sh` boundary.

During Scene dropdown rendering (write path):

- Compute the currently hovered parent row via `ui_menu_bar_dropdown_item_hit()`.
- If that row maps to a tag row, update `g_scene_open_tag`.
- If the tag changes, set `g_scene_submenu_open_time = snap->anim_time`.
- If the pointer is inside the currently open submenu, keep
  `g_scene_open_tag` unchanged.
- If the pointer is on a parent row that is not a tag row (header, divider,
  scene action, user-scene row), close the submenu — the user has moved
  attention away from the tag area.
- If the pointer is outside both the parent dropdown and the submenu, close the
  submenu but leave the parent dropdown behavior unchanged.

Hit-test reads the open-tag state without writing it: the new submenu hit
branch in `ui_menu_bar_hit_test()` calls `scene_example_submenu_rect(
g_scene_open_tag, ...)` to test against whatever submenu the previous
render frame opened. With `g_scene_open_tag == -1` no submenu rect exists,
so hit-test transparently falls through to parent rows. This implies a
one-frame lag between hovering a tag and the submenu being click-targetable
— acceptable because the submenu fade is also one frame behind.

Add local geometry helpers. The parent-row ↔ tag mapping goes through the
visible-tag layer added in Step 2:

```c
/* Map parent row [1..visible_tag_count] to the underlying tag_idx via
 * repl_example_visible_tag_at(row - 1). Returns -1 for non-tag rows
 * (header, divider, scene actions, user scenes). */
static int scene_tag_idx_for_parent_row(int row);

/* Inverse: returns the parent row that displays tag_idx, or -1 if
 * tag_idx is not currently visible (zero matching examples). */
static int scene_parent_row_for_tag(int tag_idx);

static int scene_example_submenu_rect(int tag_idx,
                                      int *sx, int *sy,
                                      int *sw, int *sh);
```

`scene_example_submenu_rect()` should:

- Require `g_open_menu == MENU_SCENE`.
- Use `menu_dropdown_rect()` for the parent rect.
- Width should be based on the longest matching example label, with the same
  minimum width/padding style as the parent dropdown.
- Height is `max(1, repl_example_count_for_tag(tag_idx)) * LINE_H + 8`.
- Align its top row with the matching parent tag row.
- Place to the right of the parent by default.
- Flip to the left when `sx + sw > viewport.window_w`.

Expose a thin test-only accessor so `tests/test_ui_menu_bar.c` can drive
hit-test points inside the submenu without scanning the viewport:

```c
/* Test helper: return the submenu rect that would render for tag_idx,
 * assuming MENU_SCENE is the open menu. Returns 0 if no rect would
 * render (e.g. tag has no examples). */
int ui_menu_bar_scene_example_submenu_rect_for_test(int tag_idx,
                                                    int *sx, int *sy,
                                                    int *sw, int *sh);
```

Keep this declaration in `src/ui/menu_bar.h` alongside other
`_for_test` accessors, and document that it ignores `g_scene_open_tag`
so a test can probe any tag's geometry without first driving the
hover state.

## Step 6: Render Tag Rows And Submenu Rows

In the parent Scene dropdown:

- Render tag labels from `repl_example_tag_label(tag_idx)`.
- Draw a small right-facing arrow indicator near the right edge of tag rows.
  A text glyph such as `>` is enough and avoids adding a custom icon path.
- Highlight the hovered/open tag row using the existing hover color.
- Do not highlight the parent tag row as the active example; active example
  highlighting belongs in the submenu rows. (UX consequence: when the user
  arrows back via F12, the parent dropdown shows no "you are here" cue
  unless they hover into the matching tag. Acceptable for v1 — a small
  bullet/dot suffix on tag rows that contain the active example is a
  future polish item.)

After rendering the parent dropdown, render the open submenu if
`g_open_menu == MENU_SCENE && g_scene_open_tag >= 0`:

- Use `ui_fade_alpha(snap->anim_time, g_scene_submenu_open_time)`.
- Draw background/border with the same colors as the parent dropdown.
- Iterate `ordinal = 0..repl_example_count_for_tag(tag)-1`.
- Resolve `example_idx = repl_example_index_for_tag(tag, ordinal)`.
- Draw `repl_example_name(example_idx)`.
- Highlight hover row with the existing hover color.
- Highlight active example with the green accent if
  `example_idx == snap->scenes.active_example_idx`.

This submenu render is scoped to `MENU_SCENE` only. The legacy-named
`ui_menu_bar_render_example_dropdown()` renders every top-level dropdown
today; the submenu code path must early-out for `g_open_menu != MENU_SCENE`
so opening the File or Config dropdown doesn't leak a stale Scene submenu.

Keep blend enable/disable balanced around the whole parent+submenu render.

## Step 7: Extend Hit Testing For Submenu Examples

Extend `UiHitKind` in `src/ui/hit.h`:

```c
UI_HIT_EXAMPLE_SUBMENU_ITEM,
```

Document field semantics:

- `cmd_idx = tag_idx`
- `item_idx = global example_idx`
- `line_idx = ordinal within the tag submenu` if useful, otherwise `-1`

Add a hit-test helper in `src/ui/menu_bar.c`:

```c
static UiHit scene_example_submenu_hit_test(int mx, int my);
```

Priority (insert submenu rows at the top of the existing chain in
`ui_menu_bar_hit_test()`, which today is parent-row → pin → menu-button):

1. Submenu rows.
2. Parent dropdown rows.
3. Pin buttons.
4. Top-level menu buttons.

This prevents a submenu click from being interpreted as an outside click that
closes the parent menu before loading the example. The submenu rect and parent
dropdown rect do not overlap geometrically (submenu is placed to the right /
left of the parent), so a real geometric collision is unlikely; the priority
ordering is the belt-and-suspenders defense against future layout changes.

`ui_menu_bar_dropdown_item_hit()` can remain parent-menu-only for compatibility,
or it can internally call a parent-only helper. Keep its current return contract:
it returns parent dropdown row indices, not submenu example indices.

Update `ui_menu_bar_hit_test()` so submenu hits return
`UI_HIT_EXAMPLE_SUBMENU_ITEM` with the global example index in `item_idx`.

## Step 8: Route Submenu Hits In The Controller

In `src/app/glr_ctrl.c`:

- Add a `route_example_submenu_item_hit(const UiHit *hit)` helper.
- Call `glr_action_load_example(hit->item_idx)`.
- Close the menu after a successful load with `ui_menu_bar_close()`.
- Request redraw.

Update the switch in `glr_ctrl_router_handle_code_panel_hit()` to dispatch
`UI_HIT_EXAMPLE_SUBMENU_ITEM`.

Also update the dropdown-dismiss condition near the top of
`glr_ctrl_router_handle_code_panel_hit()` so submenu hits are treated like
menu-item hits and do not dismiss the menu before routing:

```c
hit.kind != UI_HIT_EXAMPLE_SUBMENU_ITEM
```

## Step 9: Keep Legacy Parent Activation Safe

After parent rows become tags, `glr_action_menu_item_activate(GLR_MENU_SCENE,
tag_row)` should not accidentally load an example. The old condition:

```c
if (item_idx >= 1 && item_idx <= example_count)
```

must be removed or changed to a tag no-op. Concretely: bind
`tag_count = repl_example_visible_tag_count()` (the same visible-tag
identity used by `menu_item_count(MENU_SCENE)`), then detect
`item_idx >= 1 && item_idx <= tag_count` and `return 0` immediately so the
controller leaves the dropdown open and the user can keep hovering tags. All
example loading from the Scene menu should flow through
`UI_HIT_EXAMPLE_SUBMENU_ITEM` and `glr_action_load_example(example_idx)`.

Verify the user-scene row mapping still uses dense user-scene indices, and
that the visible-tag count drives the offset:

```c
int scene_idx = item_idx - (tag_count + GLR_SCENE_OFF_SCENES);
```

## Step 10: Tests

Update `tests/test_repl_core_examples.c` or add a focused examples test block:

- `repl_example_tag_count() > 0`.
- Every tag has a non-empty label.
- Every example has a non-zero tag mask.
- Every bit set in an example mask is within `[0, repl_example_tag_count())`.
- Every tag query ordinal maps to a valid global example index.
- `repl_example_has_tag(idx, tag)` agrees with
  `repl_example_tag_mask(idx) & repl_example_tag_bit(tag)`. The public
  `repl_example_tag_bit()` inline added in Step 2 is the canonical mask
  helper; do not assume the raw `1u << tag` layout in test code.
- Invalid-index tests should assert `repl_example_tag_bit(-1) == 0` and
  `repl_example_tag_bit(repl_example_tag_count()) == 0`.
- `repl_example_visible_tag_count() <= repl_example_tag_count()`, and
  every dense visible index maps to a tag with a non-zero example count
  (regression guard for the empty-tag skip path).
- At least one known multi-tag example is discoverable under each assigned tag.
- Existing example load/export/round-trip tests still pass without fixture
  changes unless the registry conversion accidentally changes source lines.

Update `tests/test_ui_menu_bar.c`:

- Scene parent dropdown uses tag rows, not example rows. The existing
  `find_dropdown_item_point(GLR_MENU_SCENE, 1, ...)` style probe will now
  hit the first tag row, not the first example. Update fixtures that
  asserted example-row positions accordingly.
- Parent Scene tag row hit returns `UI_HIT_MENU_ITEM` with
  `cmd_idx == GLR_MENU_SCENE` and the parent row index.
- Hovering a tag row causes the submenu render path to draw example text under
  GL stubs. The render-time hover update means the test must call
  `ui_menu_bar_render_example_dropdown(&snap)` (which mutates
  `g_scene_open_tag`) before issuing the submenu hit-test.
- A point inside a submenu example row returns
  `UI_HIT_EXAMPLE_SUBMENU_ITEM`. Use the new
  `ui_menu_bar_scene_example_submenu_rect_for_test()` helper to pick a
  pixel inside the submenu rather than scanning the viewport.
- Submenu hit payload has `cmd_idx == tag_idx` and
  `item_idx == global example_idx`.
- Active example highlight path renders when `snap.scenes.active_example_idx`
  matches a submenu example.
- Offscreen horizontal flip: a narrow-viewport test feeds the test helper
  a tag index and asserts the returned rect lands to the left of the
  parent rather than off-screen to the right.
- Parent dropdown hit tests still work for File, Tutorials, Config, and Scene
  fixed rows.
- Existing assertion at `test_ui_menu_bar.c:286` that exercises the Scene
  dropdown with `snap.scenes.active_example_idx = 0` continues to render
  rows (the parent now renders tag rows); the active-example highlight
  moves to the submenu and only fires when a submenu is open. Either
  drive the hover state to open the matching submenu, or relax the
  assertion to just confirm rendering happens.

Controller/action coverage:

- If there is an existing controller hit-routing test seam, add a submenu hit
  case that loads the expected global example and closes the menu.
- If not, keep controller changes minimal and rely on action/helper tests plus
  UI hit-test tests.

## Step 11: Verification Commands

Run the focused tests first:

```bash
make test_ui_menu_bar USE_GL_STUBS=1
make test_repl_core_examples USE_GL_STUBS=1
```

Then run the stub suite and compile both sample modes:

```bash
make test-stubs
make sample USE_GL_STUBS=1
make sample
```

If GL headers/libraries are unavailable locally, `make sample` may fail while
the stub path passes. In that case, record the failure and exact missing symbol
or header.

## Non-Goals

- Do not change example source text format.
- Do not change F12 cycling order.
- Do not add arbitrary user-defined tags yet.
- Do not turn the GL stubs into a renderer.
- Do not refactor the whole menu system beyond the row math and submenu
  support needed for this feature.

## Open Questions

- Should the first tag set stay limited to `2D`, `3D`, `Polygons`, and `Lines`,
  or should obvious categories such as `Animation`, `Functions`, and `Particles`
  be added immediately? Suggest starting with the four-tag set; add categories
  once the assignment table proves stable.
- Should clicking a parent tag row pin/keep the submenu open, or should hover be
  the only opener? Resolved above (Step 4) — parent tag rows are no-ops and
  hover is the only opener.
- Should a tag row show a count, for example `3D (12)`, or keep the Windows
  menu style minimal? Suggest minimal for v1; the active-example bullet
  follow-up (Step 6) is the higher-value polish item.
- Should an active-example bullet/dot suffix mark tag rows whose submenu
  contains the currently loaded example? Out of scope for v1 per Step 6; the
  hit-test/render plumbing for it already exists (`snap.scenes.active_example_idx`),
  so this is a one-line addition later.
