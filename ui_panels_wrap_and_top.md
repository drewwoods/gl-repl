
Here's the plan for both features:

---

## Plan: Comma-wrap toggle + layout-toggle effort analysis

---

### Feature 1: Wrap long lines at comma boundary

**Approach:** Display-only — no changes to stored data. When `g_wrap_at_comma` is on, the renderer splits a source line at the last comma that fits within the panel width and draws continuation text on successive display lines.

**Steps**

**Phase A — Global flag** *(~5 min)*
1. Add `int g_wrap_at_comma = 0;` to `repl_core.c` near the other `g_show_*` flags
2. Add `extern int g_wrap_at_comma;` to `sample.h`

**Phase B — Wrap helper** *(~15 min)*
3. Add static helper in ui_panels.c:
   `find_comma_wrap(const char *s, int max_chars)` → returns index of last `,` at or before `max_chars`, or `-1` to fall back to hard-wrap

**Phase C — Render loop** *(~45 min)*
4. In `render_code_panel()` (ui_panels.c), replace the single `draw_string(text_x, line_y, src, FONT_MONO)` call with a wrapping loop:
   - Compute `avail_chars = (panel_w - text_x) / FONT_W`
   - If `g_wrap_at_comma` and line exceeds that width: split at comma, draw first chunk, step `line_y -= LINE_H`, draw continuation (indented to opening-paren column or fixed 4-char indent), repeat until exhausted

**Phase D — Scroll accounting** *(~30 min)*
5. Add `count_wrapped_lines(const char *src, int avail_chars)` helper that returns total display lines (≥1) consumed by a source line
6. Use it when computing total display height for scroll extent and `visible_lines` skip logic

**Phase E — Config menu toggle** *(~15 min)*
7. Add `[W] Wrap at commas` row in `render_config_menu()` (ui_panels.c)
8. Wire the key press in the keyboard handler in `repl_core.c` (when config menu open) to toggle `g_wrap_at_comma`

**Total estimated effort: ~2 hours**

---

### Feature 2: Toggle editor to top (above scene) — Effort Analysis

Current: horizontal split — code panel **left** (`g_win_w × g_panel_frac`), scene **right**.
New mode: vertical split — code panel **top** (`g_win_h × g_panel_frac`), scene **bottom**.

| Area | Change | File | Effort |
|---|---|---|---|
| Global flag | `int g_layout_vertical` | `repl_core.c`, `sample.h` | Trivial |
| Panel geometry | `render_code_panel()` uses `panel_w`; conditionalize to `panel_h` in vertical mode | ui_panels.c | Medium |
| Sub-panels | `render_autocomplete`, `render_help`, `render_var_panel`, `render_config_menu` all assume horizontal layout | ui_panels.c | Medium |
| Scene viewport | `glViewport(panel_w, 0, ...)` → `glViewport(0, 0, g_win_w, g_win_h - panel_h)` | scene_render.c | Easy |
| Mouse dispatching | Hit-test splits on X in horizontal; must split on Y in vertical. GLUT's Y is top-down, GL is bottom-up — needs careful flipping | `repl_core.c` | Medium |
| Scroll/visible_lines | `visible_lines` is derived from `g_win_h`; in vertical mode panel height shrinks | ui_panels.c | Easy |
| Hotkey/config entry | Config menu entry or `Ctrl+L` / `Ctrl+T` | `repl_core.c`, `ui_panels.c` | Easy |

**Total estimated effort: ~3–5 hours.** The core change is straightforward (one flag), but the mouse Y-coordinate math, all the sub-panel placements, and the scroll height accounting need careful adjustment across ~6 locations in 3 files.

---

**Relevant files**
- ui_panels.c — primary for both features
- `repl_core.c` — globals, keyboard/mouse handlers, viewport dispatching
- `sample.h` — shared constants and externs
- scene_render.c — viewport setup (Feature 2 only)

---

Ready to proceed with Feature 1? Or would you like any adjustments to the plan first?
