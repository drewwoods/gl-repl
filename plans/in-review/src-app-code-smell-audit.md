# `src/app/` — Code-Smell Audit

> Audit produced 2026-05-24. Findings come from four parallel reviews of
> `src/app/` (`glr_ctrl.c`; `glr_actions.c`; `glr_audio.c`; the camera +
> completion + config + state + debug + defaults + source-document
> bucket) plus targeted spot-verification of the most actionable claims.
> File:line references are exact at the time of writing — check `git
> log` on the cited files before acting if this doc has aged.
>
> Scope: every file under `src/app/`. Tests under `tests/` were read
> where they document a contract, but not audited.
>
> The single most important contract for this directory:
> **`src/app/` is the composition root / mediator — `glr_ctrl_*` routes
> raw input to owning subsystems and assembles per-frame snapshots; it
> does not implement editor behavior, parse the language, or draw
> widgets.** A `check-glr-ctrl-not-editor-mirror` guard enforces the
> name-shape side of that contract, but the guard only watches symbol
> names. The dominant theme in the findings below is *that contract is
> leaky in places the guard cannot see* — chiefly ~600 lines of GL
> rendering living in `glr_ctrl.c` and `_mut()` accessors used for
> read-only work — plus a pile of audio-thread bugs that just haven't
> bitten yet.

## How to read this

Severity grouping mirrors the previous audits:

- **🔴 Actual bugs / hazards (verified)** — correctness or
  memory-safety issues with a concrete failure mode that exists in
  current production code. Pick these up first.
- **🟡 Drift / boundary hazards** — layer-crossing reaches, naming
  drift, parallel structures, hidden side effects, ambiguous-intent
  code that works today but is one edit away from misbehaving.
- **🟢 Dead code / dead fields** — code with no callers, unreachable
  branches, redundant initializers, unused parameters. Pure surface
  reduction.
- **🔵 Structural concerns** — long functions, near-duplicate pairs,
  magic numbers, comment archaeology. Bigger refactors; higher cost.

Each finding cites file + line, names the smell, says why it matters,
and suggests a one-line fix.

## 🔴 Actual bugs / hazards (verified)

### 1. Audio worker holds `g_mtx` while calling into miniaudio

**Where:** `src/app/glr_audio.c:196-204` (`cursor_seconds_locked`),
`:670-674` (`stop_music`), `:724-734` (`set_loop_mode`), `:745-749`
(`set_paused`)

**Smell:** The pattern repeats:
```c
lock();
g_loop_mode = mode;
if (g_active >= 0)
    ma_sound_set_looping(&g_slot[g_active], ...);   // miniaudio call under g_mtx
unlock();
```
Plus `cursor_seconds_locked` calls `ma_sound_get_cursor_in_pcm_frames`
under the lock. The file's doc-comments assert these miniaudio entry
points are "non-blocking" — but miniaudio's public contract does not
formally guarantee that, and the audio device callback (a thread the
REPL doesn't own) may take internal locks.

**Why it matters:** Future deadlock waiting for a miniaudio version
bump. If `ma_sound_set_looping` ever takes an internal lock that the
device callback also holds, audio-callback → miniaudio internal → main
holds `g_mtx` and waits for the same internal → deadlock. Same family
as #2 / #3.

**Fix:** Snapshot `g_active` + a slot pointer under the lock, release,
then call the miniaudio op. Or add a code comment cross-referencing
the miniaudio doc lines that confirm lock-freedom for each op used.

### 2. Audio module's lock helpers no-op when the worker isn't running

**Where:** `src/app/glr_audio.c:160-161`

**Smell:**
```c
static void lock(void)   { if (g_worker_running) pthread_mutex_lock(&g_mtx); }
static void unlock(void) { if (g_worker_running) pthread_mutex_unlock(&g_mtx); }
```

**Why it matters:** The conditional-lock trick conflates "no worker
thread exists" with "no synchronization needed". On the worker-create
failure path (#5), the engine is still inited and miniaudio's internal
threads are still alive, but every public API now runs without
synchronization — silently. The `g_worker_running` read itself is
unsynchronized.

**Fix:** Initialize `g_mtx` unconditionally (use
`PTHREAD_MUTEX_INITIALIZER` or always call `pthread_mutex_init` in
`glr_audio_init`); lock unconditionally. Uncontended mutex ops are
negligible. This single change also tightens #3, #4, and #11.

### 3. Several audio public APIs touch shared state without the lock

**Where:** `src/app/glr_audio.c:758-768` (`glr_audio_set_muted` /
`_is_muted`), `:807-820` (`glr_audio_on_user_gesture` reads
`g_playlist_pos` / `g_req_seek` worker-mailbox slots),
`:529-543` (`request_start` writes `g_playlist_pos` / `g_req_seek`
unlocked in the deferred-gesture branch)

**Smell:** Inconsistent with `set_paused`/`set_loop_mode`/`set_cfg_mode`
peers (which take the lock — see #1). `g_muted` is touched bare;
`g_pending_start` / `g_playlist_pos` / `g_req_seek` are touched bare on
some paths.

**Why it matters:** Torn-read on architectures without atomic word
writes; the project's `-std=c99` portability goal is partly about
"old machines / old GCC" where `int` writes aren't always atomic.
Worse, `g_req_seek` aliases the worker-mailbox seek slot with the
deferred-gesture seek — same variable, two ownership disciplines.

**Fix:** Take `g_mtx` in every flag-style getter/setter. Use a dedicated
`g_pending_seek` for the deferred-gesture path so the worker mailbox
isn't aliased.

### 4. `worker_save_state` ignores all I/O errors before `rename`

**Where:** `src/app/glr_audio.c:240-280`

**Smell:** None of the `fprintf` return values are checked; `fflush` /
`fclose` errors are ignored. The temp file is then `rename`d on top of
the real state file.

**Why it matters:** If the partition fills up, a partial line like
`track=foo\noffs` gets atomically swapped over the good state file.
`load_state`'s `atof` returns 0.0 on the malformed offset line;
symptom is "song mysteriously restarts from 0 sometimes" with no log
clue.

**Fix:** Check `fflush(f)` / `fclose(f)` returns; on either failure
`remove(tmp)` and return without renaming.

### 5. `AWR_QUIT` can be silently overwritten by a later `worker_post`

**Where:** `src/app/glr_audio.c:471-507` (worker loop) and `worker_post`
(latest-wins mailbox)

**Smell:** `worker_post` overwrites whatever request is in flight. If
`glr_audio_shutdown` posts `AWR_QUIT` and another caller posts
`AWR_START` before the worker dispatches, the worker never quits and
`pthread_join` hangs forever.

**Why it matters:** Real shutdown-hang potential under racy teardown
(test fixtures, signal-handler quit paths, etc.). The current production
sequence happens to be safe — shutdown is called last — but the
invariant is unenforced.

**Fix:** In `worker_post`, treat `AWR_QUIT` as terminal — refuse to
overwrite it. Or use a separate `int g_quit_requested` flag the worker
checks at each iteration.

### 6. `glr_audio_init` returns success even when mutex/cond init fails

**Where:** `src/app/glr_audio.c:549-589`

**Smell:**
```c
if (pthread_mutex_init(&g_mtx, NULL) == 0 &&
    pthread_cond_init(&g_cv, NULL) == 0) { ... pthread_create ... }
... /* both can fail and we still fall through */
return 0;
```
The engine is inited above this block; if mutex setup fails the engine
stays live with no worker and no working primitives. `set_muted` /
`play_playlist` / etc. appear to work and silently no-op.

**Why it matters:** Silent degradation; downstream callers have no
signal that audio is broken.

**Fix:** On mutex / cond / thread setup failure, `ma_engine_uninit` and
set `g_inited = 0` — i.e. fully back out.

### 7. `worker_save_state` corrupts the temp filename for paths with a directory component

**Where:** `src/app/glr_audio.c:233-235`

**Smell:**
```c
char tmp[GLR_AUDIO_MAX_PATH];
if (snprintf(tmp, sizeof(tmp), ".%s.tmp", state_file) >= (int)sizeof(tmp))
    return;
```
The `.` is prepended to the *whole* path. `state_file = "subdir/state.ini"`
becomes `.subdir/state.ini.tmp` — a file named `state.ini.tmp` inside a
hidden directory `.subdir` that doesn't exist. `rename` then fails
silently (return is not checked beyond zero — see #4) or crosses
directories.

**Why it matters:** Saves silently break for any non-flat path. Today
the production path is flat, but the constraint is implicit.

**Fix:** Split into dirname + basename, prepend `.` to the basename
only. Or use `mkstemp` adjacent to the target file.

### 8. `source_document_apply_change` leaves the document partially mutated on failure

**Where:** `src/app/glr_source_document.c:44-81`

**Smell:** A pre-insert delete can succeed and the subsequent insert (or
`INSERT_MANY` mid-loop) can fail, returning 0 but leaving the editor
buffer half-mutated. Callers (`autonormal.c`, `export.c`) treat the
return as atomic.

**Why it matters:** Concrete corruption window. Callers attempt rollback
by issuing a follow-up `source_document_apply_change(&rollback)` — that
pattern only works if intermediate failures leave a recoverable
snapshot, which they don't here.

**Fix:** Snapshot the buffer before any mutation and restore on
failure, or precheck capacity (insert count + current line_count ≤
`MAX_COMMANDS`) before issuing any sub-call.

### 9. `SOURCE_TEXT_LOAD_ALL` silently truncates oversized input

**Where:** `src/app/glr_source_document.c:71-77`

**Smell:**
```c
if (n > MAX_COMMIT_CMDS) n = MAX_COMMIT_CMDS;
for (int i = 0; i < n; i++) ptrs[i] = change->text[i];
return editor_buffer_load_lines(ptrs, n);
```
LOAD_ALL silently clamps `change->count` rather than failing. The
`SourceTextChange.text` array is itself sized `[MAX_COMMIT_CMDS]`, so
`change->count > MAX_COMMIT_CMDS` already implies a contract
violation that should crash, not paper over.

**Why it matters:** For an atomic-replace operation, dropping the tail
without telling anyone is the wrong default.

**Fix:** `if (n > MAX_COMMIT_CMDS) return 0;` so callers see the bound
violation, or assert if the invariant is supposed to hold by
construction.

### 10. `glr_camera_restore` doesn't reset velocities

**Where:** `src/app/glr_camera.c:283-288`

**Smell:**
```c
void glr_camera_restore(const GlrCameraState *snap) {
    if (snap) { cancel_target_ease(); g_camera = *snap; }
}
```
`g_vel_*` survive the restore. If a user is mid-drag (high momentum)
and a snapshot restore lands a new pose (e.g., scene switch, undo), the
next `glr_camera_tick()` applies the stale velocity to the new pose.

**Why it matters:** Visible glitch — camera lurches after restore. The
header at lines 67-72 acknowledges "Pointer cache, target easing, and
momentum velocities are NOT part of the snapshot — those are transient
session state". `cancel_target_ease()` is called but `reset_velocities()`
is not. The "transient session state" comment makes the intent clear;
the implementation only does half of it.

**Fix:** Add `reset_velocities()` and `g_pointer_button = -1;` to the
restore, matching `glr_camera_controls_reset()` semantics.

### 11. Camera-import state-3 fall-through comment is wrong; behavior is ambiguous

**Where:** `src/app/glr_camera_export.c:167-178`

**Smell:**
```c
if (g_cam_parse_state == 3 && strncmp(p, "glRotatef", 9) == 0) {
    /* Animation hook line `glRotatef(g_angle, 0,1,0)` — no
     * scalars, just advance. Tolerate its absence by falling
     * through to state 4 (older saved files omit it). */
    const char *q = strchr(p, '(');
    if (q && strstr(q, "g_angle")) { g_cam_parse_state = 4; return 1; }
    g_cam_parse_state = 4;
    /* fall through to try the target translate on the same line */
}
```
The "fall through to try the target translate on the same line" comment
is wrong — the next state-4 branch matches `glTranslatef`, not the
current `glRotatef` line. The state machine advances **without
consuming** the line; the line is then re-fed as user code while the
machine is misaligned.

**Why it matters:** Any unexpected non-`g_angle` `glRotatef` at state 3
silently leaks into user code AND leaves the state machine expecting
the wrong line shape for the camera-target translate.

**Fix:** Decide explicitly: either consume-and-discard (return 1 after
setting state=4) or reject-the-block (return 0 without bumping state).
Update the misleading comment regardless.

### 12. `cam_consume_example_block_now` fabricates a synthetic `g_angle` line

**Where:** `src/app/glr_camera_export.c:203-214`

**Smell:**
```c
if (!cam_try_consume_import_line(block->lines[2])) return 0;
if (!cam_try_consume_import_line("glRotatef(g_angle, 0, 1, 0)")) return 0;  /* synthetic! */
if (!cam_try_consume_import_line(block->lines[3])) return 0;
```
Two formatters write 4-line blocks (`cam_format_save_block` uses
`g_angle`; `cam_format_display_block` uses literal `ry`), but the
import state machine expects 5 lines. A fake line is injected to satisfy
the state-3 → state-4 transition.

**Why it matters:** Exactly the kind of seam-misalignment hack the
bridge abstraction was meant to remove. The two block representations
and the import state machine are out of shape with each other.

**Fix:** Unify the block representation (state 2 accepts either
`g_angle` or numeric `ry` without a separate state 3) or expose a
dedicated `apply_block_directly()` entry point that bypasses the
line-stream parser.

### 13. `editor_clear_all_cmds()` inside NEW_SCENE posts wrong status and is otherwise redundant

**Where:** `src/app/glr_actions.c:604`

**Smell:**
```c
repl_scenes_enter_transient_scene();
repl_scenes_reset_for_transient();   /* already wipes the document store */
editor_clear_all_cmds();              /* runs tutorial guard, pushes undo, re-clears empty store,
                                         AND calls repl_set_status("All commands cleared") */
editor_undo_clear();                  /* immediately discards the snapshot above */
```
`repl_scenes_reset_for_transient()` already does
`repl_state_document_reset()`. `editor_clear_all_cmds()` runs the
tutorial guard (can ABORT), pushes an undo snapshot (immediately
discarded one line later), re-clears an already-empty store, and posts
the user-facing status `"All commands cleared"` when the user clicked
*New Scene*.

**Why it matters:** Misleading toast on a routine action; potential
tutorial-guard surprise. The buffer-clear / edit-line reset / input
reset is the only real work this call does on this path.

**Fix:** Drop `editor_clear_all_cmds()` and inline the few editor-side
resets `repl_scenes_reset_for_transient` doesn't cover
(`editor_buffer_clear`, `editor_state_edit_line_set(0)`,
`editor_input_clear`, pending-newline reset). Or add a smaller
`editor_reset_for_new_scene()` helper.

## 🟡 Drift / boundary hazards

### 14. ~600 lines of GL rendering live in the "thin controller"

**Where:** `src/app/glr_ctrl.c:243-924` — `tess_preview_*` (243-248),
`glr_ctrl_render_replay_fade_batches` (292),
`glr_ctrl_render_replay_tess_preview` (350),
`glr_ctrl_render_vertex_numbers` (416),
`glr_ctrl_render_normal_vectors` (434),
`glr_ctrl_render_outlines` (535-667),
`glr_ctrl_render_vertex_points` (669),
`glr_ctrl_render_cursor_guides` (843)

**Smell:** ~30% of the 4069-line controller is raw `glBegin/glEnd/
glColor/glPushAttrib/glPolygonMode/glLineWidth` code. The file's own
README (`src/app/README.md:18-20`) says: *"It does not implement editor
behavior, parse the language, or draw widgets."* `scene/overlays.h:26-29`
openly admits the work was pushed out of `src/scene/` rather than
refactored: *"Outlines and vertex-point overlays are controller-owned
passes, not scene primitives. src/app/glr_ctrl.c re-executes the
user's geometry in GL_LINE or GL_POINT mode."*

**Why it matters:** The `check-glr-ctrl-not-editor-mirror` guard
catches name-shape violations only — it can't see ~600 lines of GL
state-machine pushing. The `post_fill_fn` / `post_overlays_fn` callback
abstraction makes `src/scene/` call back into the controller, inverting
the intended dependency arrow.

**Fix:** Extract `src/scene/replay_overlays.c` (fade batches, tess
preview), `src/scene/edit_overlays.c` (outlines, vertex points/numbers,
normals), `src/scene/cursor_guides.c` (cursor edit guide walk). Have
them take `OverlayWalkCtx` / `SceneGuideSnapshot` (already pure data
types) — those are the natural API boundaries. The `post_fill_fn`
callback collapses into direct calls from `scene_render_3d_scene`.
`glr_ctrl.c` shrinks by ~600 lines.

### 15. `_mut()` accessors used for read-only work

**Where:** `src/app/glr_ctrl.c:90-101` (`glr_ctrl_build_focus_vertex`
reads via `repl_state_document_cmds_mut()` three times);
`src/app/glr_completion.c:142-147` (read-only walk uses
`repl_state_document_cmds_mut()[i]` three times in two lines);
`src/app/glr_config.c:34-43` (`accum_aa_get_cycle` reads
`glr_state_render_mut()` three times)

**Smell:** CLAUDE.md is clear: *"Mutable `_mut()` accessors; owner
modules and controller only."* The non-mut variants exist and are used
elsewhere in the same files.

**Why it matters:** Silently grants write access where read suffices.
Defeats the constness contract the typed-facade pattern encodes; any
future refactor that wants to enforce read-only paths can't.

**Fix:** Cache the const accessor result once at the top of the
function and use it. Add the const variant if one is missing (in the
non-`_mut` form).

### 16. `glr_ctrl_init_gl` mutates `g_cfg_items[]` strings directly

**Where:** `src/app/glr_ctrl.c:2348-2364`

**Smell:** The controller scans `g_cfg_items[]` (owned by
`glr_actions.c`) twice, overwriting the `label` field of the
`GLR_CONFIG_MSAA` row in-place, with a `static char msaa_label[32]` to
back it.

**Why it matters:** Breaks the `g_cfg_items[]` ownership invariant.
Both branches duplicate the same scan. The `static char` mutable
global is pointed-to by the table — a publisher pattern that nothing
else in the table follows.

**Fix:** Add `glr_actions_set_msaa_label(int samples)` to
`glr_actions.{c,h}`; let the actions module own the string and the
lookup.

### 17. `glr_actions.c` writes `scenes->active_example_idx` directly

**Where:** `src/app/glr_actions.c:599-601`

**Smell:** A grep across `src/` confirms only **two** places assign
this field: `src/repl/example_loader.c:491` (legitimate owner) and
this site. `repl_scenes_enter_transient_scene()` on the **next line**
already does `g_example_idx = -1` internally (`scenes.c:353`).

**Why it matters:** Reach-through that breaks the typed-facade
convention this codebase deliberately maintains (`state_owners.h` vs
`state_views.h`). The line is also redundant — fix is delete-only.

**Fix:** Delete lines 599-601 entirely; `repl_scenes_enter_transient_scene()`
covers it. If a separate semantic is wanted, add a named API in
`repl/scenes.h` (e.g., `repl_scenes_detach_active_example()`).

### 18. `find_item_by_key` returns the first section header on `GLR_CONFIG_NONE`

**Where:** `src/app/glr_config.c:123-128`

**Smell:** Section headers carry `GLR_CONFIG_NONE`. A caller passing
`GLR_CONFIG_NONE` (e.g., from an uninitialized `GlrConfigKey`) gets back
the first `### RENDERING` row. The two consumers
(`glr_config_state_count`, `glr_config_state_name`) defend with
`item->section_header`, but that defense is far from the lookup and is
the only thing keeping the bug from leaking.

**Why it matters:** Action at a distance. Any new caller has to know
to defend.

**Fix:** Make `find_item_by_key` early-return NULL when
`key == GLR_CONFIG_NONE`.

### 19. Hidden side effect: every cfg cycle stops an active replay

**Where:** `src/app/glr_actions.c:431-432`

**Smell:**
```c
if (replay_active()) replay_stop();
```
Toggling anything (MSAA, grid, vertex labels, syntax highlighting…)
silently aborts an in-progress replay. Not documented in help text or
surfaced on any cfg-item descriptor.

**Why it matters:** Broken UX — no way to cycle backdrop during a
replay without losing the spot. Surprising behavior with no signal.

**Fix:** Narrow this to the specific keys that genuinely invalidate
replay state (AUTO_NORMALS, POINT_ATTENUATION), or add an
`invalidates_replay : 1` bit on `GlrConfigItem` and key the
side-effect off that.

### 20. Audio-mode cycle has a two-path side effect; `glr_config_set` alone doesn't apply audio

**Where:** `src/app/glr_actions.c:462-464`, `src/app/glr_config.c:154-155`

**Smell:** The cycle path is `glr_cfg_cycle_row` → `glr_config_cycle` →
`glr_config_set` → `glr_audio_set_cfg_mode(value)` (stores int only;
does NOT change `paused`/`loop_mode`). Then `glr_cfg_cycle_row` reads
the stored value back and *separately* calls `apply_audio_cfg_mode(mode)`
to actually apply the semantics.

**Why it matters:** Any other caller of
`glr_config_set(GLR_CONFIG_AUDIO_MODE, …)` — e.g. the
`glr_export_cfg_apply` bridge during workspace `@cfg audio = N` load —
silently fails to take effect. `fill_all` (full workspace cfg snapshots)
includes audio; `cfg_key_in_scene_subset` excludes it. Latent bug.

**Fix:** Have `glr_audio_set_cfg_mode` itself call the equivalent of
`apply_audio_cfg_mode` (or have `glr_config_set` call both). The
two paths merge.

### 21. Naming drift: `GLR_CONFIG_XFORM_GUIDES` ↔ `show_vertex_guides`

**Where:** `src/app/glr_config.c:83`, `src/app/glr_state.h:34`

**Smell:**
```c
case GLR_CONFIG_XFORM_GUIDES:       return &glr_state_presentation_mut()->show_vertex_guides;
case GLR_CONFIG_XFORM_GUIDE_MODE:   return &glr_state_presentation_mut()->xform_guide_mode;
```
The config-key family `XFORM_*` rebinds to a field with the *different*
`vertex` prefix. The menu label is "Xform guides".

**Why it matters:** Reading the switch case is misleading — key, field,
and label disagree on the name.

**Fix:** Rename either the key (`GLR_CONFIG_VERTEX_GUIDES`) or the
field (`show_xform_guides`) so all three converge.

### 22. `CFG_DEFAULT_*` isn't actually the single source of truth

**Where:** `src/app/glr_state.c:58, 62, 70-78` (bare literals);
`src/app/glr_defaults.h` (where they should live)

**Smell:** `autonormal = 0`, `highlight_current_poly = 1`,
`use_accum = 1`, `accum_aa_enabled = 1`, `accum_samples = 2`,
`msaa_samples`, `accum_jitter_x/y` are inlined as numerics.
`glr_defaults.h`'s file-comment says its job is to be the single source
of truth for these.

**Why it matters:** Exactly the brittleness the project's own MEMORY
warns about (`config_default_test_brittleness`): tests pin a value and
silently diverge from the shipped default. Two places to update on
every shipped-default tweak.

**Fix:** Add `CFG_DEFAULT_AUTONORMAL`, `CFG_DEFAULT_HIGHLIGHT_CURRENT_POLY`,
`CFG_DEFAULT_USE_ACCUM`, `CFG_DEFAULT_ACCUM_*`, `CFG_DEFAULT_MSAA_SAMPLES`
macros and reference them. Consider a `GLR_STATE_INITIAL` literal
mirroring `GLR_CAMERA_INITIAL` so BSS-zero startup matches docs.

### 23. `glr_config.c` forward-declares accessors that already have headers

**Where:** `src/app/glr_config.c:10-18`

**Smell:** The comment cites a `check-controller-boundaries` constraint
to justify the forward declarations. That constraint forbids `ui/`
includes from REPL modules — but `glr_config.c` is an `app/` module
and is explicitly allowed to include UI headers. The four accessors
(`glr_camera_mut`, `ui_state_profile_panel_mut`,
`variable_panel_view_mut`, `replay_state_mut`) already have proper
headers.

**Why it matters:** Bypasses type-checking for any future signature
change to those accessors. The justification comment is stale.

**Fix:** Include the real headers; drop the forward decls and the
obsolete justification comment.

### 24. Inconsistent locking discipline: setters lock; getters don't

**Where:** `src/app/glr_audio.c:758-768` (`set_muted` mutates bare;
`is_muted` reads bare) vs `:724-734`, `:745-749`, `:790-805` (peers
that lock)

**Smell:** No documented rule; each accessor picked a discipline.

**Why it matters:** Reader has to grep every accessor to find the
synchronization shape. Mixed disciplines also mean any future "this is
fine because we always lock" reasoning is locally true and globally
wrong.

**Fix:** Pick one rule and apply it (recommended: always lock; the cost
is negligible). Subsumed by #2's "lock unconditionally" fix.

### 25. `set_playlist` while a load is in flight is latest-wins-racy

**Where:** `src/app/glr_audio.c:618-647`

**Smell:** `worker_post(AWR_UNINIT, …)` is fired-and-forgot; the
worker may be mid-`worker_load`. Then `set_playlist` zeroes
`g_playlist_count` immediately under the lock, but `worker_load`'s
post-init publish at `:419-426` writes `g_playlist_pos = idx` where
`idx` may now be `>= g_playlist_count`.

**Why it matters:** `get_current_track` and `worker_save_state` both
have a `pos < count` guard, so the symptom is "save skipped, status
shows nothing." Cleaner: `g_music_loaded = 1` with `g_playlist_pos`
already out of range — invariant violation that's only OK because
all observers re-check.

**Fix:** Add cancel-in-flight semantics (a `cancel` flag the worker
checks before publish), or explicitly clear `g_music_loaded` in
`set_playlist`.

### 26. Auto-mode getter / setter shape is inconsistent across audio config slugs

**Where:** `src/app/glr_audio.c:100` (`audio_cfg_names[]`) and
`src/app/glr_actions.c:465-470` (`labels[]`)

**Smell:** Two arrays indexed by the same enum (`AUDIO_CFG_*`, 0..3).
Adding a fifth mode requires updating both, and `labels[mode]` reads
past the end with no bounds check.

**Why it matters:** Shadow source of truth; classic two-source-update
bug bait.

**Fix:** Delete `labels[]` and let AUDIO_MODE fall through to the
generic `item->state_names` formatter (`glr_actions.c:472`). The
audio-mode names (`"Pause"`, `"Once"`, `"Song"`, `"All"`) read fine
as-is.

### 27. `g_audio_gesture_sent` audio-gate flag lives on the controller

**Where:** `src/app/glr_ctrl.c:1021-1027`

**Smell:** Every input entry point must remember to call
`glr_ctrl_notify_audio_gesture_once()`. Four dispatch entry points
currently do; a fifth that forgets leaves the audio context suspended
forever (web-only side effect, but still).

**Why it matters:** Bug-bait: adding a new input entry point silently
breaks audio on web. The audio module is the natural owner.

**Fix:** Move the flag into `glr_audio.c`; make
`glr_audio_on_user_gesture()` itself the idempotent gate. Every
caller can call it unconditionally.

### 28. Stale plan/phase references in source comments

**Where:** Multiple files, 15+ occurrences. Sampled:
`src/app/glr_ctrl.c:3511` (`/* set to editor_state_edit_line() in J2.1 */`),
`:1020` (`/* (Relocated from editor_input.c in Phase J1 commit 48a.) */`),
`src/app/glr_camera_export.c:21-22` (`(Bridge introduced as step 4a of feature/decouple-…)`),
`src/app/glr_state.c:11-20`, `src/app/glr_source_document.c:13-14`,
`src/app/glr_completion.c:451`

**Smell:** Phase-N / step-X references to plans that have since landed
or been renamed.

**Why it matters:** Means nothing to a reader without the live plan
loaded. Litters source comments with archaeology that should be in
commit history.

**Fix:** Mechanical sweep — delete phase-step references, keep "why
this is here" notes. Provenance lives in commit messages and
`plans/done/`.

### 29. Two top-level naming prefixes for one module: `glr_ctrl_*` and `glr_app_*`

**Where:** `src/app/glr_ctrl.c` (mixed throughout) — `glr_app_*` examples
at `:1889-2191`: `glr_app_reset_all`, `glr_app_reset_example_chrome`,
`glr_app_install_app_services`, `glr_app_camera_distance`,
`glr_app_export_reshape_projection`, `glr_app_editor_input_reset`,
`glr_app_editor_insert_mode_off`, `glr_app_scroll_to_line`,
`glr_app_follow_cursor`

**Smell:** CLAUDE.md's prefix table doesn't define `glr_app_*`. The
names seem to mark "host-effect bridges installed into REPL" but the
distinction is undocumented; `glr_app_reset_all` is exposed in
`glr_ctrl.h` (line 32) alongside the `glr_ctrl_*` neighbors.

**Why it matters:** Convention drift; new contributors have to guess
the rule.

**Fix:** Either rename all `glr_app_*` → `glr_ctrl_*` (the natural
home — the file is `glr_ctrl.c`), or document `glr_app_*` as a
sub-prefix in CLAUDE.md and group them physically.

### 30. `OverlayWalkCtx` and `ReplayVertexWalkContext` overlap, but are separate structs

**Where:** `src/app/glr_ctrl.c:403-414` (`OverlayWalkCtx` fill),
`:466-477`, `:885-896` (`ReplayVertexWalkContext` fill)

**Smell:** Both structs duplicate 5 fields: `program`, `edit_line_idx`,
`cursor_block_begin`, `cursor_block_end`, `cursor_func_scope_mask`.
`OverlayWalkCtx` adds 4 presentation flags; `ReplayVertexWalkContext`
adds `selected_block_only` + a stop_flag. Two fill sites repeat the
same 5-field init; the comments at `:410` and `:890` even copy-paste
the same "not currently exposed via repl_state" admission.

**Why it matters:** Drift between near-duplicate structs is a known
trap (parallel-tables hazard).

**Fix:** Make `OverlayWalkCtx` embed `ReplayVertexWalkContext`, or
extract a `CursorBlockState { edit_line_idx, cursor_block_begin/end,
cursor_func_scope_mask }` and compose.

### 31. `g_replay_fade_plan_*` triplet should be one struct

**Where:** `src/app/glr_ctrl.c:229-236`

**Smell:** Four parallel statics that always change together:
`g_replay_fade_plan` (the data), `g_replay_fade_plan_active`,
`g_replay_fade_plan_base_limit`, `g_replay_tess_preview_active`. The
last one isn't even semantically part of the fade plan but lives
adjacent.

**Why it matters:** Adding a fifth flag means one more parallel static
to remember to update at every write site (build, read, reset).

**Fix:** Make `ReplayFadePlan` carry `active`, `base_limit`,
`tess_preview_active`. One static, one consistent state.

### 32. `glr_debug_dump_editor` and `_dump_flat_commands` take different view types

**Where:** `src/app/glr_debug.h:17-18`

**Smell:**
```c
void glr_debug_dump_editor(FILE *out, SourceTextView text);
void glr_debug_dump_flat_commands(FILE *out, EditorBufferView text);
```
Both functions do the same thing — pull line text out by index — but
one takes `SourceTextView` and the other `EditorBufferView`, forcing
callers to remember which is which.

**Why it matters:** Mechanical friction with no semantic difference.

**Fix:** Pick one (`EditorBufferView` is more specific; `SourceTextView`
is more neutral) and use it for both. Or have both take the underlying
`const char (*lines)[MAX_LINE_LEN] + int count`.

### 33. Stale decay-value comment contradicts `config.h`

**Where:** `src/app/glr_camera.c:24-27`; `config.h:122-130`

**Smell:**
```c
/* Orbit/pan momentum decay per frame. Deliberately independent of
 * config.h's GLR_CAMERA_TARGET_DECAY (the ease-to-target decay) even
 * though both currently default to 0.88f — they are different knobs. */
#define CAM_DECAY 0.88f
```
But `config.h:130` actually defines `GLR_CAMERA_TARGET_DECAY 0.93f`,
and `config.h`'s own prose at line 123 also says "at the default 0.88".
Both comments are wrong.

**Why it matters:** Reader trying to understand the relationship has to
discover the comment is lying.

**Fix:** Delete the "currently default to 0.88f" clause from
`glr_camera.c:25`; the "they are different knobs" reason stands on its
own. Fix the matching prose in `config.h:123`.

### 34. `glr_camera_export_install_bridge` declared on the wrong header

**Where:** `src/app/glr_camera.h:101-105`

**Smell:** The function is implemented in `glr_camera_export.c` but the
only declaration sits on `glr_camera.h`. Any TU that includes
`glr_camera.h` for the basic accessors also sees the bridge-install
API it doesn't care about.

**Why it matters:** Layering implication ("split impl, shared header")
is unstated; symmetry with other bridges (`glr_completion`,
`source_document`) is broken.

**Fix:** Add a one-line `glr_camera_export.h` declaring the install
function. Or, since the file split currently buys little, roll the
bridge code back into `glr_camera.c`.

### 35. Magic tutorial-menu offsets have no symbolic constants

**Where:** `src/app/glr_actions.c:652, 658`; parallel in
`src/ui/app/menu_bar.c:297-298`

**Smell:**
```c
if (tutorial_active() && item_idx == tag_count + 1) { ... }  /* Restart */
if (tutorial_active() && item_idx == tag_count + 2) { ... }  /* Exit */
```
The Scene menu has named offsets in the header: `GLR_SCENE_OFF_HDR = 1`,
`GLR_SCENE_OFF_SCENES = 2`. The Tutorials menu duplicates the pattern
across two files with bare numerals.

**Why it matters:** Adding "Reload Tutorial" silently shifts the
indices and breaks both files.

**Fix:** Add `GLR_TUTORIAL_OFF_SEP/_RESTART/_EXIT` to `glr_actions.h`,
or factor offsets behind a `tutorials_menu_*` accessor.

### 36. `glr_camera_restore` doesn't reset control mode or pointer

**Where:** `src/app/glr_camera.c:292-297` (`glr_camera_controls_reset`)
and adjacent

**Smell:** `glr_camera_controls_reset` clears pointer state +
velocities but preserves `g_control_mode` (2D vs 3D). Other reset
paths (`glr_camera_reset_default`, `glr_camera_ease_to_default`)
*do* force 3D.

**Why it matters:** Asymmetry between named resets isn't documented;
a caller calling "controls reset" hoping for a clean slate gets
half of one.

**Fix:** Add a one-line comment ("mode is preserved across this
reset; use `glr_camera_reset_default` for a full slate") on the
function, or factor the policy into a `GlrCameraResetWhat` enum.

### 37. Inconsistent scene-load helpers: one resets transients, the other doesn't

**Where:** `src/app/glr_actions.c:376-388`

**Smell:**
```c
void glr_scene_load_example(int example_idx) {
    editor_reset_transients();   /* closes menu, picker, camera drag, code-panel drag */
    editor_undo_clear();
    editor_state_edit_line_set(repl_load_example(example_idx));
}
void glr_scene_load_user_slot(int slot) {
    editor_undo_clear();
    if (repl_load_user_scene_idx(slot))
        editor_load_line_to_input(editor_state_edit_line());
}
```
The header doc-comment treats them as "shared scene-load sequences"
but they diverge in non-obvious ways. Loading a user slot leaves the
color picker / camera drag / menu intact; loading an example tears
them down.

**Why it matters:** Tab-strip caller assumes "lightweight" semantics;
menu caller assumes a full reset. Edit one without the other and the
divergence widens.

**Fix:** Either unify with one helper taking a `reset_transients`
flag, or spell out in the header doc-comment which transient surfaces
each path touches and why.

### 38. Trailing fallthroughs in `glr_action_menu_item_activate` swallow out-of-range items

**Where:** `src/app/glr_actions.c:637-638` (MENU_SCENE),
`:678` (implicit fallthrough out of MENU_FILE)

**Smell:** For an unmatched `item_idx` in MENU_FILE, control falls
through to the closing `return 1;`. MENU_SCENE has its own trailing
`return 1;` at 638. Either swallows unknown indices by closing the
menu silently.

**Why it matters:** When MENU_FILE gains an item and a developer
forgets to update the chain, the symptom is the menu closing
inexplicably, not a visible warning.

**Fix:** Terminate each branch with explicit `return 0;` for unmatched
`item_idx` (keeps menu open), or convert to `switch` so missing cases
get `-Wswitch` warnings.

### 39. Snapshot is built three times per display frame

**Where:** `src/app/glr_ctrl.c:1720-1722`

**Smell:**
```c
glr_ctrl_build_ui_snapshot(&ui_snap);
glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(&ui_snap, NULL, NULL);
glr_ctrl_build_ui_snapshot(&ui_snap);
```
Defended in comment as "intentional so the published snapshot reflects
the post-follow-scroll offset". Architecturally this is the controller
dancing around a missing invariant: snapshot depends on scroll, scroll
is computed from layout that needs the snapshot.

**Why it matters:** Per-frame cost doubled for 25+ fields that didn't
change. The workaround pattern repeats elsewhere — `glr_ctrl_mouse`
(`:3832`) builds the snapshot on each left-button press; the
selection-drag motion path (`glr_ctrl.c:3603-3664`) rebuilds it once or
twice per mouse-move (see #40).

**Fix:** Split snapshot construction into "stable fields built once"
and "scroll-dependent fields fixed up after follow-scroll runs". Or
factor follow-scroll to read live editor state directly.

### 40. Selection-drag handler rebuilds the snapshot once or twice per motion event

**Where:** `src/app/glr_ctrl.c:3603-3664`

**Smell:** On every `motion` callback during a drag, the controller
rebuilds the entire `UiRenderSnapshot` (variable-panel vars copy,
projection lines printf, scene-tabs build, swatch reparse, …) and may
do it twice — the inner `ui_snap` even shadows the outer. `gl_y` is
computed but only used to clamp the top edge; no analogous bottom-edge
clamp despite the comment promising one.

**Why it matters:** Cold mouse motion shouldn't pay for full snapshot
rebuild. Bottom-clamp missing as documented.

**Fix:** Cache last-built snapshot (or the `UiCodePanelRuntimeState`
slice the hit-test needs) per frame on a `g_*` static and reuse in
motion/press routes. Add the missing bottom-clamp.

### 41. `route_numeric_swatch_hit` open-codes an entire editor commit transaction

**Where:** `src/app/glr_ctrl.c:3275-3342`

**Smell:** 68-line function that compiles a new line, parses it, builds
a `ReplCompiledChange`, copies text into the change buffer, calls
`editor_commit_apply_external_change`, then re-loads the line and
re-runs autocomplete. Compare to the much-simpler
`route_inline_color_swatch_hit` (`:3263`, ~10 lines) which delegates
to the color picker peer subsystem.

**Why it matters:** Exactly the per-editor-operation logic the
`check-glr-ctrl-not-editor-mirror` guard exists to discourage. The
guard catches `glr_ctrl_editor_*` symbol names but not router statics
that do the same anti-pattern internally.

**Fix:** Move the body into `numeric_swatch_apply_step(int line_idx,
int direction)` in either `src/editor/` (treated as an editor service)
or a new `src/subsystems/numeric_swatch/` peer subsystem matching the
color-picker pattern. The router becomes 5 lines.

### 42. Hitch-threshold env cache defeats `GLR_AUDIO_HITCH_MS=0`

**Where:** `src/app/glr_audio.c:179-187`

**Smell:**
```c
static double worker_hitch_threshold_ms(void) {
    static double cached = -1.0;
    if (cached < 0.0) { ... }
    return cached;
}
```
The `< 0.0` check is used as the cache-empty sentinel. The clamp
`if (cached < 0.0) cached = 0.0` then makes "user set 0 to disable"
overlap with "cache hasn't been populated" — the function re-reads the
env on every call.

**Why it matters:** Defeats the cache for precisely the value people
will test with ("0 disables").

**Fix:** Use a separate `static int cached_valid = 0;` sentinel.

## 🟢 Dead code / dead fields

### 43. `OverlayWalkCtx` / `ReplayVertexWalkContext` parallel statics — fold into one struct (#30)

Covered above; both fills duplicate 5 lines of init.

### 44. `glr_ctrl_apply_variable_panel_value_change` has an unused return value

**Where:** `src/app/glr_ctrl.c:2918-2962`, called at `:2971`

**Smell:** Function returns `1` in every code path (8 returns); caller
ignores the value: `if (variable_panel_handle_drag_motion(x, &value_change))
glr_ctrl_apply_variable_panel_value_change(&value_change);`.

**Why it matters:** Misleading API — reader assumes the `int` means
consumed/not-consumed.

**Fix:** Make it `void`.

### 45. Dead defensive checks in `build_param_hint_text`

**Where:** `src/app/glr_completion.c:78, 82`

**Smell:**
```c
int arg_index = 0;     /* never goes negative — only `arg_index++` */
...
if (arg_index < 0 || arg_index > param_count) return;
int next_param = arg_has_text ? arg_index + 1 : arg_index;
if (next_param < 0 || next_param > param_count) return;
```
Both `< 0` halves are unreachable.

**Fix:** Drop the `< 0` halves.

### 46. Gratuitous `params_out` parameter

**Where:** `src/app/glr_completion.c:118-166`

**Smell:**
```c
static int find_defined_func_call_params(... const char *params_out[MAX_EXPR_VARS], int *count_out,
                                         char param_storage[MAX_EXPR_VARS][16]) {
    ...
    for (int j = 0; j < param_count; j++) params_out[j] = param_storage[j];
```
Both `params_out` and `param_storage` are caller-supplied; the
function fills `params_out[j]` with pointers into `param_storage[j]`.
Callers already have `param_storage` in scope and don't need a separate
`params` array.

**Fix:** Delete `params_out`; callers index `param_storage` directly.

### 47. Redundant initializer for `g_camera_target`

**Where:** `src/app/glr_camera.c:73`

**Smell:**
```c
static GlrCameraState g_camera_target = GLR_CAMERA_INITIAL;
```
Gated by `g_camera_target_active = 0` (line 74) and overwritten on
every `glr_camera_ease_to` call (line 246). Initial value is never
observed.

**Fix:** Drop the initializer (leave BSS-zeroed) — removes the
misleading suggestion that the initial value matters.

### 48. Stack zero-init arrays fully overwritten on the next line

**Where:** `src/app/glr_ctrl.c:1630-1631` then `:1703-1704`

**Smell:**
```c
float live_predef_vals[MAX_PREDEF_VARS] = { 0 };
float live_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN] = { { 0.0f } };
...
repl_copy_predef_values(live_predef_vals);
repl_eval_copy_scratch_arrays(live_scratch_arrays);
```
The `= { 0 }` initializer is wasted CPU per frame; the contract of
`repl_copy_predef_values` is "fills every slot".

**Fix:** Drop the initializers. If they must stay, add a one-line
comment.

### 49. Dead defensive `if (scenes->active_example_idx >= 0)` guard

**Where:** `src/app/glr_actions.c:600-601`

**Smell:** Assigning `-1` to a field already at `-1` is harmless; the
field has no setter notification. Subsumed by #17's full delete.

## 🔵 Structural concerns

### 50. `glr_cfg_cycle_row` is a 91-line per-key special-case chain

**Where:** `src/app/glr_actions.c:390-481`

**Smell:** Five `if (item->key == GLR_CONFIG_…)` branches (REPLAY,
FOCUS_ORIGIN, RESET_CAMERA, AUTO_TIME+Shift), then a cascade of more
(CODE_PANEL_LAYOUT, AUTO_NORMALS, POINT_ATTENUATION, AUDIO_MODE),
then fallback formatters.

**Why it matters:** Exactly the kind of switch the table-driven
`g_cfg_items[]` idiom exists to avoid (CLAUDE.md: *"Adding a config
item: append to `g_cfg_items[]` — count is auto-computed via sizeof"*).
Hides the replay-stop side effect (#19) inside the same chain.

**Fix:** Add an `on_change(int new_value, const GlrConfigItem *)` hook
field to `GlrConfigItem`; each key declares its own side-effect and
status formatter. Action rows (`state_count == 0`) become the natural
discriminator for early-return. Reduces the function to: lookup →
action-row check → cycle → invoke handler → fallback status.

### 51. `glr_ctrl_render_outlines` is a 132-line god-function

**Where:** `src/app/glr_ctrl.c:535-667`

**Smell:** Single function with: outer `if` on two flags, `glPushMatrix`,
a `for` over the flat program, `switch` with 8 cases, several of which
do their own `if` cascades. 6 state-machine flags mutated inside the
switch (`in_begin`, `matrix_depth`, `block_is_current`,
`tess_in_contour`, `tess_poly_is_current`, plus loop index). Cleanup
tail after the loop mirrors in-switch cleanups verbatim.

**Why it matters:** TESS_BEGIN_CONTOUR / TESS_VERTEX / TESS_END group
(573-605) and the BEGIN/END/VERTEX group (606-641) are two separate
state machines interleaved.

**Fix:** Split into `render_outlines_glbegin_pass()` and
`render_outlines_tess_pass()`. Or extract per-case handlers so the
switch becomes a dispatch table.

### 52. Near-duplicate function pairs

**Where:**
- `cycle_example_or_user_scene` / `_prev` (`src/app/glr_ctrl.c:2750-2839`) — 89 lines walking the same sequence in opposite directions
- `cam_format_save_block` / `cam_format_display_block` (`src/app/glr_camera_export.c:34-60`) — 12 lines duplicated for one line-2 difference (`g_angle` vs literal)
- `glr_ctrl_step_projection_toward` (`src/app/glr_ctrl.c:1169-1195`) — symmetric up/down branches
- `glr_ctrl_router_handle_active_replay_key` / `_replay_toggle_key` (`:2538-2544`) — both forward to the same `replay_handle_key`; the "active" filter is already inside the callee

**Why it matters:** Any change to one half has to be mirrored.

**Fix:** Parameterize: `cycle_scene(int direction)`; one camera-block
formatter taking a line-2 string; one projection-step using
`sign = (mix < target) ? +1 : -1`; collapse the two replay-key routers.

### 53. Magic numbers where named constants exist nearby

**Where:**
- `src/app/glr_ctrl.c:2998` vs `:3956` — scroll-wheel zoom velocity
  `0.3f` vs `0.1f` for the same gesture
- `:580, 612, 615, 703` — outline `glLineWidth(1.5f/3.0f/1.2f)` and
  `glPointSize(2.0f/7.0f)`; only `GLR_NORMAL_ARROW_SCALE` is named
- `src/app/glr_camera.c:421` — auto-rotate `c->ry += 0.3f;` (every
  other camera tunable is a `CAM_*` define)
- `src/app/glr_actions.c:536-537` — `key_code <= 0 || key_code >= 32`
  (the ASCII control-character range, no name)

**Why it matters:** Same input shape, different numeric outcome — and
no comment explaining the asymmetry.

**Fix:** Hoist constants to a single header (or `config.h`). The
zoom-velocity divergence is most likely a real bug (the freeglut path
should match the wheel-callback path); pick one and use it in both.

### 54. Snapshot read-only redundancy: `glr_state_presentation()` called 3-4× per cycle

**Where:** `src/app/glr_actions.c:439, 441, 443, 452`

**Smell:** Function returns the struct by value. The CODE_PANEL_LAYOUT
branch calls it three times reading `.code_panel_layout`. Cheap, but
a hoisted local `GlrPresentationState p = glr_state_presentation();`
would clarify intent.

**Fix:** Hoist to a local.

### 55. Missing direct includes for transitive deps

**Where:**
- `src/app/glr_actions.c` uses `isalnum`/`tolower` (`:194`) without
  `#include <ctype.h>`; uses `strcmp`/`strncmp`/`strtol` without
  direct `<string.h>`/`<stdlib.h>`
- `src/app/glr_ctrl.c:1370` uses `fmaxf` without direct
  `#include <math.h>` (resolves through `repl/eval.h`)
- `src/ui/app/variable_panel.c` family (covered in `src-ui-code-smell-audit.md`)

**Why it matters:** Works today through transitive includes; an
upstream header reorganization breaks the build.

**Fix:** Add the direct includes.

### 56. `lock()/unlock()` ad-hoc helpers shadow stdlib pthread naming

**Where:** `src/app/glr_audio.c:160-161`

**Smell:** Two-character names `lock()` / `unlock()` at file scope are
loud but unsearchable.

**Why it matters:** Reader can't `grep` for the lock points without
matching unrelated `lock_*` words. Audio-thread reasoning needs the
mutex sites to be easily found.

**Fix:** Rename to `audio_lock()` / `audio_unlock()`, or inline the
two pthread calls and drop the helpers.

### 57. `'isspace(ch)'` style mixed across `glr_completion.c`

**Where:** `src/app/glr_completion.c:74` (`if (!isspace(ch))` — `ch`
already `unsigned char`); `:128, 131, 293` (use the explicit cast)

**Smell:** Two styles for the same `<ctype.h>` UB guard.

**Why it matters:** Cosmetic, but the cast is the codebase-standard
form and protects against future signed-char re-typing of the local.

**Fix:** Apply the cast at the bare site.

### 58. `glr_completion_provider_clear` doesn't reset all the AC statics

**Where:** `src/app/glr_completion.c:453-457`

**Smell:** Clears `g_ac_mode`, `g_ac_token_len`, `g_ac_suffix` but
leaves `g_ac_input_offset` and `g_ac_func_matches` writes from
`update_autocomplete` / `accept_autocomplete` paths in place. Three
sites diverge on which statics they touch.

**Why it matters:** The stale values are meaningful only when
`g_ac_mode != AC_MODE_NONE` (today), but the inconsistency is a
footgun.

**Fix:** Single internal `reset_ac_statics()` helper called from all
three places.

### 59. `glr_completion_accept_autocomplete` silently truncates ghost on overflow

**Where:** `src/app/glr_completion.c:428-431`

**Smell:**
```c
if (inp->input_len + ghost_len < MAX_INPUT_LEN - 1) {
    strcat(inp->input, ac.ghost);
    ...
}
```
If the ghost doesn't fit, input is left unchanged and the popup is
cleared. User sees the popup vanish without their typed text growing
— no feedback.

**Fix:** Emit a one-shot status when the ghost is dropped, or factor
the bound into a comment.

### 60. `STATE_SAVE_INTERVAL_SECS` is `#define`d mid-file

**Where:** `src/app/glr_audio.c:154`

**Smell:** Other module tunables (`GLR_AUDIO_MAX_TRACKS`,
`GLR_AUDIO_MAX_PATH`) are grouped at lines 70-71; this one is at 154
in a different section.

**Fix:** Move alongside the other tunables.

### 61. `worker_save_state` and `load_state` duplicate INI parsing

**Where:** `src/app/glr_audio.c:243-263` and `:306-318`

**Smell:** Same `track=` / `offset=` / CR-LF strip pattern in two
places. Small but a copy-paste bug in one corrupts only the
save-while-empty path, which makes it hard to catch.

**Fix:** Extract `static void parse_ini_line(const char *line, char
*out_track, float *out_offset, int *out_cfg_mode)`.

### 62. `audio_worker_main` calls into shared state during its `_uninit_all` epilogue without the lock

**Where:** `src/app/glr_audio.c:343-354` (`worker_uninit_all`)

**Smell:** Releases the lock between zeroing `g_active` and the
per-slot `ma_sound_uninit`. `g_slot_inited[s]` is read/written without
any lock. Today single-writer (the worker) makes this fine; the
invariant is unstated.

**Fix:** Add a one-line ownership comment on `g_slot_inited[]`
("worker-thread-only, no lock") or move the writes inside the lock.

### 63. `glr_ctrl_help_overlay_state` rebuilds help content per-query

**Where:** `src/app/glr_ctrl.c:2657-2668`

**Smell:** Each Tab/click in the help overlay calls
`glr_ctrl_help_overlay_state()`, which calls
`glr_ctrl_help_overlay_content()`, which walks the help table
(`repl_help_text.c:323`, ~11 `cmd_emit_group` + language sections).
Negligible at keystroke rate but inconsistent with the snapshot's
`help_content` field at `:1591`.

**Fix:** Cache the `UiOverlayContent *` once. Or read from
`snap->help_content` (requires a snapshot — see #39's broader fix).

### 64. `static const ReplayTessPreviewCallbacks` uses `g_` prefix instead of `k_`

**Where:** `src/app/glr_ctrl.c:250`, `:1934` (`g_export_projection_bridge_impl`),
`:2025` (`g_glr_host_effects`)

**Smell:** CLAUDE.md convention: `g_` is for *mutable* file-scope
state. Read-only constant tables use `k_` (e.g.,
`k_example_tag_defaults` at `:1845`).

**Fix:** Rename to `k_tess_preview_cb`, `k_export_projection_bridge_impl`,
`k_glr_host_effects`.

### 65. Comment-mass overwhelms code in several `glr_ctrl.c` sections

**Where:** `src/app/glr_ctrl.c:1832-1846, 2017-2024, 2089-2130, 2138-2191`
and others; ~30% of the file is comments

**Smell:** Several dense paragraphs read like commit messages
reproduced inline; many describe historical migrations. Hard to skim.

**Fix:** Promote design context to `MODULES.md` / `ARCHITECTURE.md`
(both exist). Keep one-line "what" + "why" in source.

## Sequencing

### One-afternoon pass

1. **#13** — Drop `editor_clear_all_cmds()` from NEW_SCENE. Removes
   misleading toast and tutorial-guard risk. ~10-line diff.
2. **#17** — Delete the `scenes->active_example_idx = -1` reach-through.
   3-line delete.
3. **#45** + **#46** + **#47** + **#48** + **#49** — Mechanical dead-code
   removal in completion / camera / display path.
4. **#28** — Stale plan/phase comment sweep. Pure delete.
5. **#33** — Fix the wrong decay-value comment in `glr_camera.c:25` and
   `config.h:123`.
6. **#23** — Drop the forward-decl block in `glr_config.c` (include the
   real headers). Removes an obsolete justification comment too.
7. **#44** — `glr_ctrl_apply_variable_panel_value_change` → `void`.
8. **#64** — Rename three `g_*` constant tables to `k_*`.
9. **#55** — Add direct `<math.h>`/`<ctype.h>`/`<string.h>`/`<stdlib.h>`
   includes where currently transitive.

That's ~9 surgical commits; total LOC reduction in the
50-100 range and one real bug closed (#13).

### One-week pass — the audio bug cluster

This is the highest-leverage block: most of the **🔴** bucket is here
and the fixes are well-contained.

- **#2** (lock unconditionally) + **#24** (consistent locking
  discipline) + **#3** (lock the bare-access getters/setters) — all
  one PR. Removes the "no-worker-no-lock" silent degradation and
  closes three torn-read windows.
- **#1** (snapshot `g_active` + slot pointer under the lock; call
  miniaudio unlocked) — separate PR, mechanical but touches every
  setter and `cursor_seconds_locked`.
- **#5** (sticky `AWR_QUIT` in `worker_post`) — one-liner with one
  test.
- **#4** (`fflush`/`fclose` error handling in `worker_save_state`) —
  small.
- **#6** (back out engine on mutex/cond failure in `init`) — small.
- **#7** (fix `tmp` filename prefix for paths with a directory) —
  small.
- **#27** (move audio-gesture flag to `glr_audio.c`) — small; removes
  a hidden invariant from the controller.
- **#42** (hitch-threshold env-cache sentinel) — one-liner.
- **#25** (in-flight `set_playlist` race) — slightly larger; needs a
  cancel flag.

After this PR set, the audio module's `🔴` findings are closed and the
lock discipline is uniform.

### One-week pass — closing the controller's rendering layer

The structural smell that swallows most of `glr_ctrl.c`'s size:

- **#14** — Extract `src/scene/replay_overlays.c`,
  `src/scene/edit_overlays.c`, `src/scene/cursor_guides.c`. Promote
  `OverlayWalkCtx` / `SceneGuideSnapshot` to shared scene types.
  Have `scene_render_3d_scene` call them directly instead of
  through `post_fill_fn` callbacks. `glr_ctrl.c` shrinks by ~600
  lines.
- **#51** — Split `glr_ctrl_render_outlines` into two passes (GL_BEGIN
  / TESS) as the function moves to `src/scene/edit_overlays.c`.
- **#30** + **#31** — Fold `OverlayWalkCtx` / `ReplayVertexWalkContext`
  into one struct; fold `g_replay_fade_plan_*` parallel statics into
  one struct.
- **#15** — `_mut()`-for-reads sweep across `glr_ctrl.c`,
  `glr_completion.c`, `glr_config.c`.

Then **the camera-export seam**:

- **#11** + **#12** — Pick a shape for the camera block (state-2
  accepts either `g_angle` or numeric `ry`) and delete the synthetic
  line and the misleading fall-through comment.
- **#34** — Either add `glr_camera_export.h` or roll the bridge back
  into `glr_camera.c`.

Then **the cycle/menu-action layer**:

- **#50** — Add `on_change` hook to `GlrConfigItem`; migrate the
  per-key chain out of `glr_cfg_cycle_row`.
- **#19** — Narrow the replay-stop side effect to keys that actually
  invalidate replay state (or add an `invalidates_replay : 1` bit).
- **#20** — Have `glr_audio_set_cfg_mode` itself apply the pause/loop
  semantics; collapse the two paths.
- **#26** — Delete `labels[]` audio shadow array; use
  `state_names` formatter.
- **#52** — Collapse the four near-duplicate function pairs.

Then **the small-name-shape cleanups**:

- **#16** + **#22** — Add `glr_actions_set_msaa_label()`;
  add the missing `CFG_DEFAULT_*` macros and a `GLR_STATE_INITIAL`.
- **#21** — Pick one name across `XFORM_GUIDES` key / `show_vertex_guides`
  field / "Xform guides" label.
- **#29** — Rename `glr_app_*` → `glr_ctrl_*` (or document the
  sub-prefix in CLAUDE.md and group them).
- **#32** — Pick one view type for the two `glr_debug_dump_*`
  signatures.
- **#35** — Symbolic constants for the tutorial-menu offsets.

### Out of scope

- The `glr_ctrl_router_*` helper family is the cleanest thing in the
  module — small, single-purpose, testable in isolation. Don't refactor
  the router shape itself; the smells are at the edges (#41, #40),
  not in the routing pattern.
- `glr_defaults.h` is the right kind of header: pure macro definitions
  with a clear ownership story. The smell (#22) is that callers don't
  use it consistently, not the header itself.
- `glr_source_document.c` is 81 lines of bridge plumbing; the bug at
  #8 / #9 is the only finding here. Don't refactor the file; just fix
  the contract.

## Method note

This audit was produced by four parallel review agents:

- `glr_ctrl.c` (163KB, 4069 lines — by far the biggest file in the
  directory) + `glr_ctrl.h` + the module `README.md`
- `glr_actions.c` (30KB, 691 lines) + `glr_actions.h` — the table /
  menu / cycle layer
- `glr_audio.c` (29KB, 833 lines) + `glr_audio.h` — threading and
  miniaudio plumbing
- The remainder bucket: camera (`glr_camera.c` + `_export.c` +
  header), completion, config, debug, defaults, source-document,
  state

Each agent was asked for ~15-25 highest-signal findings, not a
comprehensive sweep. The most actionable claims (real-bug findings
above) were verified against the source. The 🟡 / 🟢 / 🔵 findings
are reported as the agents framed them; spot-check before acting on
the more mechanical ones — and re-grep the `_mut()` references in
#15 in particular, since that pattern shows up enough that the
neighboring sites may have shifted.
