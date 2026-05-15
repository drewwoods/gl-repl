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
   `### YOUR SCENES` + user-scene rows. **No action rows.**
3. File menu gains: **New Scene**, **Save Scene**, **Rename Scene**
   (moved verbatim from the Scene menu; Close is *not* added now).
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
  or compute inline. Extend `REPL_FILE_ITEM_*` with
  `REPL_FILE_ITEM_NEW_SCENE`, `_SAVE_SCENE`, `_RENAME_SCENE`
  (after the existing four; `_COUNT` auto-grows).
- `menu_bar.c` `menu_item_count` / `menu_item_label` /
  `menu_item_shortcut`: SCENE returns
  `1 + e + 1 + repl_user_scene_count()`; labels emit the two new
  headers + rows. FILE returns the new count; labels add
  "New Scene" / "Save Scene" / "Rename Scene" (a `---` separator
  before them keeps Export/Workspace grouped). Keep
  `FILE_ITEM_EXPORT` shortcut `Ctrl+S`.

### B. Dispatch — `src/app/glr_actions.c` (`glr_action_menu_item_activate`)

- `GLR_MENU_SCENE` branch: keep only example load
  (`item_idx 1..e → glr_scene_load_example(item_idx-1)`) and
  user-scene load. Scene-idx math changes from
  `item_idx - (e + GLR_SCENE_OFF_SCENES)` to
  `item_idx - (e + 2)` (after the new `### YOUR SCENES` header).
  Skip the header rows. Delete the NEW/SAVE/RENAME cases here.
- `GLR_MENU_FILE` branch: add `REPL_FILE_ITEM_NEW_SCENE`
  (the old `editor_clear_all_cmds()` + clear-active-example body),
  `_RENAME_SCENE` (active-slot guard → `editor_inline_rename_begin`),
  `_SAVE_SCENE` (new core call, §C). Bodies move verbatim from the
  old Scene cases — no behavior change beyond menu location.
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
  - Confirm the bound-workspace-dir accessor at implementation
    (the dir set by `repl_load_workspace`); add a narrow getter if
    none is exposed.
- File `Save Scene` dispatch calls it via
  `glr_ctrl_fill_export_layout(&layout)` then
  `repl_save_active_scene(&layout)`.
- `repl_save_default_output` / Ctrl+S path untouched.

## Files

- `src/app/glr_actions.h` — `GLR_SCENE_OFF_*` collapse, `REPL_FILE_ITEM_*`
  + New/Save/Rename Scene
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
+ `### YOUR SCENES` with no rows under it (or suppress the empty
header — decide; suppressing is cleaner). Rename with no active user
scene → existing "No active scene to rename" guard, now under File.
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

## Sequencing

This lands first; `scene-close-capability.md` is sequenced after it
(Close slots into this File-menu layout once its semantic questions
are answered).
