# Partial Plan Archive

This directory holds plans where part of the work landed in `main` and
the rest is **deferred on purpose** — paused pending a future review,
prerequisite, or priority shift, not abandoned. Files stay verbatim so
the deferred phases retain their context and a future reader can resume
from the same baseline that paused them.

A plan moves here from `plans/active/` when enough phases have landed
that the work delivers value standing on its own and the remaining
phases are explicitly deferred. It promotes to `plans/done/` when every
phase lands; it moves back to `plans/active/` if implementation resumes
on the deferred phases.

---

## `editor-demo.md` — SRP split for code-panel UI + editor demo

**Landed** (Phases 0-4 + 7a):

- New generic text-panel module `src/ui/text_panel.{c,h}` with supporting
  `text_layout.{c,h}` and `text_search.{c,h}`, all REPL-free.
- New REPL adapter `src/ui/repl_code_panel.{c,h}` absorbing the
  REPL-aware half of the deleted `src/editor/code_panel_document.c`.
- `src/ui/panels.c` reduced from ~1500 to ~160 lines — now just
  overlay-priority hit routing and the scene-status banner.
- Virtual-row hit routing split cleanly: generic panel leaves
  `line_idx` unresolved, REPL adapter rewrites it before the
  controller routes the hit.
- `scripts/check-ui-text-panel-pure.sh` + `make check-ui-text-panel-pure`
  guard, wired into `make check`. Fails on `repl/`, `src/editor/`,
  `GLCmd`, `CmdType`, or `CMD_*` references in `src/ui/text_panel.*`.
- `tests/test_ui_text_panel.c` (25 tests) drives the generic renderer
  and hit-test against fabricated snapshots under `USE_GL_STUBS=1`,
  locking the contract independently of the REPL pipeline.
- `src/ui/text_panel.h` field docs tightened (`background_active`,
  `left_marker_active`, `UiTextPanelRightAction.emphasized`).
- All 4271 stub-mode tests green; full `make check-state-ownership`
  clean.

**Open sub-item in 7a:** MODULES.md still references the deleted
`editor_code_panel_document` module at lines 325, 548, and 883, and
does not yet list the new `ui_text_panel` / `ui_repl_code_panel`
modules. Five-minute follow-up.

**Deferred** (Phases 5, 6, 7b):

- **Phase 5 — Editor REPL/chrome decoupling.** Extend the existing
  `EditorServices` seam (`src/editor/services.h`, today scoped to
  compile/apply via `commit.c`) to cover the rest of the REPL-pipeline
  surface; add a complementary `EditorChromeServices` seam for
  `glr_camera_*`, `glr_ctrl_router_*`, `glr_ctrl_sync_ui_chrome`,
  and `glr_state_presentation*`. Target `input.c` and `commit.c`
  REPL surface reductions of 23 → ~5 and 33 → ~5 respectively.
- **Phase 6 — `editor_demo` binary** as a forcing function for module
  independence, parallel to `scene_demo` / `repl_demo`. A second
  binary that fails to link if the split regresses, turning "the
  module is independent" from a claim into a checkable invariant.
- **Phase 7b — surface guards + demo polish.** Wire
  `scripts/check-editor-repl-surface.sh` and
  `scripts/check-editor-chrome-surface.sh` (added in Phase 5) into
  `make check`; add the root-level `editor_demo` symlink; document
  the shim files as dependency ledgers against the service tables.

**Deferral reason.** The measured Phase 5 entry surface is roughly
85 REPL function calls across the six editor files — about 6× the
Phase 6 shim tripwire. Without first decoupling, the demo's shim
ships as a sprawling parallel implementation that defeats the
tripwire's purpose. The plan's own "Required Review Before
Implementation" section is the gate before Phase 5 starts: re-measure
against the current tree, redraw the service-table boundary, and
confirm the post-refactor surface fits the shim budget.

**Resuming.** Start by re-running the Phase 5 measurement table
against current sources, then re-evaluate whether the service-table
boundary or the tripwire budgets need adjustment before implementation.
