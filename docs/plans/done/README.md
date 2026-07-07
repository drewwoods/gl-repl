# Landed Plan Archive

This directory is the canonical archive for plans whose contracts
shipped. Files are preserved verbatim — they double as design history
and the audit trail for the hard guards that lock the contracts in
(`make check-state-ownership`).

Non-plan files also present: `ARCHITECTURE.md` (architecture reference),
`Next.md` (backlog snapshot), `OLD_PLAN` / `refactor_plan` (pre-git prose
notes), `design-rework/` (early design sketches). These are kept as
historical context and are not in the table below.

| Plan | Landed | What it shipped |
|---|---|---|
| `tender-exploring-dream.md` | 2026-02-24 | Early wishlist / open-items catalog; kept as design history. |
| `GLUTesselator.md` | 2026-02-24 | GLU tessellator support: `gluBegin/gluEnd/gluVertex/gluNormal/gluColor` commands with per-vertex normal and color data. |
| `recursive-func.md` | 2026-04-09 | Feasibility assessment for recursive functions and float-returning calls; recommended phased approach (args before control flow). |
| `ui_panels_wrap_and_top.md` | 2026-04-10 | Wrap-at-comma display toggle and vertical-layout mode option for the code panel. |
| `repl-core-cleanup.md` | 2026-04-11 | Phased extraction of search, export, and editor functionality from monolithic `repl_core.c` into focused sibling modules. |
| `repl-core-cleanup-refinements.md` | 2026-04-11 | Symbol inventory and cross-module dependency map used to plan the `repl_core` split. |
| `repl-core-cleanup-result.md` | 2026-04-11 | Outcome: 8750-line `repl_core.c` split into `repl_search.c`, `repl_export.c`, `repl_editor.c` with stable public API. |
| `single-export-geometry.md` | 2026-04-11 | Refactored exporter to emit shared geometry body once with pass-aware macro remapping for fill / outline / vertex-point passes. |
| `unify-hidden-gl-init-with-repl.md` | 2026-04-11 | Unified startup GL state via shared bootstrap command list rendered consistently in code-panel footer, dump, and export. |
| `reworking-camera-system.md` | 2026-04-12 | Orbit camera redesign: world-space target, XZ-plane right-drag panning, eased focus-origin reset, gizmo visualization. |
| `audio-support.md` | 2026-04-13 | miniaudio integration: MP3/WAV/OGG playback, playlist engine, persisted audio state, worker thread lifecycle. |
| `back-drop-scenes.md` | 2026-04-14 (PR #24) | Backdrop modes + deterministic cityscape and star-field renderers. |
| `cache-replay-commands-for-big-performance-lift.md` | 2026-04-14 | Fixed O(n²) replay annotation slowdown by caching flat-command lookups per replay frame. |
| `user-defined-var-names.md` | 2026-04-16 | `float name;` variable declarations: predef-var table registration, in-use guards for deletion, export round-trip. |
| `draw-translation-rotation-guides.md` | 2026-04-17 | 3D overlay guides for `glTranslatef` and `glRotatef` showing the transformation effect in local frame. |
| `benchmark-metrics.md` | 2026-04-18 | Extended REPL benchmarks with hardware performance counters (instructions, cycles, cache misses) and baseline-normalized metrics. |
| `build-warning-cleanup.md` | 2026-04-18 | Cleared `-Wformat-truncation` and `-Wstringop-truncation` warnings across the build. |
| `auto-apply-line-edits.md` | 2026-04-20 | Auto-commits edited lines on Up/Down navigation, restoring committed state if the edit is invalid. |
| `menu-reorg.md` | 2026-04-20 | Reorganized Scene and Config menus with semantic `### HEADER` sections and `--- DIVIDER` separators. |
| `multi-user-scenes.md` | 2026-04-20 | Expanded single-slot user scene system to 8 simultaneous editable scenes with LRU eviction and workspace folder save/load. |
| `repl-cleanup.md` | 2026-04-21 | Strategic refactor: 11 stages consolidating globals, ownership, and module boundaries into a clean baseline. |
| `avoid-black-crush-no-srgb.md` | 2026-04-23 | Scaled grid-line alpha when background luminance drops below design point to prevent black-crush visibility loss. |
| `repl-state-phase2-sketch.md` | 2026-04-23 | Design sketch for moving broad `g_*` access behind focused `ReplRuntimeState` typed accessors. |
| `repl-state-phase2-step-3.md` | 2026-04-23 | 10 domain-at-a-time storage migrations (document, flat program, camera, config, etc.) with per-slice verification. |
| `repl-state-phase2-substeps.md` | 2026-04-24 | Per-domain migration plan backing `repl-state-phase2-step-3`. |
| `repl-state-phase2-bridge-retirement.md` | 2026-04-24 | Completed storage migration and deleted `repl_state_compat.h` bridge after converting all callers to typed accessors. |
| `repl-cleanup-punch-list.md` | 2026-04-29 | Tiers 1–3 of mechanical / pattern / structural extractions. Preserved as historical context per the doc itself. |
| `push-architecture-ui.md` | 2026-04-29 | `ui_*_render*()` consumes `const UiRenderSnapshot *snap` built once per frame; render-time write-backs via `UiCodePanelOutput`; replay HUD moved to `replay_ui_hud.{c,h}` under feature-UI prefix. |
| `push-architecture.md` | superseded | Original generic-scene-plugin sketch. Superseded by `push-architecture-refinement.md`; kept as design history. |
| `push-architecture-refinement.md` | 2026-04-29 + follow-ups | Option B controller layer: frame orchestration + scene-config into `glr_ctrl.c`, scene/UI off REPL globals. Phase 1 + most of Phase 2 landed; archived because the architectural goal shipped. |
| `modules-editor-view-update.md` | 2026-04-30 | Replacement language + Mermaid diagram for `MODULES.md` post-corrected-contract. Applied. |
| `file-structure-cleanup.md` | 2026-04-30 | Formalized module layout rules: paired modules at root, header-only helpers in `include/`, tests under `tests/`. |
| `editor-owns-text-spike.md` | 2026-05-01 | Spike investigation: tested moving canonical text ownership from `GLCmd.source` to a parallel editor buffer. |
| `editor-owns-text-spike-results.md` | 2026-05-01 | Spike results: re-parsing all commands from editor buffer on flatten runs under 4.2 ms — acceptable. |
| `editor-owns-text.md` | 2026-05-01 | Design for migrating canonical command text to editor buffer and deleting `GLCmd.source`. |
| `editor-text-model-controller.md` | 2026-05-03 → -05 | M/V/C+compiler+router contract: passive `UiHit`, `imrepl_ctrl` routes, editor / peers / scene each own behavior, `repl_compile`/`repl_apply` pure. |
| `editor-owns-text-completion.md` | 2026-05-03 → -05 | Three-layer ownership split (Editor / REPL / UI). Phases A–J: input-dispatch boundary, code-panel hit, color-picker writeback, cursor-pixel output, forwarder ratchets, macOS Cmd, metadata highlight. |
| `editor-owns-text-completion-revised.md` | 2026-05-05 | Sibling correcting the controller boundary (no `UiAction` dispatch enum). |
| `editor-ownership-gap-cleanup.md` | 2026-05-05 | Audit / ratchet branch that delivered the J-phase work. Hard guards: 32. |
| `add-fixed-array-support.md` | 2026-05-06 | Fixed scratch arrays `A/B/C[REPL_SCRATCH_ARRAY_LEN]` end-to-end — REPL state, parser/eval/flatten/executor, export round-trip, autocomplete + help. |
| `editor-as-text-editor.md` | 2026-05-08 | Generic text-editor split: REPL mode becomes one configuration of a configurable editor with its own UI chrome. |
| `predef-var-compaction-on-apply.md` | 2026-05-08 | Moved predef-var compaction from editor input dispatcher into the compile/apply transaction layer. |
| `repl-agnostic-clipboard.md` | Phase A 2026-05-08; Phase B incidentally landed | Block-aware copy + decl-guard queries moved behind REPL-side predicates. Phase B (`GLCmd[]` storage dropped) satisfied incidentally. |
| `decouple-repl-from-gl-repl-alt.md` | 2026-05-09 | Decoupled `repl_demo` from GL/GLUT/UI/editor layers by eliminating 17 external symbol dependencies. |
| `source-document-port.md` | 2026-05-11 | Neutral source-document port decoupling REPL pipeline from editor; link-time isolation enforced by guard. |
| `editor-input-selection.md` | 2026-05-11 | Character-range input-buffer selection: `anchor_pos`, tagged clipboard (`EMPTY`/`LINES`/`INPUT_TEXT`), shift+arrow, double-click word select, per-character drag, Ctrl+C/X/V priority routing. 169 tests. |
| `tagged-example-submenus.md` | 2026-05-12 | Built-in examples organized into tagged submenus (`GEOMETRY`/`COLOR`/etc.) under Scene menu with flyout interaction. |
| `tutorial-system-extended.md` | 2026-05-12 | Extended tutorial runner with `SET` / `REQUIRE` step kinds for config-state showcase and checkpoint steps. |
| `tutorial-system-revised.md` | 2026-05-13 | Guided tutorial mode: catalog, peer state + runner, Tutorials menu, transient scene, Tab autofill, locked comments, fade-in rendering, `make check-state-ownership` coverage. |
| `editor-demo.md` | 2026-05-13 | Standalone `editor_demo` binary proving text-buffer editing is independent of the REPL pipeline via a minimal shim. |
| `gl-stub-extensions.md` | 2026-05-14 | `GL_STUB_TRACE_LINE` macro facility for per-call argument tracing in stub test builds. |
| `per-kind-argument-hue-shift-code.md` | 2026-05-15 | Per-kind syntax highlighting for arguments (literals, constants, variables) in code panel via color-segment spans. |
| `scene-file-menu-rework.md` | 2026-05-15 | Scene menu as pure selector; New/Save/Rename moved to File menu; save derives filename from scene name. |
| `tabbed-code-panel.md` | 2026-05-15 | Scene tab strip above code panel: click-to-switch, double-click-to-rename, snapshot-pure render derived each frame. |
| `glcolormask-gl-bool-tokens.md` | 2026-05-16 | Bool-slot variant (`GL_TRUE`/`GL_FALSE`) in generalized enum-arg system covering `glColorMask` and `glDepthMask`. |
| `grid-axes-transitions.md` | 2026-05-16 | Fade-in/fade-out transitions for grid and axes theme toggles via `SceneXnState` opacity state machine. |
| `c99-compatibility.md` | 2026-05-17 | `make check-c99` ratchet: `gcc -std=c99 -fsyntax-only` over all shipped sources, non-pedantic; implicit-function-decl still a hard error. |
| `runtime-point-parameter-detection.md` | 2026-05-17 | Replaced compile-time `NO_POINT_PARAMETER` macro with runtime GL-context detection and `GLR_NO_POINT_PARAMETER` env override. |
| `UI-Color-Theming-Infrastructure.md` | 2026-05-17 | Coherent color-theming system across UI chrome using token enums and centralized palette table in `theme.h`. |
| `post-processing-filter.md` | 2026-05-17 | Opt-in chromatic-aberration post-processing filter (experimental; hidden shortcut). |
| `code-panel-focus-mode.md` | 2026-05-18 | Ctrl+Shift+F / statusbar "focus" keycap hides code-panel chrome (menu bar, autocomplete, status banner). |
| `config-menu-submenu-sections.md` | 2026-05-18 | `### SECTION` flyout engine shared by Scene example tags, Config sections, and Tutorials; generic `CatalogFlyoutOps` vtable. |
| `module-naming-convention.md` | 2026-05-18 | `check-module-prefixes` guard enforcing the `repl_*`/`editor_*`/`glr_*`/`scene_*`/`ui_*` prefix convention. |
| `scene-color-convention.md` | 2026-05-18 | `src/scene/palette.h` with disciplined non-theme scene color tokens mirroring the UI `theme.h` structure. |
| `code-quality-refactor-followups.md` | 2026-05-19 | Residual code-quality findings from refactor audits requiring behavior-preserving structural changes. |
| `edit-line-ownership.md` | 2026-05-20 | Migrated active edit-line cursor ownership from `ReplState` to `EditorState` to align with layering invariants. |
| `src-shuffle-final.md` | 2026-05-23 | 8-phase source tree restructure: `prof`/`transform_utils` relocation, `subsystems/` split, `ui/core`+`ui/app` split, `sample→gl-repl` rename. |
| `src-editor-code-smell-audit.md` | 2026-05-23 | `src/editor/` audit: layering violations, modal-capture issues, state management — all Tier A/B fixes landed. |
| `src-repl-code-smell-audit.md` | 2026-05-23 | `src/repl/` first-pass audit: correctness bugs, naming drift, structural issues — all Tier A/B fixes landed. |
| `src-ui-code-smell-audit.md` | 2026-05-23 | `src/ui/` audit: snapshot-purity violations, core/app boundary leaks — all bugs and most drift hazards closed. |
| `tutorials-check-and-set-repl-state.md` | 2026-05-23 | Tutorial `REQUIRE` and `SET` step kinds: config-state checkpoint steps and showcase (auto-advance on ack) steps. |
| `inline-numeric-swatch-stepper.md` | 2026-05-23 | Stateless up/down stepper widget for numeric literals in code panel, committing edits through the normal pipeline. |
| `src-app-code-smell-audit.md` | 2026-05-24 | `src/app/` audit: audio-module races, stale camera imports, status-reporting issues — Tier A/B findings catalogued. |
| `src-scene-code-smell-audit.md` | 2026-05-24 | `src/scene/` audit: 54 of 65 findings closed, including camera movement, transform parity, and lighting state fixes. |
| `src-subsystems-code-smell-audit.md` | 2026-05-24 | `src/subsystems/` audit: cross-peer inconsistency in state ownership, lifecycle verbs, and input routing. |
| `memory-profile-panel.md` | 2026-05-26 | Process-memory overlay: cross-platform RSS reader, 1024-sample ring, time-anchored graph, Off/On/Details config row, Ctrl+Shift+W hotkey. |
| `src-app-code-smell-audit-2.md` | 2026-05-26 | `src/app/` follow-up: 22 Tier A fixes verified across 45 binaries and 6815 tests. |
| `src-editor-code-smell-audit-2.md` | 2026-05-26 | `src/editor/` follow-up: residual modal-capture, status-publishing, and undo-ring-generation issues addressed. |
| `src-repl-audit-2-tier-a-implementation.md` | 2026-05-26 | Implementation record for 22 Tier A findings from the `src/repl/` second-pass audit. |
| `src-repl-code-smell-audit-2.md` | 2026-05-26 | `src/repl/` follow-up: 83 findings total, 80 closed — help-layering inversion, export file I/O hardening. |
| `src-subsystems-code-smell-audit-2.md` | 2026-05-26 | `src/subsystems/` follow-up: color-picker state, replay fade batches, tutorial-editor decoupling. |
| `demos-for-color-picker-variable-panel-memprof.md` | 2026-05-29 | Three standalone UI subsystem demos (color picker, variable panel, memory profiler) prove zero coupling to editor/REPL/app via in-place bridge. |
| `repl-trailing-comments-roundtrip.md` | 2026-05-29 | Full round-trip preservation of trailing `// comments` on commands through commits, reformats, and export/import. |
| `ply-feedback-export.md` | 2026-05-31 | `GL_FEEDBACK`-capture PLY mesh export: vertex colors, GLUT solid geometry, authored normals via texcoord channel, optional sRGB decode. |
| `headless-gif-generator.md` | 2026-06-01 | OSMesa headless frame capture (`FREEGLUT_CAPTURE_FRAMES`) + `scripts/record-gif.sh` → animated GIF/MP4 via ffmpeg. |
| `vendor-freeglut.md` | 2026-06-02 | Vendor freeglut as in-tree static library (`third_party/freeglut/`), Cocoa backend, `scripts/vendor-freeglut.sh` re-pin, `THIRD_PARTY_LICENSES.md` acknowledgement. |
| `remove-remaining-repl-mut-reads.md` | 2026-06-08 | Finalized REPL state-access pattern: targeted setters and `_writable()` accessors in `state_owners.h`; zeroed the `_mut()` read ratchet across all non-owner modules. |
| `color-picker-palettes.md` | Color picker — Basic / Full / Harmony palettes |
| `rename-scene-to-render3d.md` | 2026-06-24 | Renamed the 3D scene-renderer module `src/scene/` → `src/render3d/` (`scene_*`/`Scene*`/`SCENE_*` → `render3d_*`/`Render3d*`/`RENDER3D_*`, `render3d_draw_scene`, `Render3dState`, `render3d_demo`, renamed guards/tests, `PROF_RENDER3D_*`) to end the collision with the user-scene concept. Behavior-neutral; user-scene tokens untouched. Includes review. |
| `audio-menu.md` | 2026-07-06 | Add a top-level Audio menu grouping discovered tracks by source, highlighting the playing track, displaying track duration, and integrating Play/Pause, Next/Previous controls, and loop modes. |

Plans not in this directory are active, not-started, not-landed, or
external; see the sibling subdirectories.
