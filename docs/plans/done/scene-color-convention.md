# Scene Color Convention

## Context

The 2D UI side has a disciplined, documented color system (`src/ui/theme.h`:
token enum → single table → `ui_clr()` accessor → `STATIC_ASSERT` →
header-only `tests/test_ui_theme.c`, with a written 3-bucket policy for
what is/ isn't a token). The 3D scene side does **not**. A prior survey
found ~40 bare inline RGBA literals scattered across `src/scene/*` and
`src/app/glr_ctrl.c`'s scene-space draws, with no shared constants or
semantic names.

Partial precedent already exists in the scene module: `SceneRgba`
(`src/scene/render_types.h`) is the established scene color value type,
and `axes.c`/`grid.c` already use disciplined per-theme spec tables with
a `gl_color_rgba(SceneRgba)` setter. The gap is the **non-theme
one-off** colors (lights, orbit gizmo, guides, and every scene-space
color in `glr_ctrl.c`).

Goal: a coherent scene color convention with `theme.h`-level discipline
but **3D-semantic and single-scheme** (not the UI palette, not a 6-row
theme matrix). Because `glr_ctrl.c` draws in the same 3D space as the
scene module, its scene-space colors must pull from the scene
convention rather than carry their own literals.

Decisions (confirmed with user):
- Migrate scene holdouts **and** `glr_ctrl.c` scene-space draws.
- Leave `axes.c`/`grid.c` per-theme spec tables as independent
  theme-switchable data (the scene equivalent of `theme.h`'s bucket-3
  carve-out) - palette covers only non-theme one-offs.
- Single sweep (header + test + all migrations in one change).

## Approach

New header-only palette `src/scene/palette.h`, mirroring `src/ui/theme.h`
structure exactly but scene-scoped:

- Reuse the existing `SceneRgba` type - `#include "render_types.h"`
  (already transitively reached by every scene TU and by `glr_ctrl.c`
  via `scene/render.h`).
- One `static const SceneRgba g_scene_palette[SCENE_CLR_COUNT]` table
  (single scheme; **no** per-theme rows).
- `SceneColorToken` enum, `SCENE_CLR_*` tokens, `SCENE_CLR_COUNT`.
- Accessors mirroring `ui_clr` / `ui_clr_a` / `ui_rgba`:
  - `scene_clr(tok)` → `glColor4f` from the token
  - `scene_clr_a(tok, a)` → token color, alpha multiplied (matches the
    dominant `(..., 0.4f * as)` / `s_xn_alpha` call shape)
  - `scene_rgba(tok)` → returns `SceneRgba` (for component math sites
    and `gl_color_rgba`-style interop)
- `STATIC_ASSERT(SCENE_CLR_COUNT == N, ...)` like theme.h.
- Header docstring documents the bucket-3 carve-outs left as data:
  `axes.c`/`grid.c` theme spec tables, `backdrop.c` procedural sky/
  window tint multipliers, `lights.c` `d[]`-derived halo (computed from
  the light's own diffuse), `transform_guides.c` `xform_axis_color()`
  (computed). These intentionally do NOT become tokens.

### Proposed token set (~20, comparable to theme.h's 19)

| Token | Value (from current literal) | Site |
|---|---|---|
| `SCENE_CLR_AXIS_X` / `_Y` / `_Z` | red / green / blue canonical | geometry_guides ghost-axis pairs (dim variants via `scene_clr_a`) |
| `SCENE_CLR_GUIDE_VERTEX` | `0.2,0.95,0.2` green | geometry_guides.c:276/295 |
| `SCENE_CLR_GUIDE_NORMAL` | `0.95,0.2,0.2` red | geometry_guides.c:283/298 |
| `SCENE_CLR_GUIDE_NEUTRAL` | `0.8,0.8,0.8` | geometry_guides.c:264 |
| `SCENE_CLR_GUIDE_REF` | `0.55,0.55,0.55` | transform_guides.c:375 |
| `SCENE_CLR_GUIDE_REF_BRIGHT` | `0.9–0.95 gray` | transform_guides.c:312/320/382/391 |
| `SCENE_CLR_ORBIT_GLOW_OUTER` / `_MID` / `_INNER` | `1.0,0.70,0.25` / `1.0,0.90,0.55` / `1.0,0.95,0.75` | render.c:413/421/430 |
| `SCENE_CLR_LIGHT_CORE` | `1,1,1` | lights.c:84 |
| `SCENE_CLR_LIGHT_FIXTURE` | `0.4,0.4,0.4` | lights.c:127 |
| `SCENE_CLR_LIGHT_OFF` / `_OFF_DIM` | `0.7,0.2,0.2` / `0.5,0.3,0.3` | lights.c:136/146 |
| `SCENE_CLR_VERTEX_LABEL` | `1.0,1.0,0.30` | glr_ctrl.c:412 |
| `SCENE_CLR_NORMAL_LABEL` | `0.80,0.80,0.30` | glr_ctrl.c:427 |
| `SCENE_CLR_OUTLINE` | `0.55,0.20,0.70` | glr_ctrl.c:573 |
| `SCENE_CLR_OUTLINE_ACTIVE` | `0.0,0.9,0.9` | glr_ctrl.c:571/602/605 |
| `SCENE_CLR_OUTLINE_EDGE` | `0,0,0` | glr_ctrl.c:605/619 |
| `SCENE_CLR_VERTEX_POINT` | `0.85,0.85,0.90` | glr_ctrl.c:671 |
| `SCENE_CLR_VERTEX_POINT_REPLAY` | `1.0,0.88,0.20` | glr_ctrl.c:669 |
| `SCENE_CLR_REPLAY_FADE` | `0.70,0.70,0.80` | glr_ctrl.c:312 |
| `SCENE_CLR_CURSOR_GUIDE` | `0.30,0.95,0.75` | glr_ctrl.c:346 |

(Final values transcribed verbatim from current literals so the sweep is
a visual no-op; exact token list finalized during implementation.)

## Files

**New:**
- `src/scene/palette.h` - the palette (header-only).
- `tests/test_scene_palette.c` - mirrors `tests/test_ui_theme.c`
  (header-only target, links no project objects: every-slot-initialized
  check via `a > 0`, count guard already enforced by `STATIC_ASSERT`).

**Modified (migrate literals → `scene_clr*`):**
- `src/scene/lights.c` - fixed indicator colors only; leave `d[]`-derived.
- `src/scene/render.c` - orbit-target 3-ring glow.
- `src/scene/guides/geometry_guides.c` - axis/vertex/normal guide colors.
- `src/scene/guides/transform_guides.c` - the gray reference ticks only;
  `xform_axis_color()` computed colors stay. **This branch is based on
  `cd1c14f` (ghost-pass), so every color call here is already the
  `tg_color4f()` wrapper that applies the per-pass `g_guide_alpha_mul`.
  The gray ticks migrate via `scene_rgba(SCENE_CLR_GUIDE_REF*)` fed
  through `tg_color4f(c.r, c.g, c.b, c.a * ...)` - NOT `scene_clr()`,
  which would bypass the wrapper and break the ghost/solid two-pass.
  `scene_clr`/`scene_clr_a` remain the right call everywhere else.**
- `src/app/glr_ctrl.c` - all 11 scene-space color sites; add
  `#include "scene/palette.h"` (app→scene, allowed by layering rules).
- `Makefile` - wire `test_scene_palette` exactly like `test_ui_theme`
  (add to the `TEST_BINS` list ~L527 and the header-only `filter-out`
  in `CORE_TEST_BINS` ~L579).

**Reuse (do not reinvent):** `SceneRgba` (`src/scene/render_types.h`),
`STATIC_ASSERT` (`include/c_compat.h`), the `theme.h` accessor shape,
the `test_ui_theme.c` test skeleton + harness (`tests/support/`).

## Constraints / layering

- `palette.h` lives in `src/scene/`, must not depend on REPL/editor;
  app (`glr_ctrl.c`) including it is allowed (app→scene). Keeps the
  `make scene_demo` non-REPL proof intact (scene_demo may adopt tokens
  later but isn't required to).
- `axes.c`/`grid.c` unchanged (per user; bucket-3 theme data).
- `-std=c99` non-pedantic, header-only (syntax-checked transitively by
  `make check-c99` via `$(SRCS)`); `STATIC_ASSERT` not raw
  `_Static_assert`.
- **Ordering resolved:** this work is branched from
  `transform-guides-ghost-pass` (`cd1c14f`), so the ghost-pass
  `tg_color4f` wrapper is assumed landed. No rebase/sequencing needed;
  the `transform_guides.c` migration integrates with the wrapper
  directly (see the file note above - `scene_rgba()` through
  `tg_color4f`, not `scene_clr()`).

## Verification

End-to-end (single sweep is a visual no-op - same RGBA, named):
1. `make sample && make scene_demo && make sample USE_GL_STUBS=1` - all
   build (proves layering: scene palette doesn't drag in REPL).
2. `make test` (incl. new `test_scene_palette`) - green.
3. `make check-c99` and `make check-state-ownership` - pass (header-only
   C99 guard + ownership/boundary guards).
4. `./sample`, load an example with lights + guides + outlines:
   - F-key overlays (vertex labels yellow, normal arrows, outline cyan/
     purple/black, vertex points) visually identical to pre-change.
   - Orbit-drag → 3-ring gizmo glow unchanged.
   - Light indicators (F-key) unchanged for on/off lights.
   - Ctrl+G replay → fade-batch tint + replay vertex points unchanged.
   - Cursor on a `glVertex3f` / transform line → edit guides unchanged.
5. `git diff --stat` - only the 6 files above; no `axes.c`/`grid.c`/
   `backdrop.c` changes.

## Risk / rollback

Low: pure constant extraction, values copied verbatim. Any visible
color shift = a transcription error in `g_scene_palette[]`, fixable in
one table. Header-only + isolated includes; revert is removing
`palette.h`/`test_scene_palette.c` and reverting 5 call-site files.
