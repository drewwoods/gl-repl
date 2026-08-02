# Tutorials that check & set scene/feature state

## Context

The gl-repl tutorial system today teaches **OpenGL** only: every step is
`{comment, expected}` - the runner reveals an instruction comment and the user
must type the `expected` GL command to advance. The user wants tutorials to also
teach **gl-repl features** (view mode, vertex outlines, grid themes, …) in two ways:

1. **Check state (REQUIRE):** an instruction tells the user to enable a feature
   themselves (e.g. "Press F7 to turn on vertex outlines"); the step advances when
   the user sets that config to the target value. Introduces a feature by making the
   user perform the action.
2. **Set state (SET / showcase):** the step applies a config directly so the user
   immediately *sees* it ("// The radar grid looks like this." → grid set to Radar),
   shows a "Press Enter to continue" prompt, and advances on a keypress. The next
   step can showcase another value ("// The focus grid looks like this." → Focus).

Existing "type a GL command" steps keep working unchanged (now called **COMMAND**).

### Decisions confirmed with the user
- **Config restore on exit:** the tutorial is non-destructive - snapshot the config
  slugs the tutorial touches at start, restore them on exit/completion.
- **Advance keys for SET steps:** Enter, Tab, or Space.
- **REQUIRE already-satisfied:** auto-advance (never appear stuck).
- **Demo tutorial:** author one demonstrating the new step kinds.

## Key existing code being reused (do NOT reinvent)
- Catalog data/accessors/validator: `src/repl/tutorials.{h,c}`.
- Runner: `src/widgets/tutorial.c` (`tutorial_start`, `tutorial_emit_instruction_comment`,
  `tutorial_advance_after_successful_commit`, `tutorial_handle_commit_attempt`,
  `tutorial_step_instruction_line`, `instruction_line_for_step[]` label anchoring).
- Runtime state: `src/widgets/tutorial_state.{h,c}` (`tutorial_state_view/_mut/_reset`).
- **Config seam (layering):** config lives in the app layer (`glr_config_*`,
  `g_cfg_items[]`), but repl/widgets reach it only through the **installed config
  bridge** - `repl_export_config_bridge()` (`src/repl/export.h:118`) exposing
  `get_int(slug,fallback)` and `apply(ReplExportConfig*)`. Slugs (`grid`,
  `vertex_outlines`, `view_mode`, …) are the decoupled handle, exactly like `@cfg`.
- **Single user-driven config choke point:** F-keys, Ctrl-keys, and menu clicks all
  funnel through `glr_cfg_cycle_row()` (`src/app/glr_actions.c:370`).
- **Keyboard router chain:** `glr_ctrl_keyboard()` (`src/app/glr_ctrl.c:~3546`) runs
  `glr_ctrl_router_*` helpers before delegating to `editor_handle_key`.
- **Transient sandbox:** `tutorial_start` already enters a transient scene; we extend
  the sandbox to cover config.

## Design

### Step model (data)
Add a step *kind* alongside the existing *placement*:
```c
typedef enum { TUTORIAL_STEP_KIND_COMMAND = 0,
               TUTORIAL_STEP_KIND_SET,
               TUTORIAL_STEP_KIND_REQUIRE } TutorialStepKind;
```
Extend `TutorialStep` with three **trailing** fields (append, so positional
initializers in tests stay valid): `TutorialStepKind kind; const char *cfg_slug; int cfg_value;`.
- COMMAND: `comment` + `expected` (existing).
- SET/REQUIRE: `comment` + `cfg_slug` + `cfg_value`; `expected == NULL`.

The sentinel is re-keyed on **`comment == NULL`** alone (since SET/REQUIRE have no
`expected`). This is the central pitfall: `tutorial_step_at`, `repl_tutorial_step_count`,
and the validator's sentinel break currently key on `comment && expected`.

### Config slug seam (repl-layer wrappers)
Add two thin wrappers (impl in `src/repl/export.c`, declared in
`src/repl/state_owners.h` next to `repl_state_parse_workspace_header_line`):
```c
int  repl_cfg_get_int(const char *slug, int fallback);   /* bridge get_int */
void repl_cfg_set_int(const char *slug, int value);      /* one-item bag -> bridge apply */
```
`set_int` mirrors `parse_cfg` (`export.c:466`): build a one-entry `ReplExportConfig`
and call `bridge->apply`. Both no-op when no bridge is installed. **They call only
`repl_export_config_bridge()` + bag API + bridge function pointers - no `scene_*`/`glr_*`
calls, no scene/app includes** (satisfies `check-repl-export-via-bridge`).

### Runner control flow (widgets/tutorial.c)
- **Split** `tutorial_emit_instruction_comment` so the insert+lock+fade+`instruction_line_for_step`
  record stays shared, and the COMMAND-only tail (`expected_commit_line = line+1`,
  cursor placement, insert mode) moves into the kind branch. (Labels keep anchoring on
  the instruction-comment row for all kinds.)
- **New `static void tutorial_enter_step(int step)`** - resolve instruction line, emit
  comment, then branch on kind:
  - COMMAND: existing typing setup + `tutorial_set_step_status` + `editor_completion_update`.
  - SET: `repl_cfg_set_int(slug, value)`; `expected_commit_line = -1`; status
    "Press Enter to continue"; `editor_completion_update` (clears ghost).
  - REQUIRE: `expected_commit_line = -1`; status hint; **if already satisfied**
    (`repl_cfg_get_int(slug, fb) == value`) advance immediately.
- **Extract `static void tutorial_advance_to_next_step(void)`** from the body of
  `tutorial_advance_after_successful_commit`: clear pending/`expected_commit_line`,
  `step++`, read next comment (NULL ⇒ restore config + reset + "Tutorial complete"),
  else `tutorial_enter_step(step)`. Make the auto-advance **iterative** (loop), not
  recursive, so a chain of already-satisfied REQUIRE steps can't recurse.
  `tutorial_advance_after_successful_commit` (commit path) and
  `tutorial_notify_state_changed` (REQUIRE path) both call it.
- `tutorial_start`: call `tutorial_enter_step(0)`; **capture the config baseline**
  first (see below).
- **Public additions** (declare in `src/widgets/tutorial.h`):
  - `TutorialStepKind tutorial_current_step_kind(void);` (COMMAND when inactive).
  - `void tutorial_notify_state_changed(void);` - REQUIRE: advance if the watched slug
    now equals its target (slug-scoped; ignores unrelated config changes).
  - `int tutorial_handle_ack_key(unsigned char key);` - SET step + key ∈ {`\r`,`\n`,`\t`,` `}
    ⇒ advance, return 1; else 0.
  - `int tutorial_block_noncommand_commit(void);` - SET/REQUIRE ⇒ set a kind-appropriate
    status *inside tutorial.c* and return 1; COMMAND/inactive ⇒ 0. (Keeps the editor
    precheck from adding any new `repl_*` symbol - see ownership note.)

### Config snapshot / restore (non-destructive)
`fill_scene_subset` does **not** cover `view_mode`/`variable_panel`, so snapshot the
**exact slugs the tutorial's own steps reference** instead:
- File-static in `tutorial.c`: `{ char slug[REPL_EXPORT_CFG_KEY_MAX]; int orig; }`
  array + count (cap ~16 distinct slugs).
- `tutorial_start` (after transient entry): walk steps 0..N-1; for each SET/REQUIRE
  step, record each distinct `cfg_slug` with `repl_cfg_get_int(slug, fallback)`.
- On `tutorial_exit` and on the completion branch: write each saved `(slug, orig)`
  back via `repl_cfg_set_int`, then clear. (Other teardown paths - example/scene/
  workspace load, `glr_app_reset_all` - call `tutorial_state_reset()` directly and
  re-apply their own config, so they need no restore.)

### Wiring (app layer → widget, correct direction)
- `src/app/glr_actions.c`: `#include "widgets/tutorial.h"`; call
  `tutorial_notify_state_changed()` at the **tail** of `glr_cfg_cycle_row` (after the
  cycle + `glr_ctrl_sync_ui_chrome` + per-key status, so the tutorial status wins).
- `src/app/glr_ctrl.c`: add `int glr_ctrl_router_handle_tutorial_ack_key(unsigned char key)`
  returning `tutorial_handle_ack_key(key)` (consume → emit input effects); insert into
  the `glr_ctrl_keyboard` chain after existing controller routes, before
  `editor_handle_key`. (Config-menu router only consumes backtick, so an open menu
  during a REQUIRE step won't interfere; scope the ack consume strictly to SET steps.)
- `src/editor/input.c`: in `tutorial_precheck_current_input` (~`:1233`), short-circuit
  at the top with `if (tutorial_active() && tutorial_block_noncommand_commit()) {
  editor_completion_clear(); return 0; }` - replaces the misleading "Move cursor to the
  tutorial insertion line" message for SET/REQUIRE steps. **Adds only a `tutorial_*`
  call (no new `repl_*` symbol).**

### Validation (tutorials.c)
Make `repl_tutorial_validate_entry` kind-aware: sentinel break on `!comment`; then
- COMMAND: require `comment && expected`, run `expected_is_single_command`.
- SET/REQUIRE: require `comment`, non-empty `cfg_slug`, and `expected == NULL` (reject
  non-NULL with a clear diagnostic); skip `expected_is_single_command`.
- Keep label-uniqueness + placement (APPEND/LABEL) rules for all kinds.
Slug *validity* can't be checked at the repl layer (no bridge) - unknown slugs no-op at
runtime; note this in a comment.

### Demo tutorial (tutorials.c)
Add one short entry (e.g. "Feature Tour"): draw a triangle (COMMAND ×5) →
`STEP_REQUIRE(NULL, "// Press F7 to turn on vertex outlines.", "vertex_outlines", 1)` →
`STEP_SET(NULL, "// The radar grid looks like this.", "grid", 10 /* Radar */)` →
`STEP_SET(NULL, "// The focus grid looks like this.", "grid", 6 /* Focus */)`.
Use **integer** grid values with a naming comment - `tutorials.c` is repl-layer and
must not include `scene/themes.h`; the catalog already treats cfg values as opaque ints.

## Files to modify
| File | Change |
|------|--------|
| `src/repl/tutorials.h` | `TutorialStepKind`; 3 new `TutorialStep` fields; 3 accessors; doc/rule comments |
| `src/repl/tutorials.c` | macros (`STEP_SET`/`STEP_REQUIRE`, update existing+sentinel); re-key termination on `comment`; kind-aware validator; accessors; demo tutorial |
| `src/repl/export.c` | implement `repl_cfg_get_int`/`repl_cfg_set_int` (bridge-only) |
| `src/repl/state_owners.h` | declare the two `repl_cfg_*` wrappers |
| `src/widgets/tutorial_state.h` | (if needed) nothing required - snapshot lives file-static in tutorial.c |
| `src/widgets/tutorial.h` | declare `tutorial_current_step_kind`, `_notify_state_changed`, `_handle_ack_key`, `_block_noncommand_commit`; doc |
| `src/widgets/tutorial.c` | split emit; `tutorial_enter_step`; `tutorial_advance_to_next_step` (iterative); kind branches; cfg baseline capture/restore; commit-block hint |
| `src/editor/input.c` | precheck short-circuit via `tutorial_block_noncommand_commit` (no new `repl_*`) |
| `src/app/glr_actions.c` | include `widgets/tutorial.h`; notify hook in `glr_cfg_cycle_row` |
| `src/app/glr_ctrl.c` + `glr_ctrl.h` | `glr_ctrl_router_handle_tutorial_ack_key` in the keyboard chain |
| `tests/test_tutorial_runner.c` | new tests (below) |

## Ownership-check compliance (verified against the scripts)
The user asked specifically that no ownership checks break. Verified:
- **`check-repl-export-via-bridge`** - the new `export.c` wrappers call only
  `repl_export_config_bridge()` + bag API + bridge function pointers; no `scene_*(`/
  `glr_*(`, no scene/app includes. ✓
- **`check-editor-repl-surface`** (ratchets `input.c` at **21** unique `repl_*`
  symbols) - the precheck change routes through a `tutorial_*` widget call that sets
  status internally; `input.c` gains **zero** new `repl_*` symbols. ✓
- **`check-state-boundaries` / `check-views-no-owners`** - only restrict
  `SCENE_SRCS`/`UI_SRCS` (+`glr_ctrl.c`); `widgets/tutorial.c` already includes
  `state_owners.h`. Adding declarations there touches no scene/UI includer. ✓
- **`check-views-flat`** - scans only `state_views.h`/`ui_*.h`/specific scene headers;
  `TutorialStep` isn't scanned and const-pointer fields are allowed regardless. ✓
- **`check-module-prefixes`** (denylist) - `repl_cfg_*`, `repl_tutorial_step_*`,
  `tutorial_*`, `glr_ctrl_router_*` aren't denied; demo uses integer grid values (no
  `scene/themes.h` in repl). ✓
- **`check-domain-encapsulation`** - only scans `repl_state_<domain>_mut(`; none added. ✓
- Untouched guard surfaces: parser/compile/apply set-status, GL boundaries, feed_line/
  load_line_to_input in pipeline, source-document owners. ✓

## Testing & verification
Add to `tests/test_tutorial_runner.c` (harness installs the bridge via
`reset_fixture()`→`glr_app_reset_all`, so `repl_cfg_*` are live):
- Validator **accepts** well-formed SET and REQUIRE; **rejects** empty `cfg_slug` and
  non-NULL `expected` on SET/REQUIRE.
- `repl_tutorial_step_count` counts a mixed COMMAND+SET+REQUIRE entry correctly
  (termination now keyed on `comment`).
- SET applies its cfg on entry (`repl_cfg_get_int("grid",-1) == 10`) + shows the prompt;
  advancing via `glr_ctrl_keyboard('\n',…)` (covers the router) and via direct
  `tutorial_handle_ack_key` both advance + apply the next SET.
- REQUIRE advances when the watched slug reaches target via `glr_cfg_cycle_row(row,1)`;
  toggling a **different** config does **not** advance; already-satisfied auto-advances.
- Commit is rejected during SET/REQUIRE (`set_input_text(...)` + `editor_handle_key(';',…)`
  ⇒ step unchanged, no new doc line, SET hint shown - not "Move cursor…").
- Config **restore on exit**: set a slug as baseline, run a SET step that changes it,
  `tutorial_exit()` ⇒ slug back to baseline.
- Label-targeted COMMAND step still anchors correctly when a SET step sits between it
  and its target (guards the `instruction_line_for_step` interaction).

Gates to run: `make test_tutorial_runner test_tutorial_match`, then
`make check-c99`, `make check-state-ownership` (bundles the guards above), and
`make test`. Cross-check under real GCC on `gracemont` (`make check-c99 && make
test-stubs`) per CLAUDE.md.

## Risks / notes
- REQUIRE matches an **exact** value at notify time (not a transition); overshooting a
  multi-value cycle and returning still lands the match. Documented behavior.
- `editor_feed_line`-based full-walk loops in existing tests must not be pointed at the
  demo tutorial (they'd stall at a SET/REQUIRE step) - drive those via the ack/notify
  entries instead.
- Restore reverts **both** SET- and REQUIRE-applied slugs to the pre-tutorial value
  (simplest consistent "restore on exit" semantics).
