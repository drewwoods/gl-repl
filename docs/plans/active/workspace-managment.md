# Workspace management + fixing single-file exports in the packaged macOS app

## Context

Running `gl-repl.app` from Finder, only Save/Load Workspace works, and it is clunky:

1. **One hard-coded workspace.** `glr_paths_default_workspace_dir()`
   (`src/app/glr_paths.c:111`) returns `./workspace` when the cwd is writable and
   `~/Library/Application Support/gl-repl/workspace` otherwise. There is no way to
   create a second workspace, pick between workspaces, or delete a scene from one
   (no delete-scene function exists anywhere in the codebase — verified by grep).
   Removing a scene's slot without unlinking its `.c` would be pointless anyway:
   `repl_load_workspace()` re-imports every `*.c` in the directory.

2. **Single-file exports silently write to an unwritable cwd.**
   `format_scene_path()` (`src/repl/scenes.c:449`) returns a bare `output.<ext>`
   whenever there is no active *named* user scene — i.e. for every example or
   transient document — ignoring the workspace directory entirely. Finder launches
   the app with cwd `/`, so `fopen` fails and nothing appears. Three further leaks
   of the same bug:
   - `repl_save_active_scene()` (`scenes.c:469`) shortcuts to
     `repl_save_default_output()` → hard-coded `"output.c"` (`export.c:473`).
   - `bind_app_workspace_for_scene_save_if_needed()` (`glr_actions.c:117`) returns
     early when `repl_active_user_scene() < 0`, so transient docs never bind a
     workspace at all.
   - Ctrl+S (`glr_ctrl_router_handle_save_key`, `glr_ctrl_router.c:103`) never
     calls that bind helper, so Ctrl+S and File → Save Scene already disagree.
   - `glr_ctrl_save_recovery_file()` (`glr_ctrl_router.c:151`) writes
     `QUIT_RECOVERY_FILE` = `"recovery.c"` relative to the cwd with **no fallback**,
     so the quit safeguard is dead in a bundle.

Outcome: multiple named workspaces created and picked from the menu, scenes
deletable, and every write landing in a directory that exists and is writable —
with a Reveal in Finder action so the user can actually find the file.

Decisions taken (from user): no back-compat for the current
`Application Support/gl-repl/workspace` layout (unreleased); both **New Workspace**
and **Save Workspace As**; delete removes slot **and** file behind a confirm;
exports fall back to the bound/default workspace plus Reveal in Finder.

---

## Part 1 — Path policy: a workspaces root

`src/app/glr_paths.{c,h}`, `config.h`

- `config.h`: replace `GLR_DEFAULT_WORKSPACE_DIR "./workspace"` with
  `GLR_WORKSPACES_ROOT_DIR "./workspaces"` + `GLR_DEFAULT_WORKSPACE_NAME "default"`.
  Only 4 non-test references exist (`glr_paths.c:115,118`,
  `tests/test_glr_actions.c:101`), so this is a contained rename.
- New API, following the existing `user_data_subdir()` shape:
  - `int glr_paths_workspaces_root(char *buf, size_t buflen)` — `./workspaces` when
    `glr_paths_cwd_supports_relative_saves()`, else `<user-data>/workspaces`.
  - `const char *glr_paths_default_workspace_dir(void)` — now `<root>/default`
    (same static-buffer contract as today).
  - `int glr_paths_workspace_dir_for_name(const char *name, char *buf, size_t)` —
    `<root>/<slug>`; a `name` containing `/` is taken as a literal path so a typed
    path still works. Slug via the existing `workspace_io_filename_slug()`
    (`src/repl/workspace_io.c:78`).
  - `int glr_paths_resolve_output_path(const char *leaf, char *buf, size_t)` —
    `<bound workspace>/<leaf>`, else `<default workspace>/<leaf>`, else `leaf`
    verbatim (dev cwd). Used for `output.<ext>` and `recovery.c`.
- Reuse `glr_paths_ensure_dir(path, &created)` (`glr_paths.c:31`) — do **not** add
  a third mkdir-p (`workspace_io_ensure_dir` at `workspace_io.c:33` is already a
  near-duplicate of it; leave that alone, out of scope).

## Part 2 — Workspace enumeration (new module)

`src/app/glr_workspaces.{c,h}` — modelled directly on the audio playlist scanner
(`scan_dir_into()` at `src/app/glr_audio.c:101`: `opendir`/`readdir`, skip
dotfiles, `qsort` for stable order, fixed-cap arrays). Filter with a `stat` +
`S_ISDIR` helper (`path_is_dir` at `glr_paths.c:15` — export it or duplicate the
three lines; `d_type` is not portable).

```c
void        glr_workspaces_refresh(void);            /* rescan <root> */
int         glr_workspaces_count(void);
const char *glr_workspaces_name(int idx);            /* display name */
const char *glr_workspaces_path(int idx);            /* full path */
int         glr_workspaces_active_index(void);       /* == repl_workspace_dir(), else -1 */
int         glr_workspaces_create(const char *name, char *out_path, size_t sz);
```

Caps mirror audio (`GLR_WORKSPACES_MAX 64`, `GLR_PATH_MAX` per entry). Refresh is
explicit — never per-frame, because `menu_item_count()` runs every frame while the
dropdown is open. Call it from: boot (once, alongside the workspaces-root
`ensure_dir` + `created` hint, same pattern as `glr_audio.c:167-173`),
`route_menu_button_hit()` (`glr_ctrl_router.c:1503`) when the opened menu is
`GLR_MENU_FILE`, and after every create/save-as/load/delete.

## Part 3 — App-layer modal (name prompt + confirm)

`src/app/glr_modal.{c,h}` — one module, not two more copies of the bespoke
inline-modal pattern. It lives in `src/app/` rather than `src/editor/` because its
commits call `glr_paths_*` / `glr_workspaces_*`, and `src/editor/` must not depend
on the app layer.

Shape copied from `src/editor/inline_file_prompt.c` (statics + buffer + error
string + `_active/_begin/_handle_key/_handle_special/_cancel/_buffer`):

```c
typedef enum { GLR_MODAL_NONE = 0, GLR_MODAL_WORKSPACE_NEW,
               GLR_MODAL_WORKSPACE_SAVE_AS, GLR_MODAL_WORKSPACE_LOAD_PATH,
               GLR_MODAL_CONFIRM_DELETE_SCENE } GlrModalKind;
```

- Text kinds: char filter = `prompt_char_ok`'s policy from
  `inline_file_prompt.c:88` (allow `.` and `/`, reject quotes/shell metachars).
  Enter commits, Esc cancels, a failed commit keeps the strip open with an
  in-strip error (`g_prompt_err` pattern) — the strip occludes the status bar, so
  `repl_set_status` alone would be invisible.
- `GLR_MODAL_CONFIRM_DELETE_SCENE`: no buffer; `Y`/`y` commits, everything else
  except Esc is swallowed. The composed question ("Delete scene 'torus test' and
  torus_test.c?") is stored in the buffer field at `_begin` time; `panels.c`
  appends the key hints.

**Mutual exclusion.** `glr_modal_begin_*` cancels both editor inline modals
(`editor_inline_rename_cancel`, `editor_inline_file_prompt_cancel`). The reverse
direction cannot live in `src/editor/`, so add `glr_modal_cancel()` at the three
app-layer sites that start those modals: the Rename Scene and Load Scene File-menu
cases (`glr_actions.c:1405,1416`), the scene-tab double-click rename
(`glr_ctrl_router.c:1606`), and the existing click-outside cancel
(`glr_ctrl_router.c:1738-1741`). Also cancel in `glr_ctrl_reset_all`
(next to `glr_ctrl.c:3288-3289`).

**Key routing.** Add `glr_modal_handle_key` / `_handle_special` at the *head* of
the fixed sandwich in `keyboard_dispatch()` (`glr_ctrl_router.c:2012`) and
`special_dispatch()` (`:2074`), before the two existing modal captures.

**Render.** Three new snapshot fields next to the existing modal ones
(`src/ui/app/snapshot.h:158-175`): `modal_kind`, `modal_text[256]`,
`modal_error[192]`; filled beside `glr_ctrl.c:1965-1973`. New branch in
`ui_panels_render_scene_status()` (`src/ui/app/panels.c:610`) reusing
`draw_modal_strip()` (`panels.c:98`) and switching on `modal_kind` for the
label/verb, e.g. `"New workspace: %s_   [Enter] create   [Esc] cancel"`,
`"%s   [Y] delete   [Esc] cancel"`. Also add the kind to the two "a modal owns the
bottom band" guards at `panels.c:163` and `panels.c:682-687`.

## Part 4 — Delete scene

- `src/repl/scenes.c`: refactor `format_scene_path()` to take an explicit slot
  (`format_scene_path_for_slot`), keeping `repl_active_scene_export_path()` as a
  thin wrapper so the "one source of truth for export naming" comment at
  `scenes.c:442-448` stays true. Export
  `const char *repl_user_scene_file_path(int slot, const char *ext)` so the caller
  can resolve the filename *before* the slot is freed.
- New `int repl_user_scene_delete(int slot)`: clear `g_user_scenes[slot].used`, and
  if it was the active slot leave `g_active_user_scene = -1` (the caller re-lands).
- New `glr_action_delete_active_scene()` in `glr_actions.c`: guard
  `repl_active_user_scene() < 0` → `repl_set_status_error("No active scene to
  delete")`; else `glr_modal_begin_confirm_delete_scene(slot)`. The confirm commit
  resolves the path, calls `repl_user_scene_delete`, `unlink()`s the `.c` (only the
  `.c` — `.glr`/`.ply` are user exports, not workspace state), then
  `repl_scenes_activate_first_loaded_slot()` (`scenes.c:661`) +
  `editor_undo_note_wholesale_replacement()` (mandatory: a wholesale document
  replacement must clear the undo ring), and reports
  `"Deleted <name> (<path>)"`. No keymap binding — menu-only, like Export .ply.

## Part 5 — File menu rows + a File flyout

`src/app/glr_actions.h` (enum), `src/app/glr_actions.c` (dispatch),
`src/ui/app/menu_bar.c` (labels + provider), `src/app/glr_ctrl_router.c` (routing)

New layout (three new rows + one reordered separator):

```
New Scene / Save Scene (Ctrl+S) / Save Scene as .glr / Load Scene /
Load Scene from Clipboard / Rename Scene / Delete Scene* / Export .ply /
Split Declaration (Ctrl+Shift+Q) / Reveal in Finder* / --- /
New Workspace...* / Save Workspace / Save Workspace As...* /
Load Workspace > / --- / Quit (Ctrl+Q)          (* = new)
```

- Labels go in the `menu_item_label()` File table (`menu_bar.c:427-440`).
- **File gets its first flyout.** Add `kFileProvider` + a
  `flyout_provider_for(MENU_FILE)` branch (`menu_bar.c:994`). `row_count` returns
  `glr_workspaces_count() + 1` only for `parent_row == GLR_FILE_ITEM_LOAD_WORKSPACE`
  (0 otherwise); the trailing row is `"Other folder…"` → the
  `GLR_MODAL_WORKSPACE_LOAD_PATH` prompt, so a workspace outside the root is still
  reachable. `menu_row_has_submenu()` (`:1068`) then grows the `>` affordance and
  hover-open for free, and long lists already scroll
  (`ui_menu_bar_handle_wheel_scroll`, `:1597`).
- `submenu_row_is_active()` (`:1697`): add a `MENU_FILE` case →
  `ordinal == glr_workspaces_active_index()` so the bound workspace is tinted.
- **`route_submenu_item_hit()` (`glr_ctrl_router.c:1547`) needs a `GLR_MENU_FILE`
  branch placed before the final fallthrough** — that fallthrough is
  `glr_scene_load_example(item_idx)`, so a missing branch silently loads examples.
- The `Load Workspace` parent row becomes **inert on click** (return 0, hover-open
  only), matching every other parent/tag row in the engine; the bound workspace is
  reachable as the highlighted flyout entry.
- `New Workspace...` = create + bind + `repl_scenes_reset()` + undo clear +
  `glr_camera_clear_scene_default()`. `Save Workspace As...` = create + bind +
  `repl_save_workspace(dir, &layout)` (reuses `scenes.c:376` unchanged; it already
  binds the dir and restores the previous one on failure).
- Load from the flyout reuses the existing `GLR_FILE_ITEM_LOAD_WORKSPACE` body
  verbatim (`glr_actions.c:1433-1451` — recovery save, camera clear,
  `repl_load_workspace`, undo clear, `repl_scenes_activate_first_loaded_slot`),
  factored into `glr_action_load_workspace_dir(const char *dir)`.

## Part 6 — Make every write land somewhere writable

- `format_scene_path_for_slot()` (`scenes.c:449`): the no-named-scene arm becomes
  `<workspace_dir>/output.<ext>` instead of bare `output.<ext>`. **This one change
  is the fix for the missing `.c` / `.glr` / `.ply`.**
- `repl_save_active_scene()` (`scenes.c:464`): route the transient/example case
  through the same dir logic instead of shortcutting to `repl_save_default_output`
  (keep that function for the no-workspace-bound case and its tests).
- `bind_app_workspace_for_scene_save_if_needed()` (`glr_actions.c:117`): drop the
  `repl_active_user_scene() < 0` early return.
- Kill the Ctrl+S / menu drift: extract `glr_action_save_active_scene()` (bind +
  `glr_ctrl_fill_export_layout` + `repl_save_active_scene`) and call it from both
  `GLR_FILE_ITEM_SAVE_SCENE` and `glr_ctrl_router_handle_save_key()`.
- `glr_ctrl_save_recovery_file()` (`glr_ctrl_router.c:151`): resolve
  `QUIT_RECOVERY_FILE` through `glr_paths_resolve_output_path()` and print the
  resolved path in the quit hint (`glr_ctrl_router.c:160`). Update the block
  comment at `:132-139` — the workspace dir satisfies its "must be findable, must
  not be /tmp" intent.
- **Reveal in Finder**: `static char g_last_output_path[GLR_PATH_MAX]` in
  `glr_actions.c` + `glr_action_note_output_path()`, set by Save Scene, Save .glr,
  Export .ply, Save Workspace (the dir) and the workspace create paths; the menu
  item falls back to the bound/default workspace dir when nothing has been written
  yet. Implement with `fork()` + `execv("/usr/bin/open", {"open","-R",path})` —
  **not** `popen`, because the default path contains a space
  (`Application Support`) and shell quoting is a live injection/breakage hazard.
  Non-Apple builds report `"Reveal in Finder is macOS-only"`, mirroring the
  clipboard item's `#else` at `glr_actions.c:223-228`. (The File menu is already
  hidden under Emscripten — `menu_visible()`, `menu_bar.c:53`.)

---

## Verification

Automated:
- `make test` — plus the existing tests that hard-code File-menu indices, labels
  and workspace paths and **will** fail until updated:
  `tests/test_ui_menu_bar.c` (`:191`, `:1307`), `tests/test_glr_actions.c`
  (`:73-105`, `:305-348`), `tests/test_scene_file_menu.c` (whole file),
  `tests/test_repl_core_extra.c:1102-1360` (inline-modal flows),
  `tests/test_ui_scene_tabs.c`, `tests/test_repl_state.c:439-500`.
- Extend `test_app_save_falls_back_to_user_workspace()`
  (`tests/test_scene_file_menu.c:317-406` — sets `HOME` to a temp dir and
  `chdir`s into a `0555` dir) to assert that with a **transient/example** document
  Save Scene, Save .glr and Export .ply all produce
  `<user workspace>/output.{c,glr,ply}`, and that Ctrl+S and the menu path agree.
- New `tests/test_glr_workspaces.c`: create two workspaces under a temp `HOME`,
  assert `glr_workspaces_count/name/path` sorted and `active_index` tracking;
  New Workspace clears slots and binds; Save Workspace As writes N files;
  delete-scene unlinks the `.c` and the scene does not come back after a reload.
- New modal key-flow tests (begin → type → Enter → dir exists + bound; Esc;
  empty-name error keeps the strip open; confirm `Y` vs Esc), and a File-flyout
  test (`row_count`/`row_label`/`row_abs_index` + `route_submenu_item_hit` for
  `GLR_MENU_FILE`).
- `make check-state-ownership`, `make check-c99`, `make test-stubs`. New TUs are
  picked up automatically by the `src/app/*.c` and `src/ui/app/*.c` wildcards
  (`Makefile:428,438`).

Manual (the actual bug report), with native GL — not OSMesa:
1. `make app && open gl-repl.app` (Finder launch ⇒ cwd `/`).
2. File → New Workspace… → `demo`; confirm the status line shows
   `~/Library/Application Support/gl-repl/workspaces/demo`.
3. Type a scene, Ctrl+S, then File → Reveal in Finder — the `.c` must be there.
4. Load an example (F12), then Save Scene / Save Scene as .glr / Export .ply —
   all three must land as `demo/output.{c,glr,ply}` and Reveal must find them.
5. File → New Workspace… → `other`; File → Load Workspace ▸ must list `demo` and
   `other` with `other` highlighted; switch back to `demo` and confirm the scenes
   return.
6. File → Delete Scene → `Y`; confirm the tab disappears, the `.c` is gone from
   Finder, and Load Workspace ▸ demo does not resurrect it.
7. Ctrl+Q; confirm `recovery.c` is written into the workspace, and
   `./gl-repl "<path>/recovery.c"` reloads it.

Docs to update in the same change: `docs/USER_GUIDE.md:1661-1693` (Scenes &
Workspaces), `docs/ADVANCED_USAGE.md:11-25`, `docs/MODULES.md` (rows for
`glr_workspaces` / `glr_modal`, and the changed workspace path policy),
`CLAUDE.md` + `AGENTS.md:371-380` (workspace/auto-promotion section), then
`make fix-doc-links` to repair line-number drift.
