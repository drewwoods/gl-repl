# Scene/File menu rework + save-as-scene-name

## Context

The Scene menu muddies two concepts: it is mostly an *active-scene
selector* (examples + user scenes) but also carries *actions* — "New
Empty Scene", "Save to output.c", "Rename active scene" — and its
internal `### SCENE` subheading collides with the top-level menu name
"Scene". This makes the menu hard to read and mixes modes. Separately,
single-file save is always `output.c`, ignoring the scene's name.

Goal: Scene menu becomes a pure selector with clearly-named sections;
the scene *actions* move to the File menu (which already owns
Export/Import/Workspace); single-file save derives its filename from
the active scene name. "Close active scene" is **out of scope** here —
tracked separately in `plans/not-started/scene-close-capability.md`
(non-trivial, open semantic questions); this rework only makes room for
it in the File menu later.

> Line references are indicative (this work sits on a branch that has
> shifted `menu_bar.c` / `glr_*` / tests). Re-derive at implementation;
> the structural claims bind, not the numbers.

## Locked decisions (from Q&A)

1. Scene-menu subheadings: `### EXAMPLES` and `### YOUR SCENES`
   (keeps the all-caps `###` style; no longer collides with "Scene").
2. Scene menu = pure selection: `### EXAMPLES` + example rows, then
   `### YOUR SCENES` + user-scene rows. **No action rows.** **Both
   headers are always present even when a section is empty** (fixed
   layout — variable layout would fork `menu_item_count`, the
   scene-idx dispatch math, and tests; the current code already always
   shows `### SCENE` with zero user scenes, so this is status-quo).
3. File menu gains: **New Scene**, **Save Scene**, **Rename Scene**
   (the *action semantics* move from the Scene menu; New Scene's body
   is **not** verbatim — see §B [P1] — and Close is *not* added now).
4. "Save Scene" writes `<scene-slug>.c` into the bound workspace dir;
   cwd if no workspace bound; `output.c` if there is no active *named*
   user scene (example / transient). **Ctrl+S is unchanged** — it
   still runs Export→`output.c` (the existing `REPL_FILE_ITEM_EXPORT`
   shortcut). Save Scene is a distinct File item, no shortcut.

## Implementation

### A. Menu model — `src/ui/menu_bar.c` + `src/app/glr_actions.h`

- `glr_actions.h`: collapse `GLR_SCENE_OFF_*` — the Scene menu no
  longer has DIVIDER/NEW/SAVE/RENAME rows. New layout offsets:
  `[0] "### EXAMPLES"`, `[1..e]` examples (`e = repl_example_count()`),
  `[e+1] "### YOUR SCENES"`, `[e+2 ..]` user scenes. Keep only the two
  derived constants needed (`SCENE_HDR = e+1`, `SCENE_SCENES = e+2`),
  or compute inline. **[P1] The `---` separator is a real row index**
  (every visible row is an item index; the Scene menu's `---` and the
  Tutorials `---` already occupy indices, and the dropdown hit-test
  skips `### `/`---`). So the File enum must give the separator its
  own slot, not "auto-grow after four":
  ```
  REPL_FILE_ITEM_EXPORT = 0, _IMPORT, _SAVE_WORKSPACE, _LOAD_WORKSPACE,
  REPL_FILE_ITEM_SCENE_SEP,      /* "---" */
  REPL_FILE_ITEM_NEW_SCENE, _SAVE_SCENE, _RENAME_SCENE,
  REPL_FILE_ITEM_COUNT
  ```
- `menu_bar.c` `menu_item_count` / `menu_item_label` /
  `menu_item_shortcut`: SCENE returns
  `1 + e + 1 + repl_user_scene_count()` (both headers always counted,
  decision #2); labels emit the two headers + rows. FILE returns
  `REPL_FILE_ITEM_COUNT`; `menu_item_label` maps
  `REPL_FILE_ITEM_SCENE_SEP → "---"` and the three new rows by their
  explicit enum index. Keep `FILE_ITEM_EXPORT` shortcut `Ctrl+S`.

### B. Dispatch — `src/app/glr_actions.c` (`glr_action_menu_item_activate`)

- `GLR_MENU_SCENE` branch: keep only example load
  (`item_idx 1..e → glr_scene_load_example(item_idx-1)`) and
  user-scene load. Scene-idx math changes from
  `item_idx - (e + GLR_SCENE_OFF_SCENES)` to
  `item_idx - (e + 2)` (after the new `### YOUR SCENES` header).
  Skip the header rows. Delete the NEW/SAVE/RENAME cases here.
- `GLR_MENU_FILE` branch:
  - **[P1] `REPL_FILE_ITEM_NEW_SCENE` is NOT the old body verbatim.**
    The old Scene-New only cleared `active_example_idx` +
    `editor_clear_all_cmds()`; it never detached an active *user*
    scene. With Save Scene now keyed off `repl_active_user_scene()`
    (§C), File→New from a user scene would leave that slot active and
    Save Scene would overwrite the *old* scene's `<slug>.c` with the
    emptied buffer. New Scene must enter the **transient lifecycle** so
    no slot is active: clear `active_example_idx`,
    `repl_scenes_enter_transient_scene()` + `repl_scenes_reset_for_transient()`
    (`scenes.h:34/41` — the same choreography tutorial start uses),
    `editor_clear_all_cmds()`, and `editor_undo_clear()` (wholesale
    document replacement → the global undo-ring invariant, same as F12
    / load-scene). Net: after New Scene, `repl_active_user_scene() ==
    -1` and Save Scene correctly falls back to `output.c` until the
    scene is promoted/named.
  - `_RENAME_SCENE`: the old body verbatim (active-slot guard →
    `editor_inline_rename_begin`; "No active scene to rename" status
    when none).
  - `_SAVE_SCENE`: new core call (§C).
  - `REPL_FILE_ITEM_SCENE_SEP`: non-actionable (dropdown hit-test
    already returns -1 for `---`); no dispatch case.
- `glr_scene_menu_slot_for_dense_index()` (≈`glr_actions.c:287`) is
  unaffected — it maps a *dense user-scene index* to a slot by walking
  occupied slots, independent of menu offsets. The scene tab router
  (`route_scene_tab_hit`) and F12 (`cycle_example_or_user_scene`,
  `glr_ctrl.c:2067`) both go through dense-slot order, **not** menu
  item offsets — verify (read) but expected no change.

### C. Save-as-scene-name — `src/repl/scenes.c` + `src/repl/core.h`

- New `repl_save_active_scene(const ReplExportLayout *layout)`:
  - If `repl_active_user_scene() >= 0`: `scene_filename_slug(name,…)`
    (existing helper, `scenes.c:429`), build path =
    `<workspace_dir>/<slug>.c` when a workspace dir is bound, else
    `<slug>.c` in cwd; set the export scene-name hint and call
    `repl_export_save_output(path, source_document_view(), layout)` —
    same idiom as `repl_save_workspace` (`scenes.c:485-491`).
  - Else (example / transient / unnamed): fall back to
    `repl_save_default_output(layout)` (`output.c`).
  - **[P2] Own status + dir creation.** `repl_export_save_output`
    hardcodes its success/failure status to "…output.c" regardless of
    `filename` (`export.c:3262/3282`); `repl_save_workspace` masks
    this by overwriting the status afterward. `repl_save_active_scene`
    must do the same: `mkdir -p` the workspace dir before writing
    (as `repl_save_workspace` does) and set its own final status —
    e.g. `"Saved <path> (<n> commands)"` / `"Error: cannot write
    <path>"` — so the message names the real file, not `output.c`.
  - Confirm the bound-workspace-dir accessor at implementation
    (the dir set by `repl_load_workspace`); add a narrow getter if
    none is exposed.
- File `Save Scene` dispatch calls it via
  `glr_ctrl_fill_export_layout(&layout)` then
  `repl_save_active_scene(&layout)`.
- `repl_save_default_output` / Ctrl+S path untouched.

## Files

- `src/app/glr_actions.h` — `GLR_SCENE_OFF_*` collapse; `REPL_FILE_ITEM_*`
  + explicit `SCENE_SEP` + New/Save/Rename Scene
- `src/ui/menu_bar.c` — SCENE/FILE `menu_item_count/label/shortcut`,
  the layout comment block
- `src/app/glr_actions.c` — move NEW/SAVE/RENAME dispatch SCENE→FILE,
  rework scene-idx math
- `src/repl/scenes.c`, `src/repl/core.h` — `repl_save_active_scene`
  (+ workspace-dir getter if needed)
- `tests/test_ui_menu_bar.c` — menu label/count/shortcut + dispatch
  assertions for the new layout (the heaviest test churn)
- new focused test for `repl_save_active_scene` filename derivation
  (named scene → slug; unnamed → output.c; workspace vs cwd)
- `CLAUDE.md` / `MODULES.md` if any documented menu structure changes

## Edge cases

Scene with no user scenes → Scene menu shows `### EXAMPLES` + examples
+ `### YOUR SCENES` with no rows under it. **Decided (decision #2):
both headers always present (fixed layout); not suppressed** — keeps
`menu_item_count` / scene-idx math / tests single-path, and matches
today's behavior (the current `### SCENE` header already shows with
zero user scenes). Rename with no active user scene → existing "No
active scene to rename" guard, now under File.
Save Scene while an example is active (no user scene) → `output.c`
fallback. Slug collision (two scenes same name) → `scene_filename_slug`
behavior is reused as-is (workspace save already has this; same
semantics). Example-metadata leading `@cfg` parsing is unaffected
(Scene menu data only, not examples list source).

## Verification

`make sample`, `make test`, `make test-stubs`,
`make check-state-ownership`, UI boundary guards. Manual: open Scene
menu → two clearly-labelled sections, no action rows; open File menu →
New/Save/Rename Scene present and working; Ctrl+S still exports
`output.c`; "Save Scene" on a named user scene writes
`<workspace>/<slug>.c` (or cwd), reload round-trips; example active →
Save Scene falls back to `output.c`; F12 + scene tab clicks still
select correctly (dense-index unchanged).

## Review corrections (incorporated)

Four findings verified against source and folded in:

- **[P1] File `---` needs its own index.** Every visible row is an
  item index (Scene/Tutorials `---` already are). §A now specifies an
  explicit `REPL_FILE_ITEM_SCENE_SEP` slot instead of "auto-grow after
  four", and the non-actionable separator has no dispatch case.
- **[P1] New Scene must not be verbatim.** The old body left the
  active user slot attached; with Save Scene keyed off
  `repl_active_user_scene()` it would overwrite the old `<slug>.c`.
  §B/§Locked now require the transient lifecycle
  (`repl_scenes_enter_transient_scene` + `_reset_for_transient`) +
  `editor_undo_clear()` so no slot stays active.
- **[P2] `repl_save_active_scene` status/dir.**
  `repl_export_save_output` hardcodes "…output.c" status; §C now
  requires `mkdir -p` of the workspace dir and an own success/failure
  status naming the real path (mirrors `repl_save_workspace`).
- **[P2] Empty `YOUR SCENES` decided.** Both headers always present
  (decision #2 + Edge cases) — fixed layout, single-path count /
  dispatch / tests, matches current behavior.

## Sequencing

This lands first; `scene-close-capability.md` is sequenced after it
(Close slots into this File-menu layout once its semantic questions
are answered).
