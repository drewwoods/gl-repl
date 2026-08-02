# Rename `src/scene` → `src/render3d`

**Status:** ready to execute. Pure rename, **no behavior change** - the final
diff must not alter any runtime output, file format, or UI string. The compiler
+ test suite are the consistency net: if a rename is incomplete or inconsistent,
the build breaks.

**Executor note:** this is written to be run step by step with minimal
judgment. Run each phase in order, run its verification block, and **STOP** if a
check fails instead of improvising. Most steps are exact commands.

---

## 1. What and why (read once, then skip to §4)

`src/scene` is the **3D scene renderer**: it owns the *frame* (viewport, clear,
projection, lights, grid/axes/backdrop, accumulation AA / motion blur,
post-process) and renders whatever geometry is handed to it through a callback.
It does **not** own the geometry, the camera, or any REPL/editor/UI state.

The name `scene` is wrong twice: it names the *contents* the module doesn't own,
and it **collides with the unrelated "user-scene" concept** (saved program slots:
`g_user_scenes`, scene tabs, `SceneSnapshot`, the `@scene-name` save directive).

Decision (settled): rename the module to **`render3d`**. Rationale: the real
boundary is *3D rendering* (this module) vs. *2D `ui_*` rendering* (UI / HUD /
code panel), so the `3d` qualifier scopes it correctly without the bare word
"renderer" over-claiming all rendering. The user-scene concept keeps the word
"scene" (it is the legitimate owner).

### The rename rule

| Kind | Old | New |
|---|---|---|
| Directory | `src/scene/` (+ `guides/`) | `src/render3d/` (+ `guides/`) |
| Include path | `"scene/…"` | `"render3d/…"` |
| Functions / fields (snake) | `scene_*` | `render3d_*` |
| Types (TypeCase) | `Scene*` | `Render3d*` |
| Macros / enum consts / guards | `SCENE_*` | `RENDER3D_*` |
| Demo binary + dir | `scene_demo`, `tools/scene_demo/` | `render3d_demo`, `tools/render3d_demo/` |
| Guard targets | `check-pure-scene-no-repl-state`, `check-scene-no-repl-state-mut`, `check-scene-no-upper-layers` | `check-pure-render3d-no-repl-state`, `check-render3d-no-repl-state-mut`, `check-render3d-no-upper-layers` |
| Tests | `test_scene_render`, `test_scene_guides`, `test_scene_transition`, `test_scene_palette` | `test_render3d_*` |

`render3d_` / `Render3d` / `RENDER3D_` are confirmed unused today, so there are
no collisions (the old `render_*` statics in `glr_ctrl.c`/demos are a *different*
prefix and are left alone).

### Four names that are NOT mechanical (apply by hand - see §5, Step 2b)

A blind prefix swap stutters on these. Use the target on the right:

| Old | New |
|---|---|
| `scene_render_3d_scene` | `render3d_draw_scene` |
| `scene_render_init_gl` | `render3d_init_gl` |
| `scene_renderer_state_init` | `render3d_state_init` |
| `SceneRendererState` | `Render3dState` |

---

## 2. CRITICAL INVARIANT - "scene" is two concepts; only ONE moves

The single way this rename goes wrong is by sweeping **user-scene** tokens.
**Never run a blanket `s/scene/render3d/` over the tree.**

- **MOVES** = every `scene_*` / `Scene*` / `SCENE_*` token **that lives inside
  `src/scene/`** (the renderer module). This includes `SceneRenderConfig`,
  `SceneGuideSnapshot`, `SceneRgba`, `SceneFrameRenderContext`, `SceneLight`,
  all the theme/enum types, the `scene_grid_render` / `scene_axes_render` /
  `scene_lights_*` / `scene_backdrop_*` / `scene_transform_guides_*` functions,
  the `SCENE_CLR_*` / `SCENE_BACKDROP_*` / `SCENE_XN_*` macros, the header
  guards, etc.
- **STAYS** = the user-scene concept, which lives in `src/repl/scenes.c`,
  `src/repl/scene_snapshot.c`, `src/ui/app/scene_tabs.c`, etc.:
  `SceneSnapshot`, `SceneSnapshotCameraMode`, `scene_snapshot_*`, `scene_tabs*`,
  `scene_name`, `scene_name_hint`, `scene_slot`, `scene_idx`, `scene_slug_used`,
  `scene_filename_slug_for_slot`, `scene_cfg_clear`, `scene_cfg_reset_all`,
  `UserScene`, `g_user_scenes`, `restore_user_scene`, `repl_*_scene`,
  `glr_scene_load_example`; the save-file directive `@scene-name`; the tests
  `test_ui_scene_tabs`, `test_scene_file_menu`; the guard
  `check-repl-scenes-cfg-clear-paired`; theme constants `GRID_THEME_*` /
  `AXES_THEME_*` / `LIGHT_THEME_*` (no scene prefix).

**The safety mechanism:** the symbol rename in §5 *derives the MOVE set
automatically from the contents of the moved directory* (`src/render3d/` after
`git mv`). User-scene tokens are not in that directory, so they are excluded by
construction - you do not hand-maintain a denylist. Then Step 0 / Step 3 verify
the STAY tokens' counts are unchanged.

---

## 3. Files this touches (orientation)

- Source: ~100 `.c`/`.h` files across `src`, `tools`, `tests`.
- Build: `Makefile` (path vars, demo/test/guard targets).
- Guards/scripts: `scripts/check-scene-no-upper-layers.sh` (renamed),
  `scripts/check-module-prefixes.sh`, and a few view/layer guards that hardcode
  `src/scene` paths.
- Docs (current-design only): `CLAUDE.md`, `MODULES.md`, `ARCHITECTURE.md`,
  `ADVANCED_USAGE.md`, `src/scene/README.md` (→ `src/render3d/README.md`),
  `src/app/README.md`, `src/support/README.md`, `src/subsystems/README.md`.
- **Do NOT edit** `plans/done/**` or `plans/in-review/accum-motion-blur.md` -
  those are history.

---

## 4. Phase 0 - Branch + baseline + record STAY counts

```bash
cd <repo>
git switch -c rename-scene-to-render3d

# Baseline must be green BEFORE starting. STOP if any of these fail.
make test
make check-state-ownership
make check-c99
make scene_demo && ./scene_demo --help >/dev/null 2>&1 || true   # links?

# Record user-scene (STAY) token counts to compare against at the end.
git grep -hoE '\b(SceneSnapshot|SceneSnapshotCameraMode|scene_snapshot[a-z_]*|scene_tabs[a-z_]*|scene_name|scene_name_hint|scene_slot|scene_idx|scene_slug_used|scene_filename_slug_for_slot|scene_cfg_clear|scene_cfg_reset_all|UserScene|g_user_scenes|restore_user_scene)\b' \
  -- 'src/**' 'tests/**' 'tools/**' | sort | uniq -c > /tmp/stay-before.txt
cat /tmp/stay-before.txt
```

**Gate:** baseline builds/tests green; `/tmp/stay-before.txt` is non-empty.

---

## 5. Phase 1 - Move files + fix include paths (NO symbol renames yet)

This phase must compile with **zero symbol renames** - it only moves files and
repoints `"scene/…"` includes.

```bash
# 1a. Move the module (guides/ comes along).
git mv src/scene src/render3d

# 1b. Repoint path-qualified includes "scene/..." -> "render3d/..." tree-wide.
#     (Bare sibling includes like "render.h" / "guides/geometry_guides.h" are
#      resolved relative to the including file and need no change.)
grep -rlE '#include "scene/' src tools tests \
  | xargs gsed -i 's#"scene/#"render3d/#g'

# 1c. Update Makefile PATHS only (not target names yet): every src/scene/ path.
gsed -i 's#src/scene/#src/render3d/#g' Makefile

# 1d. Update the layer guard's hardcoded find path (renamed properly in Phase 3).
gsed -i 's#src/scene#src/render3d#g' scripts/check-scene-no-upper-layers.sh
```

**Verify (build must still link; no symbol changed):**
```bash
make gl-repl
make scene_demo          # still the old target name in this phase
grep -rn '"scene/' src tools tests   # EXPECT: zero hits
```
**Gate:** `make gl-repl` + `make scene_demo` succeed; no `"scene/` includes
remain. If a build error mentions a missing header, a bare include needs the
`render3d/` prefix or a path var in the Makefile was missed - fix and re-run.
**Commit** ("Phase 1: move src/scene -> src/render3d, repoint includes").

---

## 6. Phase 2 - Rename the renderer's symbols

### Step 2a - Generate the rename map from the moved directory

This derives the MOVE set from `src/render3d/`'s own tokens, so user-scene
tokens cannot be included.

```bash
{
  # Every scene_/Scene/SCENE_ token that appears inside the renderer module.
  git grep -hoE '\b(scene_[a-z0-9_]+|Scene[A-Za-z0-9]+|SCENE_[A-Z0-9_]+)\b' \
    -- 'src/render3d/**' | sort -u | while read -r tok; do
    case "$tok" in
      # Hand-mapped below (de-stutter); skip the mechanical rule for these.
      scene_render_3d_scene|scene_render_init_gl|scene_renderer_state_init|SceneRendererState) continue;;
    esac
    new=$(printf '%s' "$tok" \
      | sed -E 's/^scene_/render3d_/; s/^Scene/Render3d/; s/^SCENE_/RENDER3D_/')
    printf 's/\\b%s\\b/%s/g\n' "$tok" "$new"
  done
  # 2b. The four non-mechanical names:
  printf 's/\\bscene_render_3d_scene\\b/render3d_draw_scene/g\n'
  printf 's/\\bscene_render_init_gl\\b/render3d_init_gl/g\n'
  printf 's/\\bscene_renderer_state_init\\b/render3d_state_init/g\n'
  printf 's/\\bSceneRendererState\\b/Render3dState/g\n'
} > /tmp/render3d.sed

wc -l /tmp/render3d.sed     # sanity: expect ~140-160 substitution lines
head /tmp/render3d.sed
```

### Step 2c - Apply the map to all code (NOT docs/Makefile/scripts)

```bash
git grep -lE '\b(scene_[a-z]|Scene[A-Z]|SCENE_)' -- 'src/**' 'tools/**' 'tests/**' \
  | grep -E '\.(c|h)$' \
  | xargs gsed -i -f /tmp/render3d.sed
```

### Step 2d - Verify

```bash
# (i) No renderer-prefixed token remains inside the module:
grep -rnE '\b(scene_[a-z]|Scene[A-Z]|SCENE_)' src/render3d        # EXPECT: zero
# (ii) The new prefix is present in the module:
grep -rcE '\b(render3d_|Render3d|RENDER3D_)' src/render3d | grep -v ':0' | head
# (iii) STAY tokens UNCHANGED vs Phase 0:
git grep -hoE '\b(SceneSnapshot|SceneSnapshotCameraMode|scene_snapshot[a-z_]*|scene_tabs[a-z_]*|scene_name|scene_name_hint|scene_slot|scene_idx|scene_slug_used|scene_filename_slug_for_slot|scene_cfg_clear|scene_cfg_reset_all|UserScene|g_user_scenes|restore_user_scene)\b' \
  -- 'src/**' 'tests/**' 'tools/**' | sort | uniq -c > /tmp/stay-after.txt
diff /tmp/stay-before.txt /tmp/stay-after.txt                     # EXPECT: no diff
# (iv) The compiler proves consistency:
make gl-repl
make test
```
**Gate:** (i) zero, (iii) **no diff** (STAY untouched), (iv) green. If (iii)
shows a diff, a user-scene token was wrongly swept - `git checkout` the affected
file and investigate before continuing. If `make test` fails to *compile*, a
rename was inconsistent across a header/caller - fix the reported symbol.
**Commit** ("Phase 2: rename scene_/Scene/SCENE_ -> render3d_/Render3d/RENDER3D_").

> Note: generic field names `scene_x/y/w/h` and `scene_clr*` become
> `render3d_x/y/w/h` / `render3d_clr*`. They are `SceneRenderConfig` fields; the
> build verifies they renamed consistently. If the compiler is happy, they are
> correct.

---

## 7. Phase 3 - Build system, guards, scripts, demo + test target names

### 7a. Demo: rename dir, source file, and Makefile targets/vars

```bash
git mv tools/scene_demo tools/render3d_demo
git mv tools/render3d_demo/scene_demo.c tools/render3d_demo/render3d_demo.c
# Makefile: target name, var names, root symlink. (Paths were fixed in Phase 1.)
gsed -i 's/\bscene_demo\b/render3d_demo/g; s/\bSCENE_DEMO/RENDER3D_DEMO/g' Makefile
gsed -i 's/tools\/scene_demo/tools\/render3d_demo/g' Makefile
# Path vars for clarity:
gsed -i 's/\bSCENE_SRCS\b/RENDER3D_SRCS/g; s/\bSCENE_HDRS\b/RENDER3D_HDRS/g; s/\bSCENE_OBJS\b/RENDER3D_OBJS/g' Makefile
```
The demo's own file-private statics (`render_hud`, `render_cube`, …) are bare
`render_`, not `scene_` - leave them as-is.

### 7b. Tests: rename the four renderer tests (NOT the user-scene ones)

```bash
for n in render guides transition palette; do
  git mv tests/test_scene_$n.c tests/test_render3d_$n.c 2>/dev/null || true
done
# Makefile: rename ONLY these four test targets/bins/obj blocks.
gsed -i -E 's/\btest_scene_(render|guides|transition|palette)\b/test_render3d_\1/g' Makefile
```
**Do NOT touch** `test_ui_scene_tabs` or `test_scene_file_menu` (user-scene).

### 7c. Guard targets + scripts

```bash
# Rename the renderer guards (target names + recipe lines + aggregate lists).
gsed -i 's/check-pure-scene-no-repl-state/check-pure-render3d-no-repl-state/g; \
         s/check-scene-no-repl-state-mut/check-render3d-no-repl-state-mut/g; \
         s/check-scene-no-upper-layers/check-render3d-no-upper-layers/g' Makefile
git mv scripts/check-scene-no-upper-layers.sh scripts/check-render3d-no-upper-layers.sh

# Other scripts that hardcode the old path:
for f in scripts/check-views-by-value-snapshot.sh \
         scripts/check-no-facade-include-in-views.sh \
         scripts/check-views-flat.sh \
         scripts/check-ui-core-no-upper-layers.sh \
         scripts/allowlists/facade-includes-in-views.txt \
         scripts/baselines/views-flat-violations.txt; do
  [ -f "$f" ] && gsed -i 's#src/scene#src/render3d#g' "$f"
done
```
**Do NOT rename** `check-repl-scenes-cfg-clear-paired` (user-scene guard).

### 7d. `scripts/check-module-prefixes.sh` - update mapping + invert the deny

This guard documents the module→prefix map and denies stale prefixes. Update the
comment `src/scene -> scene_/Scene` to `src/render3d -> render3d_/Render3d`, and
make it deny the **old** prefix reappearing in `src/render3d` (catches a future
partial revert). Edit by hand - confirm the deny pattern now flags
`scene_`/`Scene`/`SCENE_` under `src/render3d`.

**Verify Phase 3:**
```bash
make render3d_demo && ./render3d_demo --help >/dev/null 2>&1 || true
make check-state-ownership      # exercises renamed guards + check-c99 + module-prefixes
make test                       # renamed test targets run
```
**Gate:** all green; `make scene_demo` no longer exists; `make render3d_demo`
works. **Commit** ("Phase 3: build/guard/script/demo/test target renames").

---

## 8. Phase 4 - Documentation (current-design docs only)

Update prose + the symbol references. **Do not** edit `plans/done/**` or
`plans/in-review/accum-motion-blur.md`. (`src/scene/README.md` already moved to
`src/render3d/README.md` with the directory in Phase 1 - just edit its contents.)

Edit by hand (these mix prose "scene" with symbol references - be selective):

- **`src/render3d/README.md`** - retitle to the `render3d` module; keep the
  "what a scene renderer is" framing but anchor it on the module name
  `render3d`; update every `src/scene` path, `scene_render_3d_scene` →
  `render3d_draw_scene`, `SceneRenderConfig` → `Render3dRenderConfig`,
  `scene_demo` → `render3d_demo`.
- **`CLAUDE.md`** - the File-Layout table rows for every `src/scene/*` file
  (paths + descriptions), the Conventions bullet (`scene_*` for 3D rendering →
  `render3d_*`), the Rendering-Pipeline and Accumulation-Motion-Blur prose
  (`scene_render_3d_scene`, `SceneRenderConfig`, `src/scene/render.c`), and any
  `scene_demo` / `make scene_demo` mention.
- **`MODULES.md`** - the layer-4 "scene renderer" map and the `scene_*` prefix
  line.
- **`ARCHITECTURE.md`** - frame-pipeline narrative references
  (`scene_render_3d_scene`, `SceneRenderConfig`, `src/scene`).
- **`ADVANCED_USAGE.md`**, **`src/app/README.md`**, **`src/support/README.md`**,
  **`src/subsystems/README.md`** - their `src/scene` path references.

**Verify:**
```bash
# No current-design doc still references the old module path/symbol
# (plans/done and accum-motion-blur are allowed to retain history):
grep -rnE 'src/scene\b|scene_render_3d_scene|SceneRenderConfig' \
  --include='*.md' . | grep -vE 'plans/done|accum-motion-blur'   # EXPECT: zero
```
**Gate:** zero non-historical hits. **Commit** ("Phase 4: docs").

---

## 9. Phase 5 - Full verification matrix

```bash
make test                       # debug = ASan + UBSan
make gl-repl
make render3d_demo
make repl_demo && make editor_demo   # unrelated demos still link
make check-state-ownership
make check-c99
make test-stubs
make gl-repl USE_GL_STUBS=1

# Final stale-token sweeps:
grep -rnE '\b(scene_[a-z]|Scene[A-Z]|SCENE_)' src/render3d        # EXPECT: zero
grep -rn '"scene/' src tools tests                               # EXPECT: zero
diff /tmp/stay-before.txt <(git grep -hoE '\b(SceneSnapshot|SceneSnapshotCameraMode|scene_snapshot[a-z_]*|scene_tabs[a-z_]*|scene_name|scene_name_hint|scene_slot|scene_idx|scene_slug_used|scene_filename_slug_for_slot|scene_cfg_clear|scene_cfg_reset_all|UserScene|g_user_scenes|restore_user_scene)\b' -- 'src/**' 'tests/**' 'tools/**' | sort | uniq -c)   # EXPECT: no diff
```

**gracemont real-GCC / portability cross-check (per CLAUDE.md):**
```bash
git push origin rename-scene-to-render3d
ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
  git fetch origin rename-scene-to-render3d && git checkout FETCH_HEAD && \
  make check-c99 && make test-stubs'
```

**Gate:** everything green; all three stale sweeps clean; STAY diff empty.

---

## 10. Definition of done

- [ ] `src/render3d/` exists; `src/scene/` gone; `git log --follow` shows history.
- [ ] No `scene_*`/`Scene*`/`SCENE_*` token left in `src/render3d/`.
- [ ] STAY token counts identical to Phase 0 (user-scene concept untouched).
- [ ] `make test`, `make check-state-ownership`, `make check-c99`,
      `make test-stubs`, `make gl-repl`, `make render3d_demo` all green.
- [ ] gracemont `check-c99` + `test-stubs` green.
- [ ] No current-design doc references `src/scene` / `scene_render_3d_scene` /
      `SceneRenderConfig`; `plans/done/**` left as history.
- [ ] Diff is behavior-neutral: no save-file-format change, no UI string change,
      no test-output change.

---

## Appendix A - Decision history (why `render3d`, not the alternatives)

Diagnostic: *does the name describe what the module owns (the framing
apparatus), or what it excludes (the geometry/contents)?* And: does it
over-claim a cross-cutting verb?

- **`scene` (status quo)** - names the excluded contents; collides with the
  user-scene concept. Rejected (the reason for this whole change).
- **`renderer`** - accurate about the pipeline, but the bare agent-noun
  over-claims *all* rendering when UI/HUD/editor rendering lives elsewhere.
  Rejected for over-claiming; `render3d` keeps the accuracy while scoping to 3D.
- **`stage` / `studio`** - collision-free and fits the grid/backdrop
  "decorations," but metaphorical where a code module wants literal, and
  undersells the projection/AA/post pipeline. Runner-up.
- **`world`** - worse than status quo; "world" is the contents/world-space, the
  exact thing the module doesn't own. Rejected.
- **`viewport3d`** - "viewport" is already owned by `UiState`
  (`UiViewportState`, `ui_state_viewport*`) and connotes the pixel rectangle /
  layout region, not the renderer. Rejected for that overlap.
- **Renaming the user-scene side instead** - fewer files but touches the on-disk
  `@scene-name` save format + UI labels (not behavior-neutral; needs
  back-compat), renames the *more* legitimate owner of "scene," and leaves the
  renderer mis-named. Rejected.
- **Also cleaning the crowded `render_*` namespace** - out of scope; those are
  file-private statics, which the codebase convention permits to be neutrally
  named. `render3d_` does not collide with the bare `render_` statics, so no
  cleanup is needed.

## Appendix B - STAY list (must remain unchanged)

`SceneSnapshot`, `SceneSnapshotCameraMode`, all `scene_snapshot_*`, all
`scene_tabs*`, `scene_name`, `scene_name_hint`, `scene_slot`, `scene_idx`,
`scene_slug_used`, `scene_filename_slug_for_slot`, `scene_cfg_clear`,
`scene_cfg_reset_all`, `UserScene`, `g_user_scenes`, `restore_user_scene`,
`repl_*_scene`, `glr_scene_load_example`; the `@scene-name` save directive;
tests `test_ui_scene_tabs`, `test_scene_file_menu`; guard
`check-repl-scenes-cfg-clear-paired`; theme constants `GRID_THEME_*`,
`AXES_THEME_*`, `LIGHT_THEME_*`.

---

## Review (2026-06-24) - landed

Reviewed against the §10 Definition of Done. **Verdict: correct and complete on
the code side; merged after a small batch of cosmetic-label cleanups.**

**Verified green:**

- **Directory move with history.** `src/scene/` gone, `src/render3d/` present;
  `git log --follow src/render3d/render.c` shows the full pre-rename history
  (154 commits) - the `git mv` preserved provenance.
- **Symbol rename complete.** Zero `scene_*`/`Scene*`/`SCENE_*` renderer tokens
  remain in `src/render3d`; zero `"scene/"` includes anywhere in `src`/`tools`/
  `tests`; zero renderer symbols (`scene_render_3d_scene`, `SceneRenderConfig`,
  `SceneRendererState`, …) survive in any `.c`/`.h`. The four de-stutter
  renames are applied.
- **User-scene concept untouched.** A real token-by-token diff of every
  `scene`-containing identifier between `main` and `HEAD` showed every *removed*
  token is a renderer symbol; `g_user_scenes`, `UserScene`, `scene_tabs*`,
  `@scene-name`, `repl_*_scene`, the `test_ui_scene_tabs` / `test_scene_file_menu`
  tests, and `check-repl-scenes-cfg-clear-paired` all remain.
- **Builds + tests.** `make gl-repl`, `make render3d_demo`, `make test` (8596
  pass), `make test-stubs` (10390 pass), `make check-state-ownership`,
  `make check-c99` all green. Renamed guards
  (`check-pure-render3d-no-repl-state`, `check-render3d-no-repl-state-mut`,
  `check-render3d-no-upper-layers`) run; `check-module-prefixes.sh` was correctly
  inverted to deny the old prefix reappearing in `src/render3d`.
- **Bonus commits beyond the plan are sound:** `PROF_RENDER3D_3D → PROF_RENDER3D`
  de-stutter (the mechanical `SCENE_→RENDER3D_` swap on `PROF_SCENE_3D` would
  have stuttered) and a Makefile lines fix.

**Flaw found in the plan's own verification (not the branch):** the §4/§6/§9
STAY-token gate uses `git grep … -- 'src/**' 'tests/**'`, and that `src/**`
pathspec matches **nothing** in this repo's git - so `stay-before.txt` /
`stay-after.txt` were both empty and the "no diff" gate passed *vacuously*. The
branch is fine (confirmed by the real comparison above), but anyone re-running
this plan's gate should replace `'src/**'` with `src` (a plain dir pathspec) or
`:(glob)src/**`.

**Cosmetic stragglers cleaned up before merge** (all behavior-neutral -
comment/echo text only, the guards were already functionally correct):

- Renamed-guard output labels: `scripts/check-render3d-no-upper-layers.sh`
  printed `scene-no-upper-layers OK` → `render3d-no-upper-layers OK`; the
  `check-pure-render3d-no-repl-state` recipe printed `Pure-scene boundary OK` →
  `Pure-render3d boundary OK`.
- Dead-name references in script comments: `scene_render_3d_scene()` →
  `render3d_draw_scene()` in `check-ui-no-export-resolver.sh`; the old
  `scene_demo` target name → `render3d_demo` in
  `check-render3d-no-upper-layers.sh`, `check-subsystem-demo-isolation.sh`, and
  `check-c99.sh`.
- A stale dangling `scene_demo` symlink in the working tree (gitignored) was
  removed.
- `plans/partial/module-architecture-doc-split-layering-audit.md` got a
  top-of-file forward-pointer note rather than a token sweep: it contains
  explicit "Evidence (original)" sections whose `src/scene` / `Scene*` text is
  deliberately historical, so rewriting them would have falsified the record.

**Not done in this branch (recommended before/after merge):** the gracemont
real-GCC `check-c99` + `test-stubs` cross-check from §9. This is a build-system /
Makefile change, and CLAUDE.md asks for a real-GCC verification on
portability-sensitive edits; local `check-c99` only exercised Apple clang in
`-std=c99` mode.
