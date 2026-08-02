
Here's the plan for both features:

---

## Plan: Comma-wrap toggle + layout-toggle effort analysis

---

### Feature 1: Wrap long lines at comma boundary

**Approach:** Display-only - no changes to stored data. When `g_wrap_at_comma` is on, the renderer splits a source line at the last comma that fits within the panel width and draws continuation text on successive display lines.

**Steps**

**Phase A - Global flag** *(~5 min)*
1. Add `int g_wrap_at_comma = 0;` to `repl_core.c` near the other `g_show_*` flags
2. Add `extern int g_wrap_at_comma;` to `sample.h`

**Phase B - Wrap helper** *(~15 min)*
3. Add static helper in ui_panels.c:
   `find_comma_wrap(const char *s, int max_chars)` → returns index of last `,` at or before `max_chars`, or `-1` to fall back to hard-wrap

**Phase C - Render loop** *(~45 min)*
4. In `render_code_panel()` (ui_panels.c), replace the single `draw_string(text_x, line_y, src, FONT_MONO)` call with a wrapping loop:
   - Compute `avail_chars = (panel_w - text_x) / FONT_W`
   - If `g_wrap_at_comma` and line exceeds that width: split at comma, draw first chunk, step `line_y -= LINE_H`, draw continuation (indented to opening-paren column or fixed 4-char indent), repeat until exhausted

**Phase D - Scroll accounting** *(~30 min)*
5. Add `count_wrapped_lines(const char *src, int avail_chars)` helper that returns total display lines (≥1) consumed by a source line
6. Use it when computing total display height for scroll extent and `visible_lines` skip logic

**Phase E - Config menu toggle** *(~15 min)*
7. Add `[W] Wrap at commas` row in `render_config_menu()` (ui_panels.c)
8. Wire the key press in the keyboard handler in `repl_core.c` (when config menu open) to toggle `g_wrap_at_comma`

**Total estimated effort: ~2 hours**

---

### Feature 2: Toggle editor to top (above scene) - Effort Analysis

Current: horizontal split - code panel **left** (`g_win_w × g_panel_frac`), scene **right**.
New mode: vertical split - code panel **top** (`g_win_h × g_panel_frac`), scene **bottom**.

| Area | Change | File | Effort |
|---|---|---|---|
| Global flag | `int g_layout_vertical` | `repl_core.c`, `sample.h` | Trivial |
| Panel geometry | `render_code_panel()` uses `panel_w`; conditionalize to `panel_h` in vertical mode | ui_panels.c | Medium |
| Sub-panels | `render_autocomplete`, `render_help`, `render_var_panel`, `render_config_menu` all assume horizontal layout | ui_panels.c | Medium |
| Scene viewport | `glViewport(panel_w, 0, ...)` → `glViewport(0, 0, g_win_w, g_win_h - panel_h)` | scene_render.c | Easy |
| Mouse dispatching | Hit-test splits on X in horizontal; must split on Y in vertical. GLUT's Y is top-down, GL is bottom-up - needs careful flipping | `repl_core.c` | Medium |
| Scroll/visible_lines | `visible_lines` is derived from `g_win_h`; in vertical mode panel height shrinks | ui_panels.c | Easy |
| Hotkey/config entry | Config menu entry or `Ctrl+L` / `Ctrl+T` | `repl_core.c`, `ui_panels.c` | Easy |

**Total estimated effort: ~3-5 hours.** The core change is straightforward (one flag), but the mouse Y-coordinate math, all the sub-panel placements, and the scroll height accounting need careful adjustment across ~6 locations in 3 files.

---

**Relevant files**
- ui_panels.c - primary for both features
- `repl_core.c` - globals, keyboard/mouse handlers, viewport dispatching
- `sample.h` - shared constants and externs
- scene_render.c - viewport setup (Feature 2 only)

---

Ready to proceed with Feature 1? Or would you like any adjustments to the plan first?

## Plan: Optional Top Code Panel

Add an optional layout mode that keeps the current left-side editor as the default but can switch the code panel to a full-width strip at the top, with the 3D scene below it. The key correction from the earlier draft is that top mode should improve long-line readability because the code panel keeps full window width and only trades height, so the existing comma-wrap logic should continue to use panel width, not panel height. The safest implementation is to introduce shared panel/scene geometry helpers first, then route viewport, code-panel rendering, hit-testing, resize behavior, and overlay placement through those helpers.

**Steps**
1. Phase 1 - Layout model and shared geometry
   Add a new layout toggle in /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c and /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/sample.h, preferably as a two-state global such as g_panel_layout or g_layout_vertical.
   Define shared geometry helpers that answer code-panel rect, scene rect, split position, and resize axis. These can live in /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c as static helpers plus any needed externs, or in a shared header if both ui_panels.c and scene_render.c need them directly.
   Use one consistent convention: left mode means code rect = x 0..panel_w, y 0..g_win_h; top mode means code rect = x 0..g_win_w, y g_win_h-panel_h..g_win_h.

2. Phase 2 - Scene viewport and HUD placement
   Update render_3d_scene in /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/scene_render.c to derive viewport origin and size from the shared scene rect instead of assuming glViewport(panel_w, 0, scene_w, g_win_h).
   Recompute aspect ratio from the scene rect dimensions, not from scene_w and g_win_h.
   Update draw_replay_hud in /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/scene_render.c so it anchors inside the scene rect in both modes rather than using hud_x = panel_w + 18.
   This phase depends on Phase 1.

3. Phase 3 - Code panel rendering in top mode
   Refactor render_code_panel in /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c to use code rect width and height separately.
   Keep wrap calculations width-based by feeding the code-panel width into code_panel_available_chars, code_wrap_iter_init, code_panel_row_count_for_text, and related helpers.
   Change visible row calculations to use code-panel height instead of g_win_h.
   Change background and border drawing to draw only the code rect: vertical divider in left mode, horizontal divider in top mode.
   Add clipping or strict visible-line limiting if needed so text never bleeds into the scene area when the top panel is short.
   This phase depends on Phase 1 and should be implemented before hit-testing so the render geometry becomes the source of truth.

4. Phase 4 - Code-panel hit testing, cursoring, and drag selection
   Update code_panel_hit_test, code_panel_drag_target, handle_code_panel_click, and handle_code_panel_drag in /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c to use the code rect instead of hard-coded x < panel_w checks.
   Preserve the current GLUT-to-OpenGL Y conversion pattern, but simplify the logic in top mode by testing whether the GLUT y coordinate falls inside the top strip before converting rows.
   Keep the wrapped-row mapping helpers as the single source of truth for visual-row to logical-line translation.
   This phase depends on Phase 3.

5. Phase 5 - Mouse routing, wheel routing, and resize handle
   Update mouse_func, mousewheel_func, passive_motion_func, and motion_func in /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c to branch on the active split axis.
   Left mode keeps the current x-versus-panel_w checks. Top mode should dispatch code-panel interactions when the pointer is inside the top strip and scene interactions otherwise.
   Resize behavior should become axis-aware: left mode uses x / g_win_w; top mode uses panel_h / g_win_h derived from the dragged Y position.
   Cursor feedback should also switch from a left-right resize cursor to an up-down resize cursor in top mode.
   This phase depends on Phase 1 and Phase 4.

6. Phase 6 - Overlay and panel placement cleanup
   Update var_panel_geom and cfg_panel_geom in /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c to clamp against the scene rect rather than assuming the scene starts at vp_left = g_win_w * g_panel_frac.
   The variable panel can likely stay bottom-right of the scene in both layouts; in top mode this is simpler than previously estimated because the code panel no longer intrudes from the left.
   The config menu should be centered within the scene rect in both layouts.
   Review render_autocomplete to make sure popup clamping uses code-panel width in left mode and full window width in top mode, while still respecting the code panel bounds vertically.
   This phase can run in parallel with late Phase 5 cleanup once the geometry helpers exist.

7. Phase 7 - Toggle exposure and persistence behavior
   Add the new layout option to the config menu table in /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c so the existing config overlay can expose it without bespoke UI work.
   If a direct hotkey is desired, add it after the config path is working, not before.
   Reset-state behavior in repl_reset_state should return to the default left layout unless there is an explicit reason to preserve the last mode.
   This phase depends on Phase 1 and can be completed early if desired.

8. Phase 8 - Validation and regression coverage
   Add focused tests around any geometry or code-panel dump helpers that can be validated headlessly, especially if shared rect helpers are introduced.
   Re-run the existing focused suite from /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking: make -s sample test_repl_core_format test_repl_core_examples, then run the binaries from the sample directory so relative fixtures resolve correctly.
   Manually verify in both layouts: panel resizing, click-to-cursor placement, wrapped long lines, autocomplete popup placement, scroll-wheel routing, variable panel dragging, config menu hit testing, and replay HUD placement.
   This phase depends on all prior phases.

**Relevant files**
- /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c - add layout toggle state, config exposure, mouse routing, wheel routing, passive-motion cursor updates, and resize-axis logic in mouse_func, mousewheel_func, passive_motion_func, motion_func, and repl_reset_state.
- /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/sample.h - add externs or shared layout declarations used by both UI and scene code.
- /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c - primary implementation site for code rect and scene rect helpers, render_code_panel, wrapped-row helpers, code_panel_hit_test, code_panel_drag_target, render_autocomplete, var_panel_geom, and cfg_panel_geom.
- /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/scene_render.c - update render_3d_scene viewport and aspect ratio math, plus draw_replay_hud anchoring.
- /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/test_repl_core_format.c - likely place for any new headless layout or visual-dump assertions if geometry helpers expose stable text behavior.
- /Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/test_repl_core_examples.c - rerun to ensure layout work does not disturb the logical code-panel dump expectations.

**Verification**
1. Build from the sample directory with make -s sample test_repl_core_format test_repl_core_examples.
2. Run ./test_repl_core_format and ./test_repl_core_examples from the sample directory.
3. Manual check in left mode: existing behavior unchanged for scene viewport, code-panel sizing, resize handle, and mouse-wheel routing.
4. Manual check in top mode: code panel spans full width at the top, scene stays below, long wrapped lines become visibly wider than in left mode, and the resize handle moves vertically.
5. Manual check in both modes: click placement inside wrapped editor rows, selection dragging, autocomplete popup clamping, variable panel drag, config menu hover and click, replay HUD, and scroll-wheel routing.

**Decisions**
- Included scope: optional top-versus-left layout toggle for the existing editor and scene, including rendering, mouse routing, overlays, and validation.
- Excluded scope: redesigning the editor UI, changing wrap behavior beyond existing width-based wrapping, or introducing persistent settings storage unless separately requested.
- Recommendation: introduce shared geometry helpers before any conditional panel logic. This is the main refinement over the earlier plan and should materially reduce regressions.
- Recommendation: keep the variable panel anchored to the scene rect rather than inventing a new dedicated placement scheme. Top mode makes this easier, not harder.
- Revised estimate: 4 to 6 hours of focused implementation and verification. The earlier estimate understated the benefit of full-width top mode for wrapping, but still understated the value of a geometry-helper refactor for safe implementation.

**Further Considerations**
1. Layout toggle naming: recommend a layout enum or named mode field over a bare boolean so the code remains extensible if a future bottom or hidden-panel mode is added.
2. Resize limits: recommend asymmetric clamps such as a larger minimum height for the top panel than the current 10 percent minimum, because too-short top layouts would make the editor unusable even though long-line width improves.
3. Popup clipping: recommend adding explicit code-rect and scene-rect clamp helpers now, because autocomplete, config, and variable panels all become simpler and less error-prone once they share the same bounds model.

Compacted conversation

Read memory [](file:///Users/drew/Library/Application%20Support/Code/User/workspaceStorage/efaf9a5c02e0279de64f86d20a1f50b4/GitHub.copilot-chat/memory-tool/memories/MDlkMTNlMzYtY2M3My00MWZlLTkzMzgtNTJkMmU2NDFjNmI4/plan.md)

Good, I have the full plan. Now let me explore the current codebase state to verify accuracy and spot anything that needs updating.

Verify key function line numbers

Check panel_h concept and resize logic

Read [](file:///Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#1-1), lines 1 to 100

Read [](file:///Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/sample.h#1-1), lines 1 to 200

Searched for files matching `src` · `immediate-mode-repl/claude4.6-opus-thinking/*.c`, 18 matches

Read memory [](file:///Users/drew/Library/Application%20Support/Code/User/workspaceStorage/efaf9a5c02e0279de64f86d20a1f50b4/GitHub.copilot-chat/memory-tool/memories/MDlkMTNlMzYtY2M3My00MWZlLTkzMzgtNTJkMmU2NDFjNmI4/plan.md)

Searched for regex `motion_func|passive_motion_func|panel_h|var_panel_geom|cfg_panel_geom|render_code_panel` (`src` · `immediate-mode-repl/claude4.6-opus-thinking/**`), 20 results

Read [](file:///Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/sample.h#200-200), lines 200 to 385

Read [](file:///Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#100-100), lines 100 to 300

Searched for text `panel_h` (`src` · `immediate-mode-repl/claude4.6-opus-thinking/**`), 17 results

Searched for text `cfg_panel_geom` (`src` · `immediate-mode-repl/claude4.6-opus-thinking/**`), 6 results

Searched for regex `g_wrap_at_comma|g_panel_frac|g_cfg_items|g_layout_vertical|g_panel_layout` (`src` · `immediate-mode-repl/claude4.6-opus-thinking/repl_core.c`), 20 results

Searched for regex `g_wrap_at_comma|g_panel_frac|g_layout_vertical|g_panel_layout` (`src` · `immediate-mode-repl/claude4.6-opus-thinking/sample.h`), 2 results

Read [](file:///Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#6656-6656), lines 6656 to 6760

Read [](file:///Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c#1500-1500), lines 1500 to 1645

Read [](file:///Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c#631-631), lines 631 to 680

Searched for regex `^void mouse_func|^void mousewheel_func|^void passive_motion_func|^void motion_func|^void repl_reset_state` (`src` · `immediate-mode-repl/claude4.6-opus-thinking/repl_core.c`), 1 result

Searched for regex `^(static )?(void|int) (mouse_func|mousewheel_func|passive_motion_func|motion_func|repl_reset_state)` (`src` · `immediate-mode-repl/claude4.6-opus-thinking/repl_core.c`), 5 results

Read [](file:///Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#700-700), lines 700 to 725

Searched for regex `^(static )?(void|int|float|Rect|CodeWrapIter) (render_code_panel|render_autocomplete|var_panel_geom|cfg_panel_geom|code_panel_hit_test|code_panel_drag_target|code_panel_rect|scene_rect)` (`src` · `immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c`), 6 results

Searched for regex `code_panel_rect|scene_rect|CodeWrapIter` (`src` · `immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c`), 9 results

Searched for regex `^(static )?(void|int) (render_3d_scene|draw_replay_hud)|hud_x\s*=|glViewport` (`src` · `immediate-mode-repl/claude4.6-opus-thinking/scene_render.c`), 5 results

Read [](file:///Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/ui_panels.c#245-245), lines 245 to 260

Good data. I found a correctness issue in Phase 5 and a few clarifications needed. Let me update the plan.

Updated memory file [](file:///Users/drew/Library/Application%20Support/Code/User/workspaceStorage/efaf9a5c02e0279de64f86d20a1f50b4/GitHub.copilot-chat/memory-tool/memories/MDlkMTNlMzYtY2M3My00MWZlLTkzMzgtNTJkMmU2NDFjNmI4/plan.md)

Updated memory file [](file:///Users/drew/Library/Application%20Support/Code/User/workspaceStorage/efaf9a5c02e0279de64f86d20a1f50b4/GitHub.copilot-chat/memory-tool/memories/MDlkMTNlMzYtY2M3My00MWZlLTkzMzgtNTJkMmU2NDFjNmI4/plan.md)

Updated memory file [](file:///Users/drew/Library/Application%20Support/Code/User/workspaceStorage/efaf9a5c02e0279de64f86d20a1f50b4/GitHub.copilot-chat/memory-tool/memories/MDlkMTNlMzYtY2M3My00MWZlLTkzMzgtNTJkMmU2NDFjNmI4/plan.md)

Updated memory file [](file:///Users/drew/Library/Application%20Support/Code/User/workspaceStorage/efaf9a5c02e0279de64f86d20a1f50b4/GitHub.copilot-chat/memory-tool/memories/MDlkMTNlMzYtY2M3My00MWZlLTkzMzgtNTJkMmU2NDFjNmI4/plan.md)

Updated memory file [](file:///Users/drew/Library/Application%20Support/Code/User/workspaceStorage/efaf9a5c02e0279de64f86d20a1f50b4/GitHub.copilot-chat/memory-tool/memories/MDlkMTNlMzYtY2M3My00MWZlLTkzMzgtNTJkMmU2NDFjNmI4/plan.md)

Good. Now let me present the fully updated plan to you:

---

## Plan: Optional Top Code Panel

Add an optional layout mode that keeps the current left-side editor as the default but can switch the code panel to a full-width strip at the top, with the 3D scene below it. In top mode, long-line readability improves because the code panel spans full window width. The safest implementation is to introduce shared panel/scene geometry helpers first, then route viewport, code-panel rendering, hit-testing, resize behavior, and overlay placement through those helpers.

---

**Steps**

**Phase 1 - Layout model and shared geometry** *(gateway - nothing else should start without this)*
- Add `int g_layout_vertical = 0` to `repl_core.c` (near `g_wrap_at_comma` ~line 551) and `extern int g_layout_vertical` to `sample.h`. Neither file has this variable yet.
- Reuse `g_panel_frac`, but it changes semantic meaning by mode: left → fraction of window WIDTH (`panel_w = g_win_w * g_panel_frac`); top → fraction of window HEIGHT (`panel_h_px = g_win_h * g_panel_frac`). Clamps [0.1, 0.9] stay the same.
- Create `code_panel_rect()` and `scene_rect()` helpers (don't exist yet) in `ui_panels.c`. OpenGL-coordinate convention: left mode = code rect `{0, 0, panel_w, g_win_h}`; top mode = code rect `{0, g_win_h - panel_h_px, g_win_w, panel_h_px}`.

**Phase 2 - Scene viewport and HUD** *(depends on Phase 1)*
- `render_3d_scene` (scene_render.c:1059): replace hard-coded `glViewport(panel_w, 0, scene_w, g_win_h)` at line 1069 with values from scene rect helper.
- `draw_replay_hud` (scene_render.c:1013): replace `hud_x = panel_w + 18` at line 1017 with scene rect `x + 18` so HUD anchors inside the scene in both modes.

**Phase 3 - Code panel rendering** *(depends on Phase 1; do before hit-testing)*
- `render_code_panel` (ui_panels.c:631) takes no parameters - computes `panel_w` from globals. Replace with code rect helper at the top so it gets correct width/height in both modes.
- `visible_lines` currently `(g_win_h - ...) / LINE_H` - must use code rect height.
- Background/border draw only the code rect; vertical divider in left mode, horizontal divider in top mode.

**Phase 4 - Hit testing and drag** *(depends on Phase 3)*
- `code_panel_hit_test` (ui_panels.c:1729) and `code_panel_drag_target` (ui_panels.c:1763): replace `mx < 0 || mx >= panel_w` bounds checks with code rect bounds checks.
- GLUT→GL Y conversion pattern stays unchanged; just the bounds test changes.

**Phase 5 - Mouse routing, wheel, and resize handle** *(depends on Phase 1 and Phase 4)*
- All four handlers in `repl_core.c`: `mouse_func:6516`, `mousewheel_func:6638`, `passive_motion_func:6656`, `motion_func:6674`.
- Left mode: existing checks unchanged (`abs(x - panel_w) < 10`, `x < panel_w`).
- Top mode: replace x-axis checks with y-axis - resize: `abs(y - panel_h_px) < 10`; routing: `y < panel_h_px`.
- **Key formula (verified correct):** `motion_func` resize in top mode → `g_panel_frac = (float)y / g_win_h` (dragged GLUT y divided by height). The earlier plan said `(g_win_h - y)` which was wrong - GLUT y=0 is at the top, so GLUT y of the divider is directly the panel height fraction.
- Cursor: `passive_motion_func` changes from `GLUT_CURSOR_LEFT_RIGHT` to `GLUT_CURSOR_UP_DOWN` in top mode.

**Phase 6 - Overlay placement** *(parallel with late Phase 5 once geometry helpers exist)*
- `var_panel_geom` (ui_panels.c:1500): left-edge clamp `vp_left + 4` becomes `4` in top mode (scene starts at x=0, so clamp simplifies naturally).
- `cfg_panel_geom` (ui_panels.c:1622): vertical centering currently uses `(g_win_h - *ph) / 2` (full window) - in top mode this bleeds into the code strip. Change to `scene_y + (scene_h - *ph) / 2`. Horizontal centering within scene width already works via `vp_left`.
- `render_autocomplete` (ui_panels.c:1080): confirm popup clamping uses code-panel width (full window in top mode).

**Phase 7 - Config menu toggle** *(depends on Phase 1; can run early)*
- Add entry to `g_cfg_items[]` (repl_core.c:701) - currently 19 entries; this becomes item 20.
- Toggle callback should reset `g_panel_frac = 0.3f` when switching modes, otherwise a wide left panel becomes an unusably tall top panel.
- `repl_reset_state` (line 7071): return to left layout and reset `g_panel_frac` to default.

**Phase 8 - Validation**
- Build + test: `make -s sample test_repl_core_format test_repl_core_examples` from sample directory. Run `./test_repl_core_format` (49 tests), `./test_repl_core_examples` (74 tests).
- Manual left mode: existing behavior unchanged - viewport, resize handle, scroll, click placement, wrapped lines.
- Manual top mode: code panel spans full width at top, scene below, resize handle is horizontal, wrapped lines visibly wider, all mouse routing correct.
- Manual both modes: autocomplete popup bounds, variable panel drag, config menu hover/click, replay HUD, scroll-wheel routing.

---

**Relevant files**
- `repl_core.c` - new toggle global; config table (19→20); `mouse_func`, `mousewheel_func`, `passive_motion_func`, `motion_func`, `repl_reset_state`
- `sample.h` - new extern for `g_layout_vertical`
- `ui_panels.c` - primary site: new geometry helpers, `render_code_panel`, hit testing, `var_panel_geom`, `cfg_panel_geom`, `render_autocomplete`
- `scene_render.c` - `render_3d_scene:1059` viewport, `draw_replay_hud:1013` HUD anchor

**Decisions**
- `g_panel_frac` is reused across modes (different semantic meaning per mode) to minimize new state. Mode switch resets it to 0.3f.
- `g_layout_vertical` preferred over an enum for simplicity; extend later if a third mode is ever needed.
- Excluded: persistent settings, redesigned UI, any wrap behavior change.
- Geometry helpers are introduced first - this is the primary risk mitigation.

**Corrections vs. previous plan**
1. `motion_func` formula was wrong: should be `g_panel_frac = y / g_win_h`, not `(g_win_h - y) / g_win_h`
2. `cfg_panel_geom` vertical centering will bleed into the code strip in top mode - explicit fix added to Phase 6
3. Phase 7 now includes `g_panel_frac` reset on mode switch to prevent unusable panel sizes
4. `g_cfg_items` count called out explicitly (19 currently, 20 after)
