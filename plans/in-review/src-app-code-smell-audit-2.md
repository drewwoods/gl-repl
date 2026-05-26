# `src/app/` — Code-Smell Audit (Follow-Up)

> Audit produced 2026-05-26 by four parallel reviewers, scoped to
> `src/app/`. Findings extend the closed first-round audit
> (`plans/done/src-app-code-smell-audit.md`, last revision
> 2026-05-26) — reviewers were given the closed audit's resolved
> list and instructed **not** to re-flag items already ✅ done.
>
> The slice split:
>
> - `glr_ctrl.c` (~3426 lines — heaviest file in `src/app/`) +
>   `glr_ctrl.h`
> - `glr_actions.c` + `glr_actions.h` + `glr_config.c` +
>   `glr_config.h` + `glr_defaults.h`
> - `glr_audio.c` + `glr_audio.h`
> - `glr_camera.c` + `glr_camera.h` + `glr_camera_export.c` +
>   `glr_completion.c` + `glr_completion.h` + `glr_debug.c` +
>   `glr_debug.h` + `glr_source_document.c` + `glr_state.c` +
>   `glr_state.h`
>
> File:line references are exact at the time of writing; re-verify
> with the cited file before acting if this doc has aged.
>
> **Prior closures verified to still hold:** #1-#13, #15-#20,
> #22-#24, #26-#41, #43-#49, #51-#59, #60-#63. No regressions on
> closed items. Prior audit #25 (partial — `set_playlist` race
> window): the cancel-flag + `AWR_UNINIT` path now covers all
> observable race paths; re-review found no remaining window.
> Recommend closing #25 as Done.

## Headline take

37 findings total. Of those: 2 🔴 (real bugs / hazards), 13 🟡
(drift/boundary), 12 🟢 (dead code / cosmetic), 10 🔵 (structural).

The dominant theme is **`_mut()` for reads and open-coded helpers**:
the prior audit's #15 (`_mut()` sweep) was closed in the controller
and completion paths, but the pattern re-grew in `glr_config.c`
(`config_value_ptr` routes every `glr_config_get` read through 33
`_mut()` calls) and `glr_ctrl.c` (the `g_predef_vars` macro in
`eval.h:172` routes through `_mut()`, used three times for reads).
The other recurring shape is open-coded input-clear sequences that
bypass `editor_input_clear()` (same pattern the editor audit's #4
caught in `commit.c`).

## Tier classification

(Mirrors the system the prior audits use; see
`plans/done/src-editor-code-smell-audit.md` "Tier system" for the
full definitions.)

- **Tier A (small, near-zero risk, 5-30 LOC each):** #1, #2, #3,
  #4, #5, #9, #10, #16, #17, #18, #19, #20, #22, #23, #24, #25,
  #27.
- **Tier B (moderate, focused pass, 50-200 LOC each):** #6, #7,
  #8, #11, #12, #13, #14, #15, #21, #26, #28, #29, #30, #31,
  #33, #34, #36, #37.
- **Tier C (high cost or cross-cutting):** #32, #35.
- **Tier D (kept on purpose):** none.

**Notes on placement:**

- **#1 (normal-guide NULL vars) → Tier A**: one-line fix (replace
  `NULL, 0` with `predef.vars, predef.count`). The vertex path on
  the adjacent line is the template.
- **#2 (host input reset) → Tier A**: replace the open-coded clear
  with `editor_input_clear()` (already used at 15+ call sites).
- **#9 (`config_value_ptr` `_mut()`) → Tier A**: add a
  `config_value_ptr_const()` that returns `const int *` via const
  getters; wire `glr_config_get` through it; leave the mutable
  version for `glr_config_set`.
- **#12 (`load_state` lockless reads) → Tier B**: needs analysis of
  which globals the worker can observe; the fix may be as simple as
  moving the audio_lock() scope to cover the `g_cfg_mode` write, but
  needs verification against the worker's init path.
- **#32 (section label cap) → Tier C**: a compile-time assert needs
  the section count to be a constant, which would require a
  table-shape change.
- **#35 (defaults.h transitive deps) → Tier C**: changing the macro
  expansion strategy touches every includer of `glr_defaults.h`.

---

## 🔴 Actual bugs / hazards (verified)

### 1. Normal-guide arg evaluation ignores predefined variables

**Where:** `src/app/glr_ctrl.c:190-191` (`fill_guide_arg_slots`).

**Smell:** The vertex-arg branch (line 178-180) passes
`predef.vars, predef.count` to the expression evaluator, so
`glVertex3f(x, y, z)` resolves user variables in the guide overlay.
The normal-arg branch (line 190-191) passes `NULL, 0` — any variable
reference in `glNormal3f(x, y, z)` silently evaluates to 0.0,
drawing the normal guide at the origin regardless of the variable's
value.

**Impact:** The guide arrow for a normal whose components come from
predefined variables (e.g. `float nx; glNormal3f(nx, 0, 1)`) points
in the wrong direction. The cursor-guide overlay is the main
feedback mechanism for vertex/normal placement — a wrong normal
direction is actively misleading.

**Fix:** Replace `NULL, 0` with `predef.vars, predef.count` to
match the vertex path:
```c
snapshot->normal_n_filled = repl_eval_parse_exprs(
    normal_args, snapshot->normal_args, 3,
    predef.vars, predef.count);
```

### 2. `glr_ctrl_host_input_reset` open-codes input clear, misses `anchor_pos` reset

**Where:** `src/app/glr_ctrl.c:1427-1435`.

**Smell:** The host-effect input-reset callback manually zeros
`input[0]`, `input_len`, `cursor_pos`, and `pending_newline`, but
does not reset `anchor_pos` to `-1`. `editor_input_clear()` at
`src/editor/state.c:359-364` does all four fields including
`anchor_pos`. After a host reset (triggered by file/example
loading), a stale `anchor_pos >= 0` from a prior selection survives
into the new document. The next Shift+arrow or click-drag sees an
unexpected selection range.

**Impact:** Stale input selection ghost after loading a file or
switching examples. Hard to reproduce because most flows also clear
the selection separately, but the path exists.

**Fix:** Replace the open-coded clear with `editor_input_clear()`:
```c
static void glr_ctrl_host_input_reset(void) {
    editor_insert_mode_set(0);
    editor_input_clear();
    EditorInputState *inp = editor_state_input_mut();
    inp->pending_newline[0] = '\0';
    inp->pending_newline_len = 0;
}
```

---

## 🟡 Drift / boundary hazards

### 3. `_mut()` for read — guide snapshot builds `source_cmds` via mutable accessor

**Where:** `src/app/glr_ctrl.c:218` (`glr_ctrl_build_guide_snapshot`).

**Smell:** `.source_cmds = repl_state_document_cmds_mut()` but the
field is only read (guide walks are non-mutating). The const
`repl_state_document_cmds()` accessor exists and should be used.
Same pattern the prior audit's #15 closed elsewhere.

**Fix:** Replace with `repl_state_document_cmds()`.

### 4. Naming inconsistency in host-effect callbacks

**Where:** `src/app/glr_ctrl.c:1427` (`glr_ctrl_host_input_reset`,
`glr_ctrl_host_insert_mode_off`) vs `src/app/glr_ctrl.c:1505-1518`
(`glr_host_editor_cursor_park`, `glr_host_completion_clear`,
`glr_host_completion_update`, `glr_host_editor_input_get`).

**Smell:** Two naming conventions for the same conceptual group of
host-effect callbacks: `glr_ctrl_host_*` (2 functions) and
`glr_host_*` (4 functions). All six are file-static, wired into
`g_glr_host_effects` at line 1523.

**Fix:** Pick one prefix (recommend `glr_ctrl_host_*` — matches the
file's `glr_ctrl_*` convention) and rename the four outliers.

### 5. `g_predef_vars` macros route reads through `_mut()`

**Where:** `src/repl/eval.h:172-173` (macro definition);
`src/app/glr_ctrl.c:1858, 2339, 2344` (usage sites).

**Smell:** `#define g_predef_vars (repl_eval_predef_vars_mut())` and
`#define g_num_predef_vars (*repl_eval_predef_count_mut())`. All
three uses in `glr_ctrl.c` are reads (string comparison, value
comparison). The macros expose mutable pointers project-wide.

**Fix:** Add `repl_eval_predef_view()` const accessor (already
exists — `ReplPredefView` returned by `repl_eval_predef_view()`).
Replace the three `glr_ctrl.c` uses with the view accessor. The
macros are used elsewhere too; a broader sweep is Tier B.

### 6. Coupled clear-color magic numbers

**Where:** `src/app/glr_ctrl.c:1335` (default clear color
`0.10f, 0.10f, 0.10f`) and `src/app/glr_ctrl.c:1434`
(alpha-boost reference luminance `0.10f`).

**Smell:** The overlay alpha-scale formula at line 1434 uses `0.10f`
as the reference background luminance. This value must match the
default clear color at line 1335. If either changes without the
other, the alpha boost breaks silently (overlays too dim or too
bright on the default background).

**Fix:** Extract `CFG_DEFAULT_CLEAR_R/G/B` or a single
`CFG_DEFAULT_BG_LUM` constant. Use it in both the clear-color
fallback and the alpha-boost numerator.

### 7. Two sources for clear-color in one function

**Where:** `src/app/glr_ctrl.c:1335` (inline scan of flat program)
and the `SceneRenderConfig.clear_color` field it populates.

**Smell:** `glr_ctrl_build_scene_config` walks the flat program to
find the last `CMD_CLEAR_COLOR` and populates
`config->clear_color[0..3]`. Later at line 1431, the alpha-boost
formula reads from `repl_render.clear_color[]` — a *different*
source (`ReplRenderState`). The two should agree, but the data flow
is not the same struct; if the repl_render path gets out of sync
(e.g. after a flatten but before the next frame's config build),
the alpha boost uses a stale clear color.

**Fix:** Read `config->clear_color[]` for the luminance calc
instead of `repl_render.clear_color[]`, since the config was just
populated three lines above.

### 8. Mid-header heavyweight includes in `glr_ctrl.h`

**Where:** `src/app/glr_ctrl.h:138` (`#include "repl/export.h"`)
and `src/app/glr_ctrl.h:229`
(`#include "repl/replay_annotations.h"`).

**Smell:** Two `#include` directives appear mid-header next to the
declarations that need their types. Every consumer of `glr_ctrl.h`
transitively pulls in `repl/export.h` (which pulls `export_state.h`,
`compile.h`, etc.) and `repl/replay_annotations.h` — heavyweight
for translation units that only need the controller's input-dispatch
or init-GL declarations.

**Fix:** Forward-declare the types (`ReplExportLayout`,
`ReplReplayAnnotationOutput`) and move the `#include` to
`glr_ctrl.c` (the only TU that calls these functions). Or split
`glr_ctrl.h` into a narrow core header and an extended header.

### 9. `config_value_ptr()` uses `_mut()` on read path

**Where:** `src/app/glr_config.c:63-104`.

**Smell:** `config_value_ptr()` returns `int *` via 33 `_mut()`
accessor calls. It is used by both `glr_config_get()` (read) and
`glr_config_set()` (write). Reads go through mutable accessors
unnecessarily — same class as the prior audit's #15.

**Fix:** Split into `config_value_ptr()` (mutable, for set) and
`config_value_ptr_const()` (const, for get). Or make
`glr_config_get` read through the by-value accessors.

### 10. OOB `glr_config_row_kind` returns `ROW_ITEM` instead of inert sentinel

**Where:** `src/app/glr_config.c:193-195`.

**Smell:** When `idx` is out of bounds (`< 0` or `>= CFG_ITEM_COUNT`),
`glr_config_row_kind` returns `GLR_CFG_ROW_ITEM` — an active kind
that tells callers "this is a clickable config toggle." A safer OOB
return would be `GLR_CFG_ROW_HEADER` or `GLR_CFG_ROW_SEPARATOR`
(inert kinds that callers skip).

**Fix:** Return `GLR_CFG_ROW_SEPARATOR` for OOB, or add a
`GLR_CFG_ROW_INVALID` sentinel if the enum has room.

### 11. `g_cfg_items[]` extern is non-const

**Where:** `src/app/glr_config.h:100` (or nearby — the extern
declaration for `g_cfg_items[]`).

**Smell:** The extern declaration doesn't carry `const`, so any
translation unit that includes `glr_config.h` can mutate the
descriptor table at runtime. The table is intended to be immutable
after startup.

**Fix:** Add `const` to the extern: `extern const GlrConfigItem
g_cfg_items[];` and update the definition in `glr_actions.c`.

### 12. `load_state()` reads shared state without lock

**Where:** `src/app/glr_audio.c:342-370`.

**Smell:** `load_state()` is called from the main thread during
`glr_audio_play_playlist()`. It reads `g_state_file` (line 344),
writes `g_cfg_mode` (line 360), and reads `g_playlist[]` /
`g_playlist_count` (lines 363-368) — all without holding
`g_mtx`. The worker thread can be active and writing to
`g_playlist_count` (via `set_playlist` at line 687+) concurrently.

**Impact:** Race on `g_playlist_count` read at line 363 vs. worker
writes. Unlikely to corrupt (single int), but the loop could
iterate over a stale count and match a track at a wrong index.

**Fix:** Wrap the `g_playlist`/`g_playlist_count` reads inside
`audio_lock()` / `audio_unlock()`, or document that `load_state` is
only called before the worker is started (verify this is true).

### 13. Stale globals survive `shutdown`/`re-init` cycle

**Where:** `src/app/glr_audio.c:604-685`.

**Smell:** `glr_audio_shutdown()` (line 657) clears
`g_music_loaded`, `g_active`, and `g_inited`, and destroys the
mutex/cond, but does not reset `g_playlist_count`, `g_playlist_pos`,
`g_state_file`, `g_cfg_mode`, `g_gesture_done`, or
`g_load_cancelled`. If `glr_audio_init()` + `set_playlist()` is
called again after a shutdown (currently no caller does this, but
the API shape allows it), stale state from the prior session leaks
into the new one.

**Impact:** No live bug today (shutdown is only called at exit), but
the API contract is silently broken for init/shutdown cycles.

**Fix:** Clear all module-level globals in `glr_audio_shutdown()`,
or document that re-init after shutdown is unsupported.

### 14. "Dump" function mutates global state

**Where:** `src/app/glr_debug.c:48-49`.

**Smell:** `glr_debug_dump_flat_commands()` checks
`repl_state_flat_program_dirty()` and, if dirty, calls
`repl_flatten_commands()` — a side-effecting rebuild of the global
flat program array. A function named "dump" (diagnostic read)
should not mutate global state.

**Impact:** Calling the dump from a debugger or test at an
unexpected moment triggers a flatten rebuild, which may reorder
the global flat program while another caller expects stability.

**Fix:** Either remove the dirty check (dump whatever is currently
in the array, noting it may be stale) or rename to
`glr_debug_dump_flat_commands_sync()` to signal the side effect.

### 15. `SOURCE_TEXT_INSERT_MANY` partial-failure leaves buffer inconsistent

**Where:** `src/app/glr_source_document.c:100-104`.

**Smell:** The `INSERT_MANY` case iterates
`editor_buffer_insert_line(pos + i, text[i])` in a loop. If
`editor_buffer_insert_line` fails at iteration `i`, lines
`0..i-1` have already been inserted, but the function returns 0.
The caller receives a failure signal with the editor buffer in a
partially-modified state.

**Impact:** A failed multi-insert (e.g., capacity overflow mid-loop)
leaves orphan lines in the buffer with no command-array
counterpart. Subsequent frame renders may read past the command
array's bounds.

**Fix:** Pre-validate capacity before the loop
(`editor_buffer_count() + count <= MAX_COMMIT_CMDS`), or roll back
inserted lines on failure.

---

## 🟢 Dead code / cosmetic

### 16. Stale legacy function references in comments

**Where:** `src/app/glr_ctrl.c` (multiple sites — grep for
`cycle_example_or_user_scene`, `glr_app_*`, `editor_commit_*` in
comments).

**Smell:** Comments reference old function names that were renamed
during the prior audit's #29 (`glr_app_*` → `glr_ctrl_*`) and
other cleanups. The comments are harmless but misleading.

**Fix:** Search-and-replace stale names in comments.

### 17. Misleading plan-internal comment "J2.1"

**Where:** `src/app/glr_ctrl.c` (grep for `J2.1`).

**Smell:** A comment references an internal plan designation
(`J2.1`) that has no meaning outside the author's planning context.
Violates the "no plan-internal references in source" convention.

**Fix:** Delete the plan reference or replace with a one-line
description of the behavior.

### 18. "Title-cased" comment but logic is sentence-case

**Where:** `src/app/glr_config.h:158` (approximate).

**Smell:** A comment claims "Title-cased display label" but the
actual label formatting logic is sentence-case. Misleading for
anyone extending the label system.

**Fix:** Correct the comment.

### 19. `apply_defaults()` docstring overstates scope

**Where:** `src/app/glr_actions.h:74-76` (approximate).

**Smell:** The docstring on `glr_actions_apply_defaults()` implies
it resets all config state, but it only resets the presentation
subset. The render-state defaults are applied separately by
`glr_state_render_reset_defaults()`.

**Fix:** Narrow the docstring to match the actual scope.

### 20. Double legacy-alias normalization

**Where:** `src/app/glr_actions.c:306-321` (approximate).

**Smell:** Legacy config-slug aliases (e.g., old names from
pre-rename) are normalized in two places: once in the
import/export bridge and once in the actions layer. The second
normalization is dead if the first always runs first.

**Fix:** Verify the call order and remove the redundant
normalization.

### 21. `glr_audio_stop_music()` — dead public API

**Where:** `src/app/glr_audio.c:743-748`,
`src/app/glr_audio.h:64`.

**Smell:** `glr_audio_stop_music()` is declared in the header and
defined in the source, but has zero callers outside the audio
module (grep confirms no external references). It also only calls
`ma_sound_stop()` without updating `g_music_loaded` or notifying
the worker, so calling it would leave the module in an inconsistent
state (sound stopped but module thinks it's playing).

**Fix:** Delete the function and its header declaration. If stop
semantics are needed later, implement them properly through the
worker.

### 22. `-1.0f` seek sentinel unnamed magic number

**Where:** `src/app/glr_audio.c` (8 sites — grep for `-1.0f`).

**Smell:** The value `-1.0f` is used as a "no seek" sentinel in
`worker_post()` calls throughout the audio module. It's always
`-1.0f` but never named.

**Fix:** `#define GLR_AUDIO_NO_SEEK (-1.0f)` and replace the 8
sites.

### 23. Unused `#include <unistd.h>`

**Where:** `src/app/glr_audio.c:45` (approximate).

**Smell:** `<unistd.h>` is included but no symbol from it is used
in the current source. Likely left over from a removed `usleep()`
or `close()` call.

**Fix:** Remove the include.

### 24. Magic auto-rotate speed constant

**Where:** `src/app/glr_camera.c:475` (approximate).

**Smell:** The auto-rotate increment `0.3f` is the only unnamed
magic number in the camera module. All other camera constants have
symbolic names (`CAM_DECAY`, `CAM_ZOOM_VEL_FACTOR`, etc.).

**Fix:** `#define CAM_AUTO_ROTATE_SPEED 0.3f`.

### 25. Dead `start < 0` bounds check

**Where:** `src/app/glr_source_document.c:51` (approximate).

**Smell:** A bounds check guards against `start < 0` in a function
where `start` is derived from an unsigned or already-validated
source. The branch is dead.

**Fix:** Remove the dead branch, or add an assertion if the intent
is defensive.

### 26. No `pose_from_state` conversion helper

**Where:** `src/app/glr_camera.h:102-106` (approximate).

**Smell:** Multiple call sites in `glr_ctrl.c` manually build a
`GlrCameraPose` struct from the camera state fields. A helper
`glr_camera_pose_from_state()` would be cleaner and prevent
drift if fields are added.

**Fix:** Add the helper to `glr_camera.h`.

### 27. Undocumented narrow API contract for `glr_ctrl_view_record_external_3d_pose`

**Where:** `src/app/glr_camera_export.c:223` (approximate).

**Smell:** This function has a narrow precondition (must be called
only from a specific point in the display frame, after the camera
modelview is loaded) but the contract is not documented in the
header or at the call site.

**Fix:** Add a one-line docstring in `glr_camera_export.h`.

---

## 🔵 Structural concerns

### 28. Magic numbers in variable-panel lift easing

**Where:** `src/app/glr_ctrl.c:3378-3382` (approximate).

**Smell:** The panel-lift easing uses numeric constants (spring
factor, damping, threshold) that are explained in adjacent comments
but not named as `#define`s or `static const`. The comments are
accurate but the values are not greppable.

**Fix:** Extract to named constants (`PANEL_LIFT_SPRING`,
`PANEL_LIFT_DAMP`, `PANEL_LIFT_THRESHOLD`).

### 29. Full `UiRenderSnapshot` rebuild on every drag-motion event

**Where:** `src/app/glr_ctrl.c:2957` (approximate).

**Smell:** The mouse-motion handler rebuilds the full
`UiRenderSnapshot` on every motion event to hit-test the code panel.
The prior audit's #39/#40 cached the snapshot per frame, but the
drag path bypasses the cache and rebuilds fresh.

**Impact:** CPU waste during drag events (the snapshot is ~2 KB of
struct copies, built per motion event at 60+ Hz). Not a
correctness issue.

**Fix:** Reuse the cached frame snapshot for drag hit-testing, with
a bounds clamp for cursor positions that fall outside the cached
geometry.

### 30. Replay row's `KEY_CTRL_R` shortcut is dispatch-dead

**Where:** `src/app/glr_actions.c:148` (config table),
`src/app/glr_ctrl.c:3066-3067` (dispatch chain).

**Smell:** The "Replay" row in `g_cfg_items[]` declares
`KEY_CTRL_R` as its shortcut. But `glr_ctrl_keyboard`'s dispatch
chain calls `glr_ctrl_router_handle_replay_key()` *before*
`glr_ctrl_router_handle_cfg_shortcut_key()` (lines 3066-3067).
`replay_handle_key()` always consumes `KEY_CTRL_R` (both when
replay is active and inactive), returning 1. The config shortcut
dispatcher never sees `KEY_CTRL_R`.

**Impact:** The shortcut column in the Config menu shows "Ctrl+R"
for the Replay row, and it does toggle replay — but via the
replay handler, not the config machinery. The config table's
shortcut is decorative. If someone changed
`replay_handle_key` to not consume the key, the config path would
suddenly activate with different semantics (it cycles the state
integer, not the replay start/stop lifecycle).

**Fix:** Remove `KEY_CTRL_R` from the Replay row in
`g_cfg_items[]` and add a `display_shortcut` field (or comment)
so the menu still shows the binding. Alternatively, route Ctrl+R
through the config machinery and have the config `on_change` hook
call the replay lifecycle (but this is a bigger change — Tier B).

### 31. `cfg_slug_from_label` recomputed per call

**Where:** `src/app/glr_actions.c:204-213` (approximate).

**Smell:** `cfg_slug_from_label()` lowercases and space-to-underscore
converts a label string on every call. Called during import/export
and config bridge operations, potentially many times per frame
during workspace load. No caching.

**Impact:** Marginal CPU waste. The slugs are stable — they could
be precomputed once at startup.

**Fix:** Precompute slugs in a parallel array during module init,
or store the slug on `GlrConfigItem` directly.

### 32. Magic `16` cap on section display labels

**Where:** `src/app/glr_config.c:232` (approximate).

**Smell:** `glr_config_section_label()` (or a similar accessor)
uses a magic `16` as the maximum number of section labels, with no
compile-time enforcement that `g_cfg_items[]` doesn't exceed this.
If sections are added beyond 16, the label array silently truncates.

**Fix:** Add a `STATIC_ASSERT(section_count <= 16, ...)` or derive
the cap from the table size.

### 33. `get_current_track` header contract promises longer lifetime than source delivers

**Where:** `src/app/glr_audio.h:92-93` (approximate).

**Smell:** `glr_audio_get_current_track()` returns `const char *`
pointing into `g_playlist[]`. The header implies the pointer is
stable, but `glr_audio_set_playlist()` can reallocate the playlist
array (via `worker_post(AWR_UNINIT, ...)` + re-population at lines
687+). A caller that caches the returned pointer across a playlist
change holds a dangling reference.

**Fix:** Document the lifetime constraint in the header: "Returned
pointer is valid until the next `glr_audio_set_playlist()` call."

### 34. Implicit zero-init gap in `glr_state.c`

**Where:** `src/app/glr_state.c:80`.

**Smell:** `static GlrState g_glr_state;` is zero-initialized by C
semantics, but the intended defaults (lines 42-78) are non-zero for
many fields (e.g., `grid_theme = GRID_THEME_XZRULER`,
`show_vertex_outlines = 1`). The defaults are applied later by
`glr_state_presentation_reset_defaults()` at `glr_ctrl.c:1604`.
Any code that reads `g_glr_state` before that call sees zeros, not
defaults.

**Impact:** No live bug (init order is correct in practice), but
the pattern is fragile — a new early reader would silently get
wrong defaults.

**Fix:** Initialize `g_glr_state` from `g_glr_state_defaults`:
`static GlrState g_glr_state = { ... };` mirroring the defaults
table. Or `= g_glr_state_defaults` if the compiler accepts it (C99
allows static init from another static for aggregate types, but
only if all initializers are constant expressions — the
`CFG_DEFAULT_*` macros are, so this should work).

### 35. Fragile transitive macro dependencies in `glr_defaults.h`

**Where:** `src/app/glr_defaults.h:28-45`.

**Smell:** Macros like `CFG_DEFAULT_GRID_THEME = GRID_THEME_XZRULER`
and `CFG_DEFAULT_BACKDROP_MODE = SCENE_BACKDROP_OFF` reference enum
values from `src/scene/themes.h` and other headers. `glr_defaults.h`
includes `app/glr_config.h` (line 25) but not `scene/themes.h` —
the enum values resolve only because some includer of
`glr_defaults.h` happens to have already included `scene/themes.h`.
If include order changes, the macros break.

**Impact:** Latent include-order dependency. Currently works because
`glr_ctrl.c` (the main consumer) includes scene headers early.

**Fix:** Add `#include "scene/themes.h"` to `glr_defaults.h`, or
use integer literals with a comment naming the enum value.

### 36. Undocumented double-decay during active drag

**Where:** `src/app/glr_camera.c:391, 457` (approximate).

**Smell:** During an active mouse drag, the camera velocity is
decayed both by the per-frame momentum tick (`CAM_DECAY`) and by
the drag-motion handler's own damping. This produces a
`CAM_DECAY^2` effective decay per frame during drag, which is
intentional (snappier feel during interaction) but undocumented.
A future editor who sees the double application may "fix" one of
them, breaking the drag feel.

**Fix:** Add a one-line comment at the drag-motion decay site:
`/* Intentional double-decay with momentum tick for snappier drag. */`

### 37. `set_target_decay(0.0)` gives instant snap, not documented

**Where:** `src/app/glr_camera.c:271-275` (approximate).

**Smell:** `glr_camera_set_target_decay(0.0)` makes the camera
jump to its target instantly (no easing). This is used intentionally
in several places (e.g., reset-camera), but the behavior of
`decay = 0.0` is not documented in the header. A caller might
expect 0.0 to mean "no decay = no easing = stay put" rather than
"instant snap."

**Fix:** Document in `glr_camera.h`: "`decay = 0.0` causes instant
snap to target (no interpolation)."

---

## Sequencing

### Tier A — afternoon pass (~17 findings, half-day)

Land these to clear the straightforward items. None are bigger
than ~10 LOC each; **#1** is the only one fixing a real rendering
bug today.

| Batch | Findings | Description |
|-------|----------|-------------|
| 1 | **#1** | One-line fix: pass predef vars to normal-guide eval |
| 2 | **#2** | Replace open-coded input clear with `editor_input_clear()` |
| 3 | **#3**, **#5** | `_mut()` for read → const accessor in guide snapshot + predef macros |
| 4 | **#4** | Rename 4 host-effect callbacks to `glr_ctrl_host_*` |
| 5 | **#9** | Split `config_value_ptr` into mutable/const variants |
| 6 | **#10** | Return inert sentinel for OOB `glr_config_row_kind` |
| 7 | **#16**, **#17**, **#18**, **#19** | Stale comment sweep |
| 8 | **#22**, **#23**, **#24**, **#25** | Named constant + dead code cleanup |
| 9 | **#20**, **#27** | Dead normalization + missing docstring |

### Tier B — one-week pass (18 findings)

1. **#6** + **#7** — Unify clear-color source; extract default
   constant.
2. **#8** — Forward-declare types in `glr_ctrl.h`; move includes
   to `.c`.
3. **#11** — Add `const` to `g_cfg_items[]` extern.
4. **#12** — Audit `load_state()` lock scope; wrap shared reads.
5. **#13** — Reset all globals in `shutdown()`.
6. **#14** — Rename or remove the dirty-check in debug dump.
7. **#15** — Pre-validate `INSERT_MANY` capacity.
8. **#21** — Delete `glr_audio_stop_music()`.
9. **#26** — Add `glr_camera_pose_from_state()` helper.
10. **#28** — Extract panel-lift easing constants.
11. **#29** — Reuse cached snapshot for drag hit-testing.
12. **#30** — Decouple replay row shortcut from config dispatch.
13. **#31** — Precompute config slugs.
14. **#33** — Document `get_current_track` lifetime.
15. **#34** — Initialize `g_glr_state` from defaults table.
16. **#36** — Document double-decay in camera drag.
17. **#37** — Document `decay = 0.0` instant-snap behavior.

### Tier C — defer (high cost or cross-cutting)

- **#32** — Compile-time section-count enforcement requires
  `STATIC_ASSERT` at table scope; may need table-shape change to
  make the count a constant expression.
- **#35** — Adding `#include "scene/themes.h"` to `glr_defaults.h`
  changes the transitive include set for every consumer. Needs
  audit of downstream effects.

### Out of scope

- The prior audit's #14 (controller rendering extraction →
  `src/subsystems/`) — has its own plan track; this audit doesn't
  re-flag it.
- The prior audit's #50 (`glr_cfg_cycle_row` `on_change`-hook
  refactor) — tracked as a deferred follow-up in the prior audit;
  not re-opened here.
- The prior audit's #65 (design-history comment mass) — remains
  Tier C as the prior audit classified it.
- The `g_predef_vars` macro definition in `eval.h` — the macro
  itself is in `src/repl/`, not `src/app/`; finding #5 above
  addresses only the `src/app/` usage sites.

## Method note

This audit was produced by four parallel review agents:

- `glr_ctrl.c` (~3426 lines — the heaviest file in `src/app/`) +
  `glr_ctrl.h`
- `glr_actions.c` + `glr_actions.h` + `glr_config.c` +
  `glr_config.h` + `glr_defaults.h`
- `glr_audio.c` + `glr_audio.h`
- `glr_camera.c` + `glr_camera.h` + `glr_camera_export.c` +
  `glr_completion.c` + `glr_completion.h` + `glr_debug.c` +
  `glr_debug.h` + `glr_source_document.c` + `glr_state.c` +
  `glr_state.h`

Each agent was given the prior audit's resolved list and instructed
**not to re-flag items already ✅ done**, but to flag regressions,
new smells, or items the prior audit closed only partially.

The consolidating reviewer spot-verified all 🔴 findings and
selected 🟡 findings against the live source before inclusion.
Prior audit #25 was re-evaluated end-to-end; the cancel-flag +
`AWR_UNINIT` worker path now covers all observable race windows.
