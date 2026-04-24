# Multi User Scenes

Expand the single-slot user scene system into a multi-slot workspace, with
editable names, auto-promotion from examples, folder-based workspace
import/export, and a fix for the Scene menu highlighting bug.

## Goals

1. Support up to **8 user scenes** in memory simultaneously.
2. Each scene has an **editable name**.
3. **Editing a loaded example auto-promotes** it to a user scene, inheriting
   the example's name.
4. **File → Save** still exports a single scene as one `.c` file (today's
   behavior). **File → Save Workspace** exports every user scene into a
   folder as `workspace/<scene-name>.c`.
5. **File → Load** detects path type: a file imports into the active slot
   (today's behavior); a directory imports every `.c` in it as a separate
   scene.
6. Fix the **Scene menu bug** where the active user scene disappears from
   the menu after selection - it should stay visible and render in the
   accent color, the same way the selected example does.
7. **Workspace folder is renamable** (path is part of workspace state, not
   hard-coded).

## Deferred (do not implement in this pass)

### Config desired-vs-inherited

Today `save_user_scene()` does not preserve scene-local `@cfg` settings;
they bleed over from whichever example was last loaded. There is no way to
distinguish "this scene *desires* grid theme = 3" from "this scene just
happened to inherit grid theme = 3 from the example that spawned it".

**Plan when revisited:** add a `cfg_snapshot[]` per scene taken at
promotion time, then on save diff against the snapshot - only diffs are
emitted to `@cfg`. On load, only emitted `@cfg` lines override current
runtime values; unset items inherit. This keeps the blast radius small
(no per-read consultation of the active scene) and matches how examples
already behave implicitly.

Tracking item; not part of this feature branch.

## Data model

```c
#define MAX_USER_SCENES 8
#define USER_SCENE_NAME_MAX 64

typedef struct {
    int     used;                         // slot occupied
    char    name[USER_SCENE_NAME_MAX];    // editable; unique across slots
    GLCmd   cmds[MAX_COMMANDS];
    int     num_cmds;
    int     edit_line;
    float   predef_vals[MAX_PREDEF_VARS];
    uint32_t last_touch;                  // LRU tick
} UserScene;

static UserScene g_user_scenes[MAX_USER_SCENES];
static int       g_active_user_scene = -1;   // -1 = none active (viewing example)
static uint32_t  g_user_scene_tick = 0;       // monotonic, bumped on touch
static char      g_workspace_dir[PATH_MAX];   // "" until set
```

Replace the existing single `UserScene` global (in `repl_core.c`) with
this array. `repl_user_scene_valid()` becomes "any slot used"; the old
single-slot API (`repl_load_user_scene`) becomes a thin wrapper around
`repl_load_user_scene_idx(int)`.

## Storage & overflow (8 slots)

- **Slot 0 is the "home" scene**: captured the first time the user loads
  an example from a fresh session (exactly today's `save_user_scene()`
  trigger, just written into slot 0 instead of the singleton). Never
  evicted. Renamable like any other scene, but the slot index is
  reserved.
- **Active scene**: the one currently loaded into `g_cmds[]`. Never
  evicted.
- **Eviction policy**: when a 9th scene needs to exist (new promotion
  from example edit, or workspace import), scan slots 1..7 excluding
  the active slot, pick the smallest `last_touch`, flush to
  `<workspace_dir>/<name>.c`, then reuse the slot.
- **No workspace dir set**: block the 9th scene with a status message
  like `"Set workspace folder to add more scenes."` until the user
  issues File → Save Workspace (which sets `g_workspace_dir`).
- Every access (load, save, rename, mutate) bumps
  `g_user_scene_tick` and writes it to the active slot's `last_touch`.

## Auto-promotion on example edit

Today: `save_user_scene()` runs once before the first example load, then
examples replace `g_cmds[]` freely.

New: when an example is active (`g_active_example >= 0`) and the user
makes any mutation (anything that would call `push_undo_snapshot`
during example viewing), promote:

1. Allocate a user scene slot (LRU evict if full - see above).
2. Copy current `g_cmds[]`, `edit_line`, `predef_vals[]` into the slot.
3. Set `name` to the example's name, de-duplicated with ` (2)`, ` (3)`,
   … if the name is already taken by another user-scene slot.
4. Set `g_active_user_scene = slot`; clear `g_active_example`.
5. Apply the user's pending mutation normally.

Home-scene capture (slot 0) still runs on first-ever example load;
promotion is distinct from home capture.

## Editable scene names

- Scene names are stored per slot and shown in:
  - Scene menu (Scene header button dropdown / right-side pin)
  - Examples dropdown in `ui_panels.c` (after predefined examples)
- Rename UI: click on the active scene's name (or a dedicated rename
  action in the Scene menu) to enter an inline-edit state
  (`g_renaming_scene = 1`, typed chars go into the slot's `name`
  buffer, Enter commits, Esc cancels). Reuse the same minimal text
  entry used elsewhere - don't introduce a full text widget.
- Validation on commit: trim, reject empty, de-duplicate with ` (n)`
  suffix against sibling slots.

## Scene menu bug fix

Symptom: after selecting "Your Scene" from the Scene menu, the entry
disappears; loading an example brings it back.

Likely causes to probe in `ui_panels.c` (example dropdown builder) and
`repl_core.c`:
- `repl_user_scene_valid()` flips false when the scene becomes active
  (the singleton gets emptied on load).
- Dropdown builder intentionally skips the active entry.

Fix in the multi-scene world: **always list every occupied slot**, and
render the active slot (whether a user scene or an example) in the
accent color - same treatment examples already get. No special case
for "hide active user scene."

## Save / Load

### Single file (today's path, preserved)

- **File → Save** / `Ctrl+S` → writes the active scene to its existing
  path (or prompts for one). Writes scene name into the header as
  `// @scene-name <name>` so single-file round-trip keeps the name.
- **File → Load** when given a file path → clears active slot (or
  promotes, if editing an example), `feed_line()` through the file.
  Reads `@scene-name` if present.

### Workspace folder

- **File → Save Workspace** → prompts for or reuses `g_workspace_dir`
  (default suggestion `./workspace`). Writes each occupied slot as
  `<workspace_dir>/<sanitized-name>.c`. Scenes evicted earlier in the
  session are already on disk; re-save will overwrite.
- **File → Load Workspace** / passing a dir on the CLI → scans for
  `*.c`, imports each into a fresh slot (using filename stem as the
  scene name if `@scene-name` is absent). Overflow uses the same LRU
  eviction.

### CLI

`./sample path/to/file.c` - today's behavior.
`./sample path/to/workspace/` - new: load entire workspace, set
`g_workspace_dir` to that path, activate slot 0.

## File layout impact

- `repl_core.c` - replace singleton `UserScene` with array + active
  index; update `save_user_scene`, `restore_user_scene`,
  `repl_user_scene_valid`, `repl_load_user_scene`.
- `repl_core.h` - extend public API: `repl_user_scene_count`,
  `repl_user_scene_name(int)`, `repl_user_scene_rename(int, const char*)`,
  `repl_load_user_scene_idx(int)`, `repl_active_user_scene(void)`.
- `repl_export.c` - extend `save_output` to accept a scene slot
  argument; add `save_workspace(dir)` and `load_workspace(dir)`. Teach
  the importer about `@scene-name`.
- `repl_editor.c` - promotion trigger on first mutation-of-example;
  inline rename state; Scene menu entries for rename / save workspace /
  load workspace.
- `ui_panels.c` - multi-scene dropdown rendering; fix active-scene
  highlight (accent color, no hiding); inline rename input.
- `sample.h` - `UserScene` struct, `MAX_USER_SCENES`, new externs.

## Open questions

- **Slot 0 "home" lifecycle**: if the user explicitly renames and edits
  slot 0, it stops being "the pre-example state". Do we still pin it
  from eviction? Suggest: yes - slot 0 is pinned by index, not by
  content. User can always manually clear it.
- **Workspace path persistence across sessions**: save
  `g_workspace_dir` into the single-file export header, or a separate
  dotfile? Lean toward: write into every `.c` the REPL exports as
  `// @workspace <path>` so loading any scene restores the dir.
- **Name collisions on workspace import**: if two files resolve to the
  same scene name, append ` (2)` etc. Same rule as auto-promotion.
- **Inline rename UX**: acceptable character set? Suggest ASCII
  printable minus path-unsafe (`/`, `\`, `:`), since names become
  filenames under workspace export.

## Execution order (suggested)

1. Data model + singleton → array migration, with slot 0 always
   populated. All existing tests pass.
2. Scene menu bug fix (small, landable independently once array exists).
3. Auto-promotion on example edit + inline rename UI.
4. Workspace export/import.
5. Overflow + LRU eviction.
6. CLI directory support.

Deferred: config desired-vs-inherited (see top of doc).
