**TL;DR:** The grid was designed at bg=(0.1,0.1,0.1) where CLASSIC minor lines render at ~0.132 luminance. At pure black, the same lines render at only ~0.04 - below display black-crush thresholds. The fix is to compute bg luminance from `g_clear_color[]` and scale up grid line alphas when the background is darker than the design point.

**Steps**

### Phase 1 - Standard themes (CLASSIC, FOG, TRON, EMBER, FAINT)

1. Add `float alpha_scale` field to `GridDrawContext` in scene_grid.c

2. In `scene_grid_render()` (~line 583), compute bg luminance from `g_clear_color[]` (already accessible via `sample.h → repl_state.h`), then compute the scale:
   ```
   bg_lum = 0.2126*R + 0.7152*G + 0.0722*B
   alpha_scale = clamp((0.10 + 0.02) / (bg_lum + 0.02),  min=1.0, max=3.0)
   ```
   Behavior at the extremes:
   | bg_lum | scale |
   |---|---|
   | 0.10 (design) | 1.0 - no change |
   | 0.05 | 1.71× |
   | 0.00 (pure black) | capped at 3.0× |

3. In `draw_grid_standard_theme()` (~line 100), after `spec->line_color(...)` fills `GridLineColors`, multiply both `x_const.a` and `z_const.a` by `ctx->alpha_scale`, clamped to 1.0 - before calling `draw_grid_line_pair()`

4. Same in `draw_grid_standard_theme()` for origin color: scale `origin_c.a` before passing to `draw_grid_origin_axes()`

**Result at pure black:** CLASSIC minor line alpha goes from 0.08 → 0.24, rendering at 0.125 luminance - clearly visible. All alphas are capped at 1.0 so TRON/EMBER don't blow out.

### Phase 2 - Custom themes (FOCUS, PLANES, OCEAN, XZRULER) - deferred

`alpha_scale` will be in `GridDrawContext`, which is already threaded into all custom theme functions. Each can optionally use it in their inline `glColor4f` calls. Can be done in a follow-up.

**Relevant files**
- scene_grid.c - `GridDrawContext` struct, `draw_grid_standard_theme()`, `scene_grid_render()`

**Verification**
1. `make sample` - clean build
2. Run → set clear color to 0.0, CLASSIC theme → lines should be clearly visible
3. Set clear color to 0.1 → grid should be visually identical to current (scale = 1.0 exactly)
4. Confirm alpha cap: TRON at black background doesn't look blown out
5. `make test` - all existing tests pass

**Decisions**
- Design luminance anchored at `0.10` (matches `CP_CLEAR_MAX_V`) - no boost when bg is at max
- Softening constant `K = 0.02` prevents divide-by-zero and keeps the curve smooth near zero
- Max boost cap `3.0×` - empirically gives CLASSIC minor lines 0.24 alpha at pure black
- Phase 2 (FOCUS, PLANES, OCEAN, XZRULER) deferred - they're thematic and less affected

**Further Considerations**
1. **Max boost constant** - 3.0× is a reasonable starting point but may need tuning. FAINT at 3× goes from 0.03→0.09 alpha, still subtle at black. Could be bumped to 4× for FAINT specifically, or we accept that FAINT is inherently hard to see on very dark backgrounds.
2. **Custom themes in Phase 2** - FOCUS and PLANES have the same low-alpha issue. They're lower priority since they're less common, but the infrastructure (alpha_scale in GridDrawContext) will be ready for them.

## Plan: Scene Alpha Boost - Full Propagation (revised)

**Status:** `alpha_scale` is already computed locally in scene_grid.c and applied to the 5 standard themes. Everything below is the work remaining.

**Core change:** Move the computation up to `scene_render_config_init()` once, propagate via two paths: `SceneRenderConfig → GridDrawContext` (already there, just switch the source) and `SceneRenderConfig → SceneGuideSnapshot` (new field) for the guide files.

---

### Phase 1 - Lift alpha_scale to SceneRenderConfig

1. Add `float alpha_scale` to `SceneRenderConfig` in scene_render_types.h
2. Compute it at the end of `scene_render_config_init()` in scene_render.c - same formula currently in scene_grid.c
3. In `scene_grid_render()` in scene_grid.c: remove the local computation block; set `grid_ctx.alpha_scale = config->alpha_scale`

### Phase 2 - Propagate to guide snapshot

4. Add `float alpha_scale` to `SceneGuideSnapshot` in scene_guides_shared.h
5. Copy `config->alpha_scale` into it in `scene_build_guide_snapshot()` in scene_render.c

### Phase 3 - Apply in scene_grid.c custom themes (*parallel with Phase 4–6*)

All four custom themes receive `GridDrawContext *grid_ctx`; add a local `float as = grid_ctx->alpha_scale` and wrap inline alpha literals with `fminf(literal * as, 1.0f)`. Affected calls:

| Theme | Low-alpha literals to boost |
|---|---|
| FOCUS | `base * fx` / `base * fz` (0.06–0.18), crosshair `0.25f` |
| PLANES | floor `0.04f`/`0.10f`, origin `0.30f`, XY/ZY `0.05f`/`0.14f`, origins `0.42f` |
| XZRULER | origin axes `0.70f`, ticks `0.22f`/`0.48f` - high enough to skip, but RULER line colors go through `grid_ruler_line_color` callback which already is in `GridLineColors` → those already go through the existing scaler in `draw_grid_standard_theme`... wait, XZRULER is a custom case that calls `grid_ruler_line_color` directly - it needs the same treatment |
| OCEAN | `base_a` (`0.28f`/`0.55f`), origin `a_o` - the water surface `0.62f * edge` is intentionally transparent, can leave alone |

### Phase 4 - Apply in scene_axes.c (*parallel with Phase 3*)

`scene_axes_render` already receives `FrameRenderContext`, so `frame_ctx->config.alpha_scale` is available. Add `float as = frame_ctx->config.alpha_scale` at the top. Boost the low-alpha inline calls:

| Location | Alpha | Action |
|---|---|---|
| GIZMO XZ floor quad | `0.07f` | boost |
| GIZMO XY/ZY plane fills | `0.11f * xy_w` / `0.11f * zy_w` | boost the `0.11f` constant |
| GIZMO XY/ZY border loops | `xy_a * 2.2f` / `zy_a * 2.2f` | `xy_a` already boosted above |
| NEON outer glow | `0.12f * glow` | boost |
| PULSE trail start | `0.05f` | low but intentional design - optional |
| COMPASS origin dot | `0.6f` | skip (fine) |
| Main axes + labels | `0.85f–1.0f` | skip |

### Phase 5 - Apply in scene_geometry_guides.c (*parallel with Phase 3*)

`scene_geometry_guides_render_for_cursor(const SceneGuideSnapshot *snapshot)` is the entry point; `snapshot->alpha_scale` is available after Phase 2. Add local `float as = snapshot->alpha_scale` and pass it to the plane draw helpers. Affected:

| Function | Alpha | Action |
|---|---|---|
| `draw_guide_yz/xz/xy_plane` | `0.72f` fill, `0.45f` border | boost (add `as` param) |
| Normal guide face normal lines ~L258 | `0.4f` | boost |
| Normal guide arrows L270/277/289/292 | `0.75f`, `0.85f` | optional, skip |
| 2-vertex line guides L125–147 | `0.9f` | skip, already fine |

### Phase 6 - Apply in scene_transform_guides.c (*parallel with Phase 3*)

Same: `snapshot->alpha_scale` after Phase 2. Add `float as = snapshot->alpha_scale`. Affected:

| Function | Alpha | Action |
|---|---|---|
| `draw_pulse_segment` base line | `0.30f` | boost |
| `draw_pulse_segment` trail start | `0.05f` | boost |
| Scale/rotate guide: dim connector L359 | `0.45f` | boost |
| Rotate arc line L511 | `0.30f` | boost |
| Rotate trail L541 | `0.05f` | boost |
| Arrow shafts/heads (0.7–1.0) | fine | skip |

---

**Relevant files**
- scene_render_types.h - add `alpha_scale` to `SceneRenderConfig`
- scene_render.c - compute in `scene_render_config_init`, copy in `scene_build_guide_snapshot`
- scene_guides_shared.h - add `alpha_scale` to `SceneGuideSnapshot`
- scene_grid.c - remove local computation; apply to 4 custom themes
- scene_axes.c - apply to GIZMO low-alpha elements, NEON outer glow
- scene_geometry_guides.c - apply to plane helpers, face-normal dim lines
- scene_transform_guides.c - apply to pulse base/trail, rotate arc/trail, scale connectors

**Verification**
1. `make test-stubs` - all 2418 tests pass (no behavioral changes to non-render code)
2. Run sample, set clear color to 0.0 - verify all themes, axes, and guides are visible
3. Set clear color to 0.1 - visual should be identical to before

**Decisions**
- Formula/constants unchanged: anchor `0.10`, K=`0.02`, max boost `3.0×`
- OCEAN water surface (`0.62 * edge`) deliberately excluded - it's a geometry fill, not a guide line
- XZRULER origin axes `0.70f` can be skipped (already clearly visible at pure black)
- Transform guide pulse trail `0.05f` included - it's barely visible at pure black otherwise
