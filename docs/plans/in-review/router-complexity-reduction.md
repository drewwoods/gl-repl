# Router Complexity Reduction - glr_ctrl_router.c

## Status - drafted, awaiting design read

[`src/app/glr_ctrl_router.c`](../../../src/app/glr_ctrl_router.c) is 3,313
lines / 134 KB. It was carved out of `glr_ctrl.c` as "the input half", and
the carve-out was the right move - but it inherited more than routing.
Roughly a third of the file is *feature logic* that lives there only because
its trigger is a key or a click, and two pieces of load-bearing ordering
knowledge exist in duplicate. This plan reduces the file to what its header
comment already claims it is: pure routing.

The stages are ordered by payoff and are independently landable. Stage 1 is
mechanical extraction; Stage 2 is the highest nuance-reduction per line
changed; Stages 3-5 are optional polish that should only proceed if the
earlier stages leave them still worth doing.

## What stays put (explicit non-goals)

- **The eleven small `glr_ctrl_router_handle_*_key` handlers stay as named
  public functions.** Tests drive them individually (55 references in
  `test_glr_ctrl.c` alone; also `test_repl_editor.c`,
  `test_editor_input_selection.c`, others). The chain ordering in
  `keyboard_dispatch` is documented as load-bearing (config-owned Ctrl+R
  before replay forwarding), and several handlers are not simple key→action
  pairs (`handle_accum_samples_key` parses two keys,
  `handle_tutorial_ack_key` checks the tutorial step kind). A
  keymap-indexed dispatch table would trade a readable ladder for
  indirection plus a parallel ordering constraint. The per-handler shape
  *is* the simple version.
- **No behavior changes anywhere in this plan.** Every stage is a move or a
  re-expression; the existing tests are the acceptance gate, unmodified
  except for `#include`/link-list churn.

## Stage 1 - evict the non-routing bands (~700 lines)

Three bands are policy/persistence, not routing decisions. Each moves to
the module that owns the domain; the router keeps a thin handler that calls
the moved entry point.

### 1a. Scene/tutorial cycling (~lines 568-916, ~350 lines)

`cycle_try_examples`, `cycle_restore_origin`,
`cycle_report_skipped_examples`, `cycle_example_or_user_scene_dir`,
`tutorial_cycle_selected`, `glr_ctrl_cycle_peek`, `cycle_tutorial_dir`, and
the public `glr_ctrl_scene_cycle_next/prev` +
`glr_ctrl_tutorial_cycle_next/prev` wrappers. This is pure policy -
skip-error reporting, origin restore, the parked example place
(`example_place_idx`), tutorial retained index. None of it inspects an
input event.

Destination: [`src/app/glr_actions.c`](../../../src/app/glr_actions.c)
(which already owns the load-example/scene/workspace actions the cycle
calls into), or a new `src/app/glr_ctrl_cycle.c` if `glr_actions.c` is
judged too large to absorb it. The router keeps only
`glr_ctrl_router_handle_scene_cycle_special` /
`_tutorial_cycle_special` - two ~10-line F12/F11 handlers calling
`glr_ctrl_scene_cycle_next()` etc.

`glr_ctrl_cycle_peek` is public API (`glr_ctrl.h`) used by the peek HUD;
its declaration moves header-side with the implementation but keeps its
name, so callers are untouched.

### 1b. Quit/recovery persistence (~lines 147-247, ~100 lines)

`glr_ctrl_recovery_has_user_work`, `glr_ctrl_save_recovery_file`,
`glr_ctrl_save_recovery_workspace`, `glr_ctrl_save_quit_recovery`, plus the
SIGINT plumbing around `glr_ctrl_request_quit`. This is file-writing
lifecycle logic. Destination: a new `src/app/glr_recovery.c` (preferred -
it is a coherent single responsibility) or a corner of `glr_actions.c`.
The router keeps `glr_ctrl_router_handle_quit_key` and
`glr_ctrl_router_run_pending_quit` (the latter is the
`glr_ctrl_internal.h` seam the host calls; it shrinks to a call into the
moved code).

### 1c. Variable-panel drag value math (~lines 917-1156, ~240 lines)

`glr_ctrl_persist_variable_panel_drag_value` and
`glr_ctrl_apply_variable_panel_value_change` compute and persist slider
values - domain logic, not routing.
[`src/subsystems/variable_panel/variable_panel_drag.h`](../../../src/subsystems/variable_panel/variable_panel_drag.h)
already exists as the drag home; the value computation/persistence moves
there. The router keeps the thin begin/motion/release bridge
(`handle_variable_panel_drag_begin/_motion/_drag_release`), including the
shift-fine-modifier read (`glutGetModifiers` is input-side and belongs in
the router).

Watch: the variable_panel subsystem is editor/UI-independent today. If the
persist path reaches back into repl/editor state, the moved code takes a
callback or lands in an `src/app/` bridge file instead - do not give the
subsystem a new upward dependency to save a hop.

## Stage 2 - single source of truth for hit-test layering

`mouse_dispatch` runs four sequential hit passes for a left press:
above-gl-state front panels → gl-state popup surface → assign plot →
console → canonical `ui_panels_hit_test`. `router_press_routes_to`
duplicates the first legs, and its own comment concedes that anything
predicting a click "must consult that same order, or it speaks for pixels
another panel already owns." Two copies of load-bearing ordering, one of
them in a comment.

Fix: one `ui_panels_hit_test_layered()` in
[`src/ui/app/panels.c`](../../../src/ui/app/panels.c) returning
`{ UiHit hit; int layer; }`, with the layer enum naming the passes
(FRONT / GL_STATE_POPUP / ASSIGN_PLOT / CONSOLE / CANONICAL). The ui band
already owns paint order, and hit order must agree with paint order - so
the ordering lives next to the thing it must match. `mouse_dispatch`
consumes the layer to keep its per-layer side effects (popup click-away
dismiss, front-panel popup-close exception); `router_press_routes_to`
collapses to one call. The gl-state popup surface test
(`glr_ctrl_router_point_in_gl_state_popup`) is controller-side state, so
the layered test takes it as a bounds callback or the popup rect moves
into the snapshot - decide at implementation time, but the *order* moves
to ui/ either way.

This is the highest nuance-reduction-per-line stage: it deletes a
duplicated ordering and turns a comment-enforced invariant into a
compiled one.

## Stage 3 - table-drive the UiHit switch

`glr_ctrl_router_handle_code_panel_hit` is a 45-case switch where nearly
every arm is `consumed = route_X(...)`. Convert to a static table keyed on
hit kind with a uniform `int (*fn)(const UiHit *hit, int x, int y)`
signature (thin adapter shims for the handful that take extra baked
arguments, e.g. the `route_code_cfg_cycle_hit(GLR_CONFIG_*, +1)` rows).

The real win is the second column: the hand-maintained "exempt from
dropdown dismiss" chain (`kind != UI_HIT_MENU_BUTTON && ... &&
UI_HIT_CODE_PANEL_WORKSPACE_CHIP`) becomes a per-entry
`keeps_dropdown_open` flag. That chain is the bug farm - the workspace
chip was already added as an afterthought exemption per its own comment -
and a flag next to the handler cannot drift from it.

Note `UiHit.kind` spans two enums (core + app-band extensions in
`ui/app/hit.h`), which is why the dispatcher takes `int`; the table
should be one array over the combined range, not two.

## Stage 4 - shared key-dispatch prologue

`keyboard_dispatch` and `special_dispatch` repeat the same ladder:
`ui_state_command_description_close()` → `glr_modal_handle_*` →
`editor_input_rename_capture_*` → `editor_input_file_prompt_capture_*`,
each arm followed by `glr_ctrl_router_dismiss_gl_state_for_editor_input()`.
Factor one `router_modal_capture(key, is_special)` helper returning
consumed/not. Small (~30 lines saved) but it makes "hard modals come
first, and every captured key dismisses the gl-state popup" a single
stated fact instead of a pattern to notice.

## Stage 5 - split the remaining TU by input surface

After Stages 1-4 the file is ~2,000 lines still mixing three surfaces:
keyboard/special routing, mouse/hit routing, and the drag/double-click
machinery (scrollbar drag, input-row drag, char→line-range promotion, word
select, double-click clock test hook). Split into:

- `glr_ctrl_router_key.c` - keyboard/special handlers + dispatch
- `glr_ctrl_router_mouse.c` - mouse/motion/wheel dispatch + UiHit routing
- `glr_ctrl_router_drag.c` - drag state machine + double-click tracking

mirroring how `src/repl/` splits its pipeline TUs. Shared file-private
state (drag snapshot, double-click clock) gets a small
`glr_ctrl_router_internal.h`.

Seams that need explicit care:

- New files including `ui/` headers must join the
  `check-controller-boundaries` ui allowlist, or the guard fails.
- The Makefile filters the router object *by name*:
  `test_glr_ctrl_OBJS = ... $(filter-out ... $(OBJDIR)/src/app/glr_ctrl_router.o, ...)`
  (Makefile:1327). The filter list grows with the split, and any other
  by-name reference to `glr_ctrl_router.o` needs auditing.
- Public API stays in `glr_ctrl.h` unchanged; only statics move.

Stage 5 is worth doing only if Stages 1-2 land and the residue still
reads as three interleaved files. If Stage 1 alone brings the file under
~2,200 lines and the bands read cleanly top-to-bottom, stop.

## Verification per stage

- `make test-stubs` (includes the trailing-whitespace and duplicate-decl
  guards) and `make check-state-ownership` (boundary allowlists, c99,
  include style) after every stage.
- `make test` for the input-routing suites: `test_glr_ctrl`,
  `test_repl_editor`, `test_editor_input_selection`, `test_ui`,
  `test_ui_scene_tabs` are the ones that exercise the router directly.
- No golden or scene-corpus churn is expected at any stage; if a stage
  produces any, that stage changed behavior and must be re-examined.
