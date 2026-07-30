# Managed workspaces + reliable packaged-app saves

## Goal

Make the packaged macOS app's filesystem behavior predictable and make a
workspace a safe, named collection of scenes rather than an implicit glob of a
process working directory.

The finished behavior is:

- packaged-app writes always land under the user's application-data directory;
- development launches keep the existing writable-cwd `output.c`, `recovery.c`,
  and relative scene-save contract;
- Ctrl+S and File -> Save Scene use one action path;
- users can create, save-as, open, reveal, and switch named workspaces;
- managed workspaces have stable scene ordering and filenames;
- rename/delete/save cannot resurrect stale scene files;
- workspace switches and loads are transactional and never discard a second
  in-memory scene merely because only the active document was recovered;
- `.glr`, `.ply`, and recovery output never become phantom workspace scenes.

The current single-directory layout is unreleased, so there is no migration
requirement for `Application Support/gl-repl/workspace`.

## Design decisions

### Managed workspace manifest

A managed workspace contains a `.glr-workspace` manifest. It is a compact,
line-oriented format so the C99 implementation needs no JSON dependency:

```text
version=1
name=Demo
scene=lantern.c
scene=torus.c
```

The manifest is the authoritative ordered set of app-owned scene files.
Unlisted `.c` files are not imported, pruned, renamed, or deleted. This is the
commit record that prevents stale files and unrelated C sources from becoming
phantom scene tabs.

Scene filenames are stable after first assignment. A scene display rename does
not silently recompute a slug or rename the file. The in-memory scene record
retains its persisted filename; only an unpersisted scene, or a collision with
an unrelated file, receives a new collision-free filename.

A folder without `.glr-workspace` is not a workspace. **Other folder...** opens
another managed workspace outside the app-owned root and rejects manifest-less
directories. Standalone `.c` files remain available through Load Scene. There
is deliberately no compatibility path for the old implicit "every `*.c`"
directory format.

### Eight tabs are not the workspace database

`MAX_USER_SCENES` remains the open-tab capacity. The manifest may list more
scenes in a later scene-browser extension, but this change must not silently
load or discard entries beyond the current capacity. Workspace load returns an
explicit capacity error and rolls back. The initial implementation therefore
rejects managed manifests with more than `MAX_USER_SCENES` entries.

### Transaction boundaries

Workspace open is all-or-nothing:

1. flush the live active scene into its in-memory slot;
2. capture the complete scene catalog, live document, binding, and presentation
   state needed by the existing scene snapshot APIs;
3. parse the manifest and every referenced scene into a replacement catalog;
4. on any missing file, parse failure, allocation failure, or capacity overflow,
   restore the old catalog and live document and keep the old workspace bound;
5. only after full success publish the new binding, activate the first scene (or
   create a fresh named scene for a genuinely empty managed workspace), clear
   undo, and clear the camera default.

Workspace save stages each output as a sibling temporary file. The manifest is
written and renamed last, so the old manifest continues to describe a complete
old workspace until every new scene file is ready. Only files named by the
previous manifest may be pruned after the new manifest commits.

Switching away from a bound managed workspace first saves all in-memory scenes.
If that save fails, the switch is cancelled. An unbound collection is not
silently overwritten; New/Open first writes a complete recovery workspace under
the app-state root, rather than recovering only the live scene.

### Path policy

App path decisions live in `src/app/glr_paths.{c,h}`:

- workspaces root: `./workspaces` for the existing writable-cwd development
  contract, otherwise `<user-data>/workspaces`;
- default managed workspace: `<root>/default`;
- app state: relative leaf in writable-cwd development runs, otherwise
  `<user-data>/state/<leaf>`;
- unbound `.glr` / `.ply`: relative leaf in writable-cwd development runs,
  otherwise `<default-workspace>/<leaf>`.

The path APIs distinguish a **workspace name** from an **arbitrary folder
path**. New Workspace and Save Workspace As accept names only and always create
`<root>/<slug>`. Other Folder accepts a path. A slash is never an implicit mode
switch in a name field.

`glr_paths_same_dir()` canonicalizes existing paths with `realpath()` and uses a
lexically normalized absolute fallback for paths that do not exist yet.

No workspace is bound merely by booting the app or opening/cancelling a prompt.
That avoids stamping an absolute `@workspace-dir` into unrelated exports and
keeps current UI goldens unchanged.

## Implementation

### 1. Workspace filesystem model

Extend `src/repl/workspace_io.{c,h}` with:

- manifest constants and `WorkspaceManifest` storage capped at
  `MAX_USER_SCENES` for this implementation;
- strict workspace-name validation before slug fallback (`"!!!"` is invalid;
  `workspace_io_filename_slug()` currently falls back to `"s"`);
- manifest read/write-to-temp/commit helpers;
- safe path join and stable filename allocation;
- regular-file checks for manifest entries;
- strict rejection of manifest-less folders.

Extend each `UserScene` with its persisted `.c` filename. Imported managed
scenes receive the manifest filename; new/promoted scenes allocate one stable
filename against the catalog and destination. Save Workspace As retains those
stable identities when possible and resolves any unrelated-file collision
without publishing the new assignment unless the save succeeds.

Change workspace APIs to report structured outcomes rather than overloading an
integer count:

```c
typedef struct {
    int ok;
    int managed;
    int files_seen;
    int scenes_loaded;
    int files_failed;
    int capacity_exceeded;
} ReplWorkspaceLoadResult;
```

Update startup and tests to use the structured managed-workspace result; do not
retain an old-format workspace compatibility surface.

`repl_save_active_scene()` becomes an `int` result. Actions close Save-As
prompts only after confirmed success. Reveal targets the bound managed
workspace and is disabled while no managed workspace is loaded.

### 2. Complete recovery and safe switching

Retain `recovery.c` for quit compatibility, but add an app-owned recovery
workspace directory for destructive workspace switches. It contains every
occupied scene plus a manifest and is never inside a user workspace.

`glr_action_open_workspace_dir()` owns the switch choreography:

- save the current managed workspace first;
- otherwise save a complete recovery workspace;
- transactionally load the target;
- mutate camera/undo/menu state only after load succeeds;
- create a fresh named scene only when the load result explicitly says the
  managed workspace is valid and empty.

### 3. Workspace catalog and paths

Add `src/app/glr_workspaces.{c,h}`:

```c
void        glr_workspaces_refresh(void);
int         glr_workspaces_count(void);
const char *glr_workspaces_name(int idx);
const char *glr_workspaces_path(int idx);
int         glr_workspaces_active_index(void);
int         glr_workspaces_create(const char *name,
                                  char *out_path, size_t out_sz,
                                  char *err, size_t err_sz);
```

Only directories containing a valid `.glr-workspace` are listed as managed
workspaces. Enumeration is explicit and sorted; refresh on File-menu open and
after create/save-as/open/delete.

Creation uses an exclusive leaf `mkdir`, never permissive mkdir-p onto an
existing destination. Empty, punctuation-only, path-containing, and colliding
names keep the prompt open with an inline error.

### 4. App modal

Add `src/app/glr_modal.{c,h}` with kinds for:

- New Workspace
- Save Workspace As
- Open Other Folder
- Save Scene As
- Confirm Delete Scene

The modal module owns prompt state, filtering, rendering data, and key capture.
Filesystem/scene mutations remain in `glr_actions`; modal commit delegates to an
action callback so the modal is not a second business-action controller.

It is mutually exclusive with the two existing editor inline prompts, routes
before them for keyboard/special input, cancels on outside click/reset, and
surfaces commit errors inside the strip.

Save Scene As is used only for an example/post-tutorial transient when relative
saves are unavailable. It reserves/promotes the scene, applies the requested
name, saves it, and closes only after success. Cancelling the prompt leaves the
workspace binding unchanged.

### 5. Safe delete and rename

Delete is available only for an active scene in a bound managed workspace.
Confirmation names the persisted file. Commit order is:

1. snapshot the complete in-memory catalog and remove the scene from the
   working catalog;
2. stage and commit a manifest without the scene;
3. remove the scene file (`ENOENT` is success; other failures restore the old
   manifest and the catalog snapshot);
4. activate another slot, or create a fresh named scene if none remains;
5. clear undo and refresh the workspace list.

An unbound scene uses **Remove Tab**, not a misleading destructive
Delete-on-disk action.

Rename updates the display name immediately in memory. On the next successful
managed save, its stable filename remains unchanged; a later explicit
"rename file with scene" feature can perform the staged manifest transaction.
This deliberately favors identity and safety over making the filename mirror
every display-name edit.

### 6. File menu and Reveal

File menu layout:

```text
New Scene / Save Scene / Save Scene as .glr / Load Scene /
Load Scene from Clipboard / Rename Scene / Delete Scene / Export .ply /
Split Declaration / Reveal Workspace Folder / --- /
New Workspace... / Save Workspace / Save Workspace As... /
Open Workspace > / --- / Quit
```

File receives a flyout provider for Open Workspace. It lists managed workspaces
plus **Other folder...**, highlights the active managed workspace, and handles
File submenu routing before the existing example fallthrough.

Reveal is deterministic: it reveals the bound managed workspace directory and
is greyed out/non-interactive when none is loaded. It does not track a mutable
"last output" whose meaning changes by action. macOS uses `posix_spawn()` with
`/usr/bin/open` and waits/reaps; other native platforms report that the action
is unavailable.

### 7. Capacity behavior

Implicit LRU eviction is removed. With a manifest capped at eight scenes,
evicting a tab to disk would either create an inaccessible ninth manifest entry
or later prune the evicted scene. New/import/promotion operations therefore
fail without mutation when all eight tabs are occupied and tell the user to
delete a scene first. A future scene browser can raise the manifest capacity
and reintroduce close/reopen semantics explicitly.

## Verification

Required regressions:

- packaged app: transient Ctrl+S prompts and saves a named scene under app data;
- writable-cwd development: transient Ctrl+S still writes `./output.c` without a
  prompt; `.glr`, `.ply`, and `recovery.c` remain relative;
- Ctrl+S and File -> Save Scene share the exact action path;
- cancel Save Scene As leaves the workspace binding unchanged;
- rename/save/reload produces one scene, not old and new filenames;
- colliding scene slugs remain stable across delete/save/reload;
- unrelated `.c` beside a managed manifest is ignored and never pruned;
- switching with multiple dirty in-memory scenes preserves all of them;
- malformed/missing manifest scene rolls the complete load back;
- more than eight manifest scenes reports capacity and rolls back;
- failed Save Workspace As keeps the old binding;
- failed unlink keeps/restores the scene and manifest;
- empty managed workspace creates one fresh named tab;
- recovery `.c`, `.glr`, and `.ply` never load as workspace scenes;
- workspace enumeration is sorted and relative/absolute active matching works;
- File flyout routing cannot fall through to example loading;
- auto-promotion with eight slots fails without mutating the origin.

Run:

```bash
make test
make test-stubs
make check-state-ownership
make check-c99
bash scripts/docs-assets.sh
```

Manual packaged-app pass:

1. `make app && open gl-repl.app`.
2. Create `demo`, save two scenes, rename one, and reveal the workspace.
3. Create/open another workspace and return; both scenes must survive.
4. Delete one scene and reopen; it must not return.
5. Put an unrelated `.c` beside the manifest; it must not appear or be removed.
6. Break a manifest scene path, attempt to open it, and confirm the current
   workspace remains intact.
7. Quit and confirm the standalone recovery path is printed and writable.

Docs updated with the implementation: `docs/USER_GUIDE.md`,
`docs/ADVANCED_USAGE.md`, `docs/MODULES.md`, `docs/ARCHITECTURE.md`,
`src/repl/ARCHITECTURE.md`, `CLAUDE.md`, `AGENTS.md`, and `.gitignore`. Run
`make fix-doc-links` after line-number drift.
