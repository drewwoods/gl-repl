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

Decisions taken: no back-compat for the current
`Application Support/gl-repl/workspace` layout (unreleased); both **New Workspace**
and **Save Workspace As**; delete removes slot **and** file behind a confirm;
exports fall back to the bound/default workspace plus Reveal in Finder.

### The constraint that shapes Parts 3 and 6

`repl_load_workspace()` imports **every** `*.c` in the directory
(`scenes.c:629-638`, filtered only by `workspace_io_has_c_ext`). So any app-owned
`.c` the plan writes *into* a workspace comes back as a phantom scene on the next
load, eating one of the 8 `MAX_USER_SCENES` slots. Naively routing `output.c` and
`recovery.c` into the bound workspace would mean every quit-and-reload grows the
workspace by one junk scene. Two rules follow, and they are load-bearing:

- **`recovery.c` is app state, not workspace state** → it goes in the workspaces
  *root*, never inside a workspace.
- **`output.c` is designed out**, not relocated: saving a transient/example
  document prompts for a scene name and promotes it to a real named slot. Only
  `output.glr` / `output.ply` keep the generic name, and the loader ignores both
  (`workspace_io_has_c_ext` accepts `.c` only).

### One thing deliberately *not* done: binding a workspace at boot

Eagerly binding a default workspace at startup would make Parts 4-6 shorter, but
`emit_workspace_dir()` (`src/repl/export_setup.c:123-131`) writes
`/* @workspace-dir <dir> */` into every export whenever `repl_workspace_dir()` is
non-empty. Binding at boot would therefore stamp the user's absolute home path
into every saved scene and add a header line to all the UI goldens
(`tests/testdata/repl_examples_ui/*.golden.txt` — verified: none contains
`@workspace-dir` today). The workspace stays unbound until the user acts.

---

## Part 0 — Housekeeping

This file was `workspace-managment.md`; renamed to `workspace-management.md`
(already done via `git mv`). Add the row to `docs/plans/active/README.md`'s table,
which currently lists only `state-ownership-finalize.md`.

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
    verbatim (dev cwd). Used for `output.glr` / `output.ply`.
  - `int glr_paths_app_state_path(const char *leaf, char *buf, size_t)` —
    `<workspaces root>/<leaf>`, **never** a workspace. Used for `recovery.c`.
  - `int glr_paths_same_dir(const char *a, const char *b)` — `realpath()` both
    sides (falling back to string compare when a path does not exist yet) so
    `./workspaces/x`, `workspaces/x` and the absolute form all match. Needed by
    `glr_workspaces_active_index()`; without it the active-workspace highlight
    silently never matches in dev-cwd runs.
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
int         glr_workspaces_active_index(void);       /* glr_paths_same_dir vs repl_workspace_dir(), else -1 */
int         glr_workspaces_create(const char *name, char *out_path, size_t sz,
                                  char *err, size_t err_sz);
```

Caps mirror audio (`GLR_WORKSPACES_MAX 64`, `GLR_PATH_MAX` per entry). Refresh is
explicit — never per-frame, because `menu_item_count()` runs every frame while the
dropdown is open. Call it from: boot (once, alongside the workspaces-root
`ensure_dir` + `created` hint, same pattern as `glr_audio.c:167-173`),
`route_menu_button_hit()` (`glr_ctrl_router.c:1503`) when the opened menu is
`GLR_MENU_FILE`, and after every create/save-as/load/delete.

`glr_workspaces_create()` is the one place that must not be permissive —
`glr_paths_ensure_dir` treats `EEXIST` as success, and `repl_save_workspace()`
never deletes stale `.c` files, so a silent bind onto an existing directory would
merge that directory's orphan scenes into the new layout. Two rejections, each
reported in-strip so the prompt stays open (see Part 3):

| Input | Result |
|---|---|
| `demo` when `<root>/demo` exists | `"Workspace 'demo' already exists"` |
| name whose slug is empty (`"!!!"`, `"   "`) | `"Workspace name needs a letter or digit"` |

## Part 3 — App-layer modal (name prompts + confirm)

`src/app/glr_modal.{c,h}` — one module, not four more copies of the bespoke
inline-modal pattern. It lives in `src/app/` rather than `src/editor/` because its
commits call `glr_paths_*` / `glr_workspaces_*`, and `src/editor/` must not depend
on the app layer.

Shape copied from `src/editor/inline_file_prompt.c` (statics + buffer + error
string + `_active/_begin/_handle_key/_handle_special/_cancel/_buffer`):

```c
typedef enum { GLR_MODAL_NONE = 0, GLR_MODAL_WORKSPACE_NEW,
               GLR_MODAL_WORKSPACE_SAVE_AS, GLR_MODAL_WORKSPACE_LOAD_PATH,
               GLR_MODAL_SCENE_SAVE_AS, GLR_MODAL_CONFIRM_DELETE_SCENE } GlrModalKind;
```

- Text kinds: char filter = `prompt_char_ok`'s policy from
  `inline_file_prompt.c:88` (allow `.` and `/`, reject quotes/shell metachars) for
  the workspace kinds; `GLR_MODAL_SCENE_SAVE_AS` uses `rename_char_ok`'s stricter
  policy (`inline_rename.c:64`, rejects `/ \ :`) since the name becomes a scene
  slug. Enter commits, Esc cancels, a failed commit keeps the strip open with an
  in-strip error (`g_prompt_err` pattern) — the strip occludes the status bar, so
  `repl_set_status` alone would be invisible.
- `GLR_MODAL_CONFIRM_DELETE_SCENE`: no buffer; `Y`/`y` commits, everything else
  except Esc is swallowed. The composed question ("Delete scene 'torus test' and
  torus_test.c?") is stored in the buffer field at `_begin` time; `panels.c`
  appends the key hints.

**`GLR_MODAL_SCENE_SAVE_AS` is what kills `output.c`.** Save Scene on a
transient/example document prompts for a name, then promotes the document to a
real named slot and saves `<workspace>/<slug>.c`. That is the same transition
`repl_promote_transient_if_needed()` (`scenes.c:1061`) already performs on edit, so
reuse its slot-reservation path rather than hand-rolling one, then
`repl_user_scene_rename(slot, typed_name)` + `glr_action_save_active_scene()`.
Errors to surface in-strip: no free slot (`"All scene slots full — delete or save
a scene first"`) and duplicate name (`repl_user_scene_rename` already
de-duplicates via `derive_unique_scene_name`, so report the name it actually got).

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
`"Save scene as: %s_   [Enter] save   [Esc] cancel"`,
`"%s   [Y] delete   [Esc] cancel"`. Also add the kind to the two "a modal owns the
bottom band" guards at `panels.c:163` and `panels.c:682-687`.

## Part 4 — Delete scene

- `src/repl/scenes.c`: refactor `format_scene_path()` to take an explicit slot
  (`format_scene_path_for_slot`), keeping `repl_active_scene_export_path()` as a
  thin wrapper so the "one source of truth for export naming" comment at
  `scenes.c:442-448` stays true. Export
  `const char *repl_user_scene_file_path(int slot, const char *ext)` so the caller
  can resolve the filename *before* the slot is freed.
- New `int repl_user_scene_delete(int slot)`: clear `g_user_scenes[slot].used` +
  `scene_cfg_clear(slot)` (mirroring `evict_scene_to_workspace`'s teardown at
  `scenes.c:989-990`), and if it was the active slot leave
  `g_active_user_scene = -1` (the caller re-lands).
- New `glr_action_delete_active_scene()` in `glr_actions.c`: guard
  `repl_active_user_scene() < 0` → `repl_set_status_error("No active scene to
  delete")`; else `glr_modal_begin_confirm_delete_scene(slot)`. The confirm commit:
  1. **Only unlink when a workspace is actually bound.** With no bound workspace
     the slug resolves to a cwd-relative path, and unlinking in an arbitrary cwd is
     exactly the class of bug this plan exists to fix. Unbound → drop the slot only
     and say so (`"Removed scene <name> (no workspace bound — nothing deleted on
     disk)"`).
  2. Resolve the path, `repl_user_scene_delete(slot)`, then `unlink()` and **check
     the result** — report `"Deleted <name> (<path>)"` on success,
     `"Removed <name> — could not delete <path>: <strerror(errno)>"` on failure
     (`ENOENT` reported as "no file on disk", not an error).
  3. Delete only the `.c`. `.glr` / `.ply` are user exports, not workspace state.
  4. `repl_scenes_activate_first_loaded_slot()` (`scenes.c:661`) +
     `editor_undo_note_wholesale_replacement()` — mandatory: a wholesale document
     replacement must clear the undo ring or Ctrl+Z restores the deleted scene into
     whatever is now live.
  5. `glr_workspaces_refresh()`.
- No keymap binding — menu-only, like Export .ply.

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
- Factor the existing `GLR_FILE_ITEM_LOAD_WORKSPACE` body
  (`glr_actions.c:1433-1451` — recovery save, camera clear, `repl_load_workspace`,
  undo clear, `repl_scenes_activate_first_loaded_slot`) into
  `glr_action_load_workspace_dir(const char *dir)`, then **route New Workspace
  through it too**: `glr_workspaces_create()` + `glr_action_load_workspace_dir()`.
  Loading a freshly created empty directory loads zero scenes, so New Workspace
  inherits the recovery save, camera clear and undo clear for free instead of
  discarding the current document with no safety net (the flaw in a
  `repl_scenes_reset()`-based implementation). One addition needed: when
  `repl_load_workspace` returns 0 the pre-load document stays live and tabless, so
  follow up with `repl_scenes_create_empty_user_scene()` to land the user on a
  fresh named slot.
- `Save Workspace As...` = `glr_workspaces_create()` + bind +
  `repl_save_workspace(dir, &layout)` (reuses `scenes.c:376` unchanged; it already
  binds the dir and restores the previous one on failure).

## Part 6 — Make every write land somewhere writable

- `format_scene_path_for_slot()` (`scenes.c:449`): the no-named-scene arm becomes
  `<workspace_dir>/output.<ext>` instead of bare `output.<ext>`. **This is the fix
  for the missing `.glr` / `.ply`.** For `.c`, Part 3's
  `GLR_MODAL_SCENE_SAVE_AS` means this arm is no longer reached from Save Scene at
  all — it survives only as the fallback for `repl_save_default_output()` and its
  tests.
- `repl_save_active_scene()` (`scenes.c:464`): keep it strictly for named slots;
  the transient case is now the app-layer Save-As prompt. `repl_save_default_output`
  stays as-is (tests, dump paths) but is no longer on any interactive path.
- `bind_app_workspace_for_scene_save_if_needed()` (`glr_actions.c:117`): drop the
  `repl_active_user_scene() < 0` early return so a transient doc about to be
  promoted binds the default workspace.
- Kill the Ctrl+S / menu drift: extract `glr_action_save_active_scene()` (bind +
  transient→Save-As prompt + `glr_ctrl_fill_export_layout` +
  `repl_save_active_scene`) and call it from both `GLR_FILE_ITEM_SAVE_SCENE` and
  `glr_ctrl_router_handle_save_key()`.
- `glr_ctrl_save_recovery_file()` (`glr_ctrl_router.c:151`): resolve
  `QUIT_RECOVERY_FILE` through **`glr_paths_app_state_path()`** — the workspaces
  root, not the bound workspace, so `repl_load_workspace` can never slurp it as a
  scene. Print the resolved path in the quit hint (`glr_ctrl_router.c:160`) and
  update the block comment at `:132-139`: the workspaces root satisfies its "must
  be findable, must not be /tmp" intent while staying out of the import glob.
- **Auto-promotion no longer dead-ends.** `reserve_slot_for_promotion()`
  (`scenes.c:1014-1019`) returns -1 when every slot is full *and* no workspace is
  bound, so editing a transient with 8 scenes open just refuses. `src/repl/` cannot
  call `glr_paths_*`, and binding at boot is ruled out (see Context), so add a
  `const char *(*default_workspace_dir)(void)` hook to `ReplHostEffects`
  (`src/repl/host_effects.h:38`) installed by the controller; the reservation binds
  it via `repl_set_workspace_dir()` instead of failing. NULL hook = today's
  behavior, so pure REPL tests and `repl_demo` are unaffected.
- **Reveal in Finder**: `static char g_last_output_path[GLR_PATH_MAX]` in
  `glr_actions.c` + `glr_action_note_output_path()`, set by Save Scene, Save .glr,
  Export .ply, Save Workspace (the dir) and the workspace create paths; the menu
  item falls back to the bound/default workspace dir when nothing has been written
  yet. Implement with `posix_spawn("/usr/bin/open", {"open","-R",path})` +
  `waitpid` — **not** `popen` (the default path contains a space in
  `Application Support`, so shell quoting is a live breakage/injection hazard) and
  **not** bare `fork`+`execv` without a reap (one zombie per reveal). `open -R`
  returns immediately, so the blocking wait is negligible. Non-Apple builds report
  `"Reveal in Finder is macOS-only"`, mirroring the clipboard item's `#else` at
  `glr_actions.c:223-228`. (The File menu is already hidden under Emscripten —
  `menu_visible()`, `menu_bar.c:53`.)

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
  Save Scene opens the Save-As prompt and lands `<workspace>/<slug>.c`, Save .glr
  and Export .ply land `<workspace>/output.{glr,ply}`, and Ctrl+S and the menu path
  agree.
- **Pollution regression test** (the one that would have caught the original
  design flaw): create a workspace, save a scene, write a quit-recovery copy, save
  `.glr` and `.ply`, then `repl_load_workspace()` the same dir and assert the
  loaded scene count is exactly 1 — no `output` and no `recovery` phantom slot.
- New `tests/test_glr_workspaces.c`: create two workspaces under a temp `HOME`,
  assert `glr_workspaces_count/name/path` sorted and `active_index` tracking
  (including a dev-cwd relative-vs-absolute case, which is what
  `glr_paths_same_dir` exists for); create-collision and empty-slug rejections;
  New Workspace writes a recovery copy before discarding and lands on a fresh slot;
  Save Workspace As writes N files; delete-scene unlinks the `.c`, reports
  `unlink` failure, refuses to unlink when unbound, and the scene does not come
  back after a reload.
- New modal key-flow tests (begin → type → Enter → dir exists + bound; Esc;
  empty-name error keeps the strip open; confirm `Y` vs Esc; Save-As on a transient
  produces a named slot), and a File-flyout test (`row_count`/`row_label`/
  `row_abs_index` + `route_submenu_item_hit` for `GLR_MENU_FILE`).
- Auto-promotion: with 8 slots full, no workspace bound and the new host hook
  installed, an edit to a transient promotes instead of refusing.
- `make check-state-ownership`, `make check-c99`, `make test-stubs`. New TUs are
  picked up automatically by the `src/app/*.c` and `src/ui/app/*.c` wildcards
  (`Makefile:428,438`).
- **Goldens**: checked — `tests/testdata/repl_examples_ui/*.golden.txt` hold
  *exported scene text* (`@cfg` block + code), not menu rows, so new File-menu rows
  do not churn them, and no new `GlrConfigKey` is added. The one way this change
  could churn them is a stray `@workspace-dir` header, which is why nothing binds a
  workspace at boot. If one appears, `make rebuild-golden` (`Makefile:2028`, runs
  under `USE_GL_STUBS=1`) regenerates — but treat a diff there as a bug, not churn.

Manual (the actual bug report), with native GL — not OSMesa:
1. `make app && open gl-repl.app` (Finder launch ⇒ cwd `/`).
2. File → New Workspace… → `demo`; confirm the status line shows
   `~/Library/Application Support/gl-repl/workspaces/demo`.
3. Type a scene, Ctrl+S, then File → Reveal in Finder — the `.c` must be there.
4. Load an example (F12), Ctrl+S → the Save-As strip appears; name it
   `from example` and confirm `demo/from_example.c` plus a new named tab. Then
   Save Scene as .glr and Export .ply and Reveal each.
5. File → New Workspace… → `demo` again ⇒ in-strip "already exists" error, prompt
   stays open. Then `other` ⇒ succeeds.
6. File → Load Workspace ▸ must list `demo` and `other` with `other` highlighted;
   switch back to `demo` and confirm the scenes return.
7. File → Delete Scene → `Y`; confirm the tab disappears, the `.c` is gone from
   Finder, and Load Workspace ▸ demo does not resurrect it.
8. Ctrl+Q; confirm `recovery.c` is written to
   `.../gl-repl/workspaces/recovery.c` (the **root**, not inside `demo`), and
   `./gl-repl "<path>/recovery.c"` reloads it. Re-open Load Workspace ▸ demo and
   confirm no `recovery` or `output` scene tab appeared.

Docs to update in the same change: `docs/USER_GUIDE.md:1661-1693` (Scenes &
Workspaces), `docs/ADVANCED_USAGE.md:11-25`, `docs/MODULES.md` (rows for
`glr_workspaces` / `glr_modal`, and the changed workspace path policy),
`CLAUDE.md` + `AGENTS.md:371-380` (workspace/auto-promotion section — the
"promotion is rejected when no workspace is bound" wording changes),
`docs/plans/active/README.md` (Part 0), then `make fix-doc-links` to repair
line-number drift.
