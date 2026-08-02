# `src/` restructure: subsystems split, ui core/app split, prof + transform_utils relocations, sample → gl-repl rename

## Completion - 2026-05-23

Done. Landed as 8 commits on `main` (a8b360e → e988e7d):

- 57bdfff - Phase A: `prof.{c,h}` → `src/support/`
- 2371aa8 - Phase B: `transform_utils.h` → `src/scene/guides/`
- 95a971a - Phase C: `src/widgets/` → `src/subsystems/<feature>/`
- 010b35a - Phase D: `src/ui/` split into `core/` and `app/`
- 978ca79 - Phase F: `sample` → `gl-repl` (binary + source files)
- c023ce9 - Phase E: doc refresh (CLAUDE.md / MODULES.md / ARCHITECTURE.md / AGENTS.md / per-subdir READMEs)
- b75be61 - Phase G (added late, user-requested): project-local includes consistency - `<X.h>` → `"X.h"` for `c_compat.h` / `gl_includes.h` / `keys.h` / `gl_2d.h`, plus a new `scripts/check-include-style.sh` wired into `check-state-ownership`.
- e988e7d - fix: stub `tutorial_teardown` in `tools/repl_demo/stubs.c` so `make test-full` links (pre-existing on `main`; the tutorial work introduced a call from REPL pipeline TUs that the demo can't satisfy without dragging in editor/). Baseline bumped 0 → 1 with a note pointing at the proper `ReplTutorialTeardownBridge` follow-up.

Gates all green on macOS:
- `make check-c99 && make check-state-ownership` - clean
- `make test-stubs` - 6510/45 (no regression)
- `make test-full` - 6510/45 + all 4 demos build (gl-repl, bench_repl, scene_demo, editor_demo)

Cross-checked on `gracemont` (Ubuntu 24.04, gcc 13.x) per CLAUDE.md:
- `make check-c99` - OK
- `make check-state-ownership` - OK (incl. new `check-include-style`)
- `make test-stubs` - 6510/45
- `make gl-repl USE_GL_STUBS=1` - links

### Open follow-up

- `ReplTutorialTeardownBridge` - analogous to the existing `ReplExportConfigBridge` / `ReplExportCameraBridge`. The full app installs a real `tutorial_teardown` callback; the demo installs nothing → silent no-op → the one-line stub at `tools/repl_demo/stubs.c` can go away and the ratchet baseline drops back to 0. Not blocking; scoped as a future PR.

## Context

The Layer view in `MODULES.md` already calls out a peer-subsystems node
("replay · variable_panel · color_picker · tutorial · camera") and an
implicit two-tier UI (generic primitives vs. feature-UI). The filesystem
doesn't reflect either: `src/widgets/` is flat, `src/ui/` is flat, `prof.{c,h}`
sits at the repo root with no owner directory, and `transform_utils.h` is a
root-level neutral helper. The main binary is still called `sample` even
though the project name is `gl-repl` - MODULES.md's "Open Refactor Edges"
already tracks the rename as R8 ("sample → glr rename, mechanical, last").
This restructure aligns the filesystem and the binary name with the
already-stated conceptual structure so future readers can see the layout
from `ls src/`, not just from prose. **Every phase sweeps source comments
alongside includes** - a stale `/* see src/widgets/foo.c */` in a
neighbouring file is just as confusing as a stale doc paragraph.

End-state filesystem (the user's spec, verbatim):

```
src/
  subsystems/
    README.md
    color_picker/{color_picker_state.c,h}
    replay/{replay.c,h, replay_state.c,h}
    tutorial/{tutorial.c,h, tutorial_state.c,h}
    variable_panel/{variable_panel_drag.c,h, variable_panel_state.c,h}
  ui/
    README.md
    core/{gl_2d.h, hit.h, layout.c,h, metrics.h, tabbed_overlay.c,h,
          text_layout.c,h, text_panel.c,h, text_search.c,h, theme.h}
    app/{autocomplete_panel.c,h, color_picker.c,h, editor.h, menu_bar.c,h,
         panels.c,h, profile_panel.c,h, repl_code_panel.c,h, replay_hud.c,h,
         scene_tabs.c,h, snapshot.h, state.c,h, state_types.h, variable_panel.c,h}
  scene/guides/transform_utils.h   # moved from repo root
  support/{prof.c, prof.h}         # moved from repo root
```

## What stays the same
- **Symbol prefixes** - `ui_*`, `replay_*`, `tutorial_*`, `color_picker_*`,
  `variable_panel_*`, `prof_*` are unchanged. `check-module-prefixes` is a
  denylist, not a sweep, so existing symbols keep passing.
- **Include resolution** - `-Isrc`, `-I.`, `-Iinclude` stay. All updated
  `#include` lines use `src/`-relative paths (e.g. `"subsystems/tutorial/tutorial.h"`,
  `"ui/core/layout.h"`, `"ui/app/snapshot.h"`, `"support/prof.h"`,
  `"scene/guides/transform_utils.h"`).
- **Build flags, sanitizer config, C99 standard** - untouched.
- **Source-line filenames inside per-feature subdirs** - the user's spec
  keeps the existing filenames (`tutorial_state.c`, not `state.c`) so the
  rename is purely a directory move.

## Blast radius (from the include audit)
- ~75 includes touching `widgets/`, ~152 touching `ui/`, ~7 touching `prof.h`,
  2 touching `transform_utils.h`, ~6 touching `sample.{c,h}` (mostly
  Makefile + the symlink line) - ~240 mechanical `#include`/Makefile-row
  edits across ~90 source files.
- Makefile: ~90 row edits across `SRCS`, `HDRS`, the `UI_SRCS` filter,
  `STATE_NEUTRAL_SRCS`, `SCENE_DEMO_DEP_SRCS`, `REPL_DEMO_DEP_SRCS`, the
  `sample` target, recipe, symlink, and help text.
- ~12 `scripts/check-*.sh` files with hardcoded `src/ui/...` paths
  (text_panel, panels, color_picker, state.{c,h}, plus `src/ui/*.{c,h}` globs).
- ~5 baseline files in `scripts/baselines/` carry path-mentioning comments
  (numbers themselves are path-agnostic; comments need refresh).
- Docs: `CLAUDE.md` File Layout (~25 rows), `MODULES.md` (Layer view labels,
  prose, file-level diagram, sanctioned-exceptions list, R8 entry),
  `ARCHITECTURE.md` (path mentions), `AGENTS.md` table, 6 existing
  `src/*/README.md` (one gets relocated as `src/subsystems/README.md`).
- **Source-comment prose**: every phase does a final
  `grep -rn <old-path-or-name> src/ tests/ tools/ bench/ *.md` to catch
  stale comments the include-only sed sweep misses (banner comments,
  "see also" cross-references, "what's not pulled in" notes in
  tools/repl_demo/, tools/scene_demo/, tools/editor_demo/).

## Phased execution (6 commits, each independently buildable)

Each phase runs `make check-c99 && make check-state-ownership && make test`
to gate before moving on; the cross-check on `gracemont` runs once at the end.

### Phase A - `prof` → `src/support/`
- Move: `prof.c` → `src/support/prof.c`; `prof.h` → `src/support/prof.h`.
- Update 7 includes: `prof.c` (self), `src/app/glr_ctrl.c`, `src/repl/core.c`,
  `src/scene/render.c`, `src/ui/profile_panel.c`, `tests/test_repl_editor.c`,
  `tests/test_ui.c` - all switch to `#include "support/prof.h"`.
- Makefile: 7 row edits (`SRCS`, `HDRS`, `STATE_NEUTRAL_SRCS`,
  `SCENE_DEMO_DEP_SRCS`, `REPL_DEMO_DEP_SRCS`, the bench harness rows).
- `scripts/check-c99.sh:46` carries a literal `prof.c` in `SAMPLE_FILES`;
  update the path.
- **Comment sweep**: `grep -rn '\bprof\.[ch]\b' src/ tests/ tools/ bench/`
  and update any prose references (the only literal `prof.h`/`prof.c`
  mentions outside the include list are likely a handful of comments in
  `src/app/glr_ctrl.c`, `MODULES.md`, and the bench harness).
- Add `src/support/README.md` (one-liner naming the dir as a home for
  neutral shared utilities; describes `prof.{c,h}` as the only inhabitant
  for now).

### Phase B - `transform_utils.h` → `src/scene/guides/`
- Move: `transform_utils.h` → `src/scene/guides/transform_utils.h`.
- Update 2 includes: `src/app/glr_ctrl.c`, `src/scene/guides/transform_guides.c`
  (the second can drop to `#include "transform_utils.h"` because it now sits
  beside the file, but `"scene/guides/transform_utils.h"` is fine too - pick
  the latter for grep consistency).
- Makefile: 1 row in `HDRS`.
- **Comment sweep**: `grep -rn 'transform_utils' src/ tests/ tools/ MODULES.md
  ARCHITECTURE.md CLAUDE.md AGENTS.md` - update any prose path references
  (the header is named in the "Sanctioned naming exceptions" block of
  MODULES.md and is mentioned in CLAUDE.md's File Layout).

### Phase C - `src/widgets/` → `src/subsystems/<feature>/`
Per-feature subdirs and file moves:
- `src/subsystems/color_picker/{color_picker_state.c,h}` (2 files)
- `src/subsystems/replay/{replay.c,h, replay_state.c,h}` (4 files)
- `src/subsystems/tutorial/{tutorial.c,h, tutorial_state.c,h}` (4 files)
- `src/subsystems/variable_panel/{variable_panel_drag.c,h, variable_panel_state.c,h}` (4 files)

Include updates (~75 lines across ~26 source files):
- Every `#include "widgets/<X>.h"` → `#include "subsystems/<feature>/<X>.h"`.
  The 4 feature mappings are the obvious ones from the filename
  (`color_picker_*` → `color_picker/`, `replay*` → `replay/`,
  `tutorial*` → `tutorial/`, `variable_panel*` → `variable_panel/`).
- Intra-subsystem includes (e.g. `tutorial.c` includes `tutorial_state.h`)
  resolve through `-Isrc` exactly the same way - full path
  `"subsystems/tutorial/tutorial_state.h"` everywhere for grep
  consistency. (Adding `-Isrc/subsystems` per-feature would let internal
  files use bare names, but introduces collision risk; defer.)
- `tools/capacity_matrix.c` includes `widgets/replay.h` → update.

Makefile (~24 row edits):
- 8 widget `SRCS` rows → 8 `src/subsystems/<feature>/...` rows.
- Header rows similarly.
- `REPL_DEMO_DEP_SRCS` lists 3 widget files (replay.c, replay_state.c,
  tutorial_state.c) - update those paths.

Scripts:
- `check-module-prefixes.sh` has `src/widgets/replay` and
  `src/widgets/variable_panel_state.h` in **comments only** - refresh for
  readability (not load-bearing).
- `check-unused-apis.sh` and `check-duplicate-api-decls.sh` do
  `find src/repl src/editor src/widgets` for header discovery - change
  `src/widgets` → `src/subsystems`.

**Comment sweep**: after the include rewrite, run
`grep -rn 'src/widgets\|"widgets/' src/ tests/ tools/ bench/` and update
any leftover prose references (file-header banners, "see also" comments,
README cross-links). The bench harness comments name widget files explicitly.

Move `src/widgets/README.md` → `src/subsystems/README.md`; rewrite the
opening to introduce the new subdir layout (one paragraph per feature
subdir, pointing at MODULES.md for the layered view).

### Phase D - `src/ui/` split into `core/` and `app/`
Per the user's spec - `ui/core/` is the REPL/editor-unaware primitives,
`ui/app/` is feature-UI that knows about REPL state, scene snapshots, or
peer subsystems.

`ui/core/`: gl_2d.h, hit.h, layout.{c,h}, metrics.h, tabbed_overlay.{c,h},
text_layout.{c,h}, text_panel.{c,h}, text_search.{c,h}, theme.h.

`ui/app/`: autocomplete_panel.{c,h}, color_picker.{c,h}, editor.h,
menu_bar.{c,h}, panels.{c,h}, profile_panel.{c,h}, repl_code_panel.{c,h},
replay_hud.{c,h}, scene_tabs.{c,h}, snapshot.h, state.{c,h},
state_types.h, variable_panel.{c,h}.

Include updates (~152 lines + ~19 internal cross-split edges):
- External callers (in `src/app/`, `src/scene/`, tests, tools, bench) updating
  `#include "ui/<X>.h"` → `"ui/core/<X>.h"` or `"ui/app/<X>.h"` based on
  where `<X>` landed. Pattern is mechanical: every `<X>` belongs to
  exactly one of the two subdirs.
- Internal cross-split includes (app files reaching into core):
  `panels.c` → gl_2d/layout/metrics from core; `repl_code_panel.{c,h}` →
  gl_2d/layout/metrics/text_layout/text_panel/theme/hit from core;
  `replay_hud.c` and `scene_tabs.c` → gl_2d/layout/metrics/theme from
  core; `color_picker.h` → hit from core. ~19 includes to retarget; all
  one-way (app→core, never reverse).

Makefile (~56 row edits):
- 14 UI `SRCS` rows split between `src/ui/core/...` and `src/ui/app/...`.
- UI `HDRS` rows similarly.
- The implicit filter `UI_SRCS = $(filter src/ui/%.c,$(SRCS))` still
  matches via prefix (because `src/ui/core/*.c` and `src/ui/app/*.c` both
  start with `src/ui/`), **but the `%.c` glob only catches single-segment
  paths**. Change to
  `UI_SRCS = $(filter src/ui/core/%.c src/ui/app/%.c,$(SRCS))`
  (and the same for `UI_HDRS`). This is the single load-bearing Makefile
  edit beyond row updates.

Scripts (~10 files):
- `check-color-picker-ui-isolation.sh` - update `src/ui/color_picker.{c,h}`
  → `src/ui/app/color_picker.{c,h}`.
- `check-editor-ownership-budget.sh` - `src/ui/state.h` → `src/ui/app/state.h`.
- `check-output-actualization.sh` - `src/ui/*.h` glob → `src/ui/app/*.h`
  (state/snapshot/output types live in app).
- `check-ui-no-export-resolver.sh` - `src/ui/*.{c,h}` glob → union of
  `src/ui/core/*.{c,h}` and `src/ui/app/*.{c,h}`.
- `check-ui-panels-no-mutators.sh` - `src/ui/panels.c` → `src/ui/app/panels.c`.
- `check-ui-renderer-signatures.sh` - `src/ui/*.{h,c}` rg patterns → both
  subdirs.
- `check-ui-returns-hits-only.sh` - `src/ui/*.c` glob + the
  `src/ui/state.c` exclusion → `src/ui/app/...`.
- `check-ui-text-panel-pure.sh` - `src/ui/text_panel.{c,h}` →
  `src/ui/core/text_panel.{c,h}`.
- `check-variable-panel-forwarders.sh` - `src/ui/state.c` → `src/ui/app/state.c`.
- Baseline files mentioning `src/ui/...` in comments (5 files) - refresh
  the comments only; numeric baselines are path-agnostic.

**Comment sweep**: `grep -rn 'src/ui/\|"ui/' src/ tests/ tools/ bench/`
after the include rewrite. Update prose references that name old paths;
tools/repl_demo and tools/editor_demo comments mention `src/ui/...` paths
in their "what's not pulled in" notes. The check-state-ownership script
descriptions also mention `src/ui/*.c` - refresh inline.

Rewrite `src/ui/README.md` to introduce the two-tier layout (core =
REPL-agnostic primitives; app = feature-UI knowing REPL/editor/peer
concepts), with a one-line directory landmark for each.

### Phase E - Documentation refresh (`.md` files + cross-doc consistency)
Four files, mechanical path updates plus narrative tweaks:
- `CLAUDE.md` File Layout (~25 rows): every `src/widgets/...`,
  `src/ui/...`, root `prof.{c,h}`, root `transform_utils.h` row updates
  to the new path. The convention paragraph on prefixes / directory
  ownership stays - it already names the prefixes, just point at the new
  homes. Also update narrative paragraphs mentioning `src/widgets/` or
  `src/ui/` (e.g. the "Editor input dispatcher" row, the "Cursor Edit
  Guides" section, the "Replay System" / "Tutorials" subsections).
- `MODULES.md`:
  - Layer view diagram (`### Layer view`): rename "Peer subsystems" node
    layer label to match `src/subsystems/`; split the "5. 2D UI" node
    text to acknowledge the `core` + `app` two-tier (drop the conflated
    "(snapshots in, UiHit out - never mutates)" annotation only as far
    as needed - keep its substance; restructure prose).
  - File-level view (`### File-level view`): update `widgets/` →
    `subsystems/` boxes and the `tutorial_sys`/`replay`/etc. labels.
  - Per-module rows (`tutorial_state`, `replay`, `variable_panel`,
    `color_picker`, plus the `repl_help_text` peer): update path
    references inline.
  - Sanctioned-naming-exceptions section: refresh path references
    (`src/widgets/variable_panel_state.h` →
    `src/subsystems/variable_panel/variable_panel_state.h`, etc.).
  - Open Refactor Edges: drop R8 ("sample → glr rename, mechanical,
    last") since Phase F lands it. Add a one-line "completed: src/
    restructure + sample → gl-repl rename" note.
- `ARCHITECTURE.md`: ~40 path mentions across discussions of bridges,
  layering, demo binaries. All mechanical replacements.
- `AGENTS.md`: ~60 table-row path updates (mirrors CLAUDE.md File
  Layout style). Mechanical.

**Cross-doc grep**: after the file-by-file passes, run
`grep -nE 'src/(widgets|ui)/|"widgets/|"ui/|\bprof\.[ch]\b|transform_utils\.h' CLAUDE.md MODULES.md ARCHITECTURE.md AGENTS.md`
and confirm zero stale matches. (Anything left is a paragraph the
mechanical sweep missed.)

### Phase F - `sample` → `gl-repl` rename
The binary, target, source file, and symlink all become `gl-repl`. C
filenames use underscores by project convention, so the source pair is
`gl_repl.{c,h}`; the binary/target name uses a hyphen (`gl-repl`)
matching the project name and Unix-binary norm.

Moves:
- `sample.c` → `gl_repl.c`
- `sample.h` → `gl_repl.h`

Makefile:
- The `sample` target name → `gl-repl`. Its dependencies, recipe,
  symlink step, and all references throughout the Makefile
  (`build/release/sample` → `build/release/gl-repl`,
  `sample.o` → `gl_repl.o`, the `make sample`, `make sample USE_GL_STUBS=1`
  usage paths) all update mechanically.
- The `SRCS` row `sample.c` → `gl_repl.c` (the file move forces this).
- The `HDRS` row `sample.h` → `gl_repl.h`.
- Help / `.PHONY` lists that name `sample` → `gl-repl`.
- `STATE_NEUTRAL_SRCS` / `SCENE_DEMO_DEP_SRCS` / `REPL_DEMO_DEP_SRCS` do
  not include `sample.c`, so they are unaffected.
- `scripts/check-c99.sh` `SAMPLE_FILES` literal (`sample.c`) →
  `gl_repl.c`. The variable name `SAMPLE_FILES` is internal-only; leave
  the variable name as-is unless it crosses a line; just update the
  filename literal.

Comment sweep - broader than the other phases because `sample` is a
common English word and shows up in many sentences:
- `grep -rnw 'sample\b' src/ tests/ tools/ bench/ *.md` - manual review
  required to distinguish "the `sample` binary" (rename to `gl-repl`)
  from "a sample value" / "code sample" prose (leave alone). The
  Makefile help text, several C-source file-banner comments, and the
  init-trace messages (`[init +N.NNNs] <phase>` strings in `sample.c`)
  are clear renames.
- `sample.h` and `sample.c` literal mentions → `gl_repl.h` / `gl_repl.c`.
- `./sample` invocation hints in CLAUDE.md `## Run` section → `./gl-repl`.
- `make sample` invocation hints in CLAUDE.md `## Build` → `make gl-repl`.
- The symlink target - Makefile's `ln -sfn build/release/sample sample`
  → `ln -sfn build/release/gl-repl gl-repl`.

Documentation:
- `CLAUDE.md`: `## Build` section (`make sample`, `make sample
  USE_GL_STUBS=1` rows), `## Run` section (every `./sample ...`
  invocation), File Layout row for the renamed source files, any
  prose paragraph naming the binary.
- `MODULES.md`: "Open Refactor Edges" - strike R8.
- `ARCHITECTURE.md`: any binary-name references.
- `AGENTS.md`: the binary row.
- All six `src/*/README.md` files - quick grep for `sample` and update.

Other touches:
- `tools/repl_demo/`, `tools/scene_demo/`, `tools/editor_demo/` - these
  standalone-demo binaries are NOT renamed (they're proofs that the
  REPL / scene / editor pipelines link without the full app). Only the
  `sample` binary becomes `gl-repl`.
- `output.c` (the file the running app saves to) - name unchanged
  (it's a user-data artifact, not a binary name).
- `quit-recovery.c`, `my-scene.c` at the repo root - unchanged.

History preservation: each move uses `git mv` so blame chains stay
intact across the rename.

## Mechanics - how to run each phase locally
For each phase:
1. `git mv` the files (preserves history) into their new homes.
2. Update includes via a scripted sed sweep (one pattern per phase). For
   the ui split, the sed needs to know which target each filename maps to
   - a small Python helper that reads the user's per-file split and emits
   the right substitution is the cleanest approach (vs. 25 manual seds).
3. Update the Makefile rows.
4. Update the affected `scripts/check-*.sh`.
5. `make check-c99 && make check-state-ownership && make test` - gate
   before committing.
6. Commit with a `refactor(layout): …` message.

The Python helper from step 2 lives in the working tree temporarily and
is removed before commit (or kept in `scripts/` if there's appetite for
later layout shuffles).

## Verification
Run after EACH phase:
- `make check-c99` - must stay OK (catches stray bare-name includes that
  used to resolve from `-I.` and no longer do).
- `make check-state-ownership` - exercises every boundary guard. The
  `check-ui-*` family is the highest-risk surface after Phase D; if any
  script glob is wrong it fires here.
- `make test` - must stay at the pre-restructure count (5713/39 binaries
  as of the last green run); no test logic changes, so the count is the
  exact regression signal.
- `make sample` (or `make gl-repl` after Phase F) - links the full app;
  smoke-runs the interactive binary.
- **Stale-reference grep** - at the end of each phase, the phase-specific
  `grep -rn <old-path>` returns zero matches across `src/ tests/ tools/
  bench/ *.md`.
- After Phase F (final phase only): `ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && git pull --ff-only origin main && make check-c99 && make test-stubs'`
  per CLAUDE.md's cross-check note. Also confirm the gracemont sample
  build now targets `gl-repl`, not `sample`.

## Notes / non-goals
- **Symbol renames are out of scope.** `widgets_*`/`subsystems_*` is not
  a thing; existing `replay_*` / `tutorial_*` / etc. stay. `check-module-prefixes`
  is a removed-name denylist, so it cares only about old stale names,
  not the directory.
- **No per-feature READMEs** inside `src/subsystems/<feature>/` or inside
  `src/ui/{core,app}/` - the user's spec lists READMEs only at the
  `subsystems/` and `ui/` top level. `src/support/README.md` is added
  as the convention-matching landing page; `src/scene/guides/` already
  has its parent README.
- **No `-Isrc/subsystems` or `-Isrc/ui` per-tier `-I` flags** - full paths
  in includes for unambiguous greppability.
- **Camera stays in `src/app/`** - the user's spec doesn't move it, and
  the Layer-view → filesystem alignment for camera is a separate open
  question (tracked in MODULES.md "Open Refactor Edges"). Out of scope here.
