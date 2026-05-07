# Landed feature plans

This directory archives plans whose contracts shipped. Files are
preserved verbatim — they double as design history and the audit trail
for the hard guards that lock the contracts in (`make
check-state-ownership`).

| Plan | Landed | What it shipped |
|---|---|---|
| `editor-text-model-controller.md` | 2026-05-03 → -05 | M/V/C+compiler+router contract: passive `UiHit`, `imrepl_ctrl` routes, editor / peers / scene each own behavior, `repl_compile`/`repl_apply` pure |
| `editor-owns-text-completion.md` | 2026-05-03 → -05 | Three-layer ownership split (Editor / REPL / UI). Phases A–J including J1 input-dispatch boundary, J2 code-panel hit dispatch, J3 color-picker writeback through editor commit + replay-UI prefix discipline, J4 cursor-pixel `UiCodePanelOutput`, J5–J7 forwarder ratchets to 0/0, J8 macOS Cmd, J9 metadata-driven highlight |
| `editor-owns-text-completion-revised.md` | 2026-05-05 | Sibling to the above; corrected controller boundary (no `UiAction` dispatch enum). |
| `editor-ownership-gap-cleanup.md` | 2026-05-05 | Audit / ratchet branch that delivered the J-phase work. Hard guards: 32. |
| `push-architecture-ui.md` | 2026-05-04 | `ui_*_render*()` consumes `const UiRenderSnapshot *snap` built once per frame. Render-time write-backs via `UiCodePanelOutput`. Replay HUD now `replay_ui_hud.{c,h}` under feature-UI prefix. |
| `repl-cleanup-punch-list.md` | 2026-04-29 | Tiers 1–3 of mechanical / pattern / structural extractions. Listed here for historical context per the doc's own "preserved as historical context" note. |
| `back-drop-scenes.md` | (PR #24) | Backdrop modes + cityscape / stars renderers. |
| `add-fixed-array-support.md` | 2026-05-06 | Fixed scratch arrays `A/B/C[REPL_SCRATCH_ARRAY_LEN]` end-to-end — REPL state, parser/eval/flatten/executor, export round-trip, autocomplete + help integration, on-demand emit in `output.c`. |
| `modules-editor-view-update.md` | 2026-04-30 | Replacement language + Mermaid diagram for `MODULES.md` post-corrected-contract. The replacement has been applied. |

Plans not in this directory are still active or proposal-only; see
the parent `feature/` directory.
