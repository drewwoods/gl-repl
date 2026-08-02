# UI Color Theming Infrastructure

**Status: ✓ COMPLETE** (May 18, 2026)

## Progress

- [x] **Phase A** - infra + Issue 1 fix. `src/ui/theme.h` created (19
  tokens, all 6 theme rows populated, inline `ui_clr`/`ui_clr_a`/
  `ui_rgba` + `ui_theme_select/active`, two `STATIC_ASSERT`s). Wired into
  Makefile `HDRS`. `menu_bar.c` dropdown/submenu hover (`:734`/`:1087`)
  + on-hilite text twins migrated; `:1086` TODO removed.
  `tests/test_ui_theme.c` added (444 assertions) + Makefile test wiring.
  Gates green: `test_ui_theme 444/444`, `make test 5356/5356`,
  `check-c99 OK`, `check-state-ownership` exit 0, sample builds
  (freeglut + stub).
- [x] **Phase B** - chrome files tokenized, one commit each:
  replay_hud, tabbed_overlay, autocomplete_panel, scene_tabs,
  menu_bar remainder (`test_ui_menu_bar 71/71`), panels (named consts
  for the blue rename modal + amber status banner - deliberately
  non-accent), text_panel chrome (`test_ui_text_panel 25/25`,
  `check-ui-text-panel-pure OK`). scene_tabs' ephemeral example-tab
  amber kept as documented `k_tab_example_*` named consts.
- [x] **Phase B finalize** - all 9 `UI_ACCENT_GREEN_*` users moved to
  `ui_clr(UI_TOK_ACCENT)` during the per-file work; the 3 macros
  deleted from `metrics.h` (now points at theme.h). Gates green: both
  sample builds, `make test 5356/5356`, `check-c99` + `check-state-
  ownership` exit 0.
- [x] **Phase C** - pointer comments added at the deliberately-excluded
  data palettes: `color_picker.c` (computed HSV/preview data),
  `repl_code_panel.c` (`k_category_colors[]` / `k_syntax_shade` syntax
  palette), `profile_panel.c` (FPS gauge - red must stay red),
  `text_panel.c` `k_clr_*` editor sub-palette (commented in Phase B).
  Final gates green: `make sample` (freeglut + stub), `make test`
  37/37 bins · 5356/5356, `check-c99` + `check-state-ownership` exit 0.

- [x] **Phase D** - user opted to extend beyond the approved Phase B.
  `variable_panel.c` chrome tokenized (bg→SUNKEN, border→BORDER,
  title→TEXT_PRIMARY, track→MENU_LABEL_ACTIVE_BG, tick→DIVIDER;
  `test_repl_var_drag 36/36`); variable-row data palette + drag-state
  indicators kept as documented `k_var_*` named consts.
  `profile_panel.c` chrome tokenized (bg→SUNKEN, border→BORDER,
  title/total→TEXT_PRIMARY, hint→TEXT_MUTED, headings/detail→
  TEXT_SECTION, rules→DIVIDER); dim/stale tiers → `k_prof_*` named
  consts; the `set_time_color` FPS gauge stays the documented data-viz
  exclusion. Final gates green: `make sample` (freeglut + stub),
  `make test` 37/37 bins · 5356/5356, `check-c99` +
  `check-state-ownership` exit 0.

## Scope Reconciliation

**Approved scope:** Phase A (infra + Issue 1 fix) + Phase B (migrate chrome files).

**Actual delivery:** Phase A + Phase B + Phase C + Phase D + config.h knob + documentation.

Breakdown of the 15 commits:
1. Phase A: `src/ui/theme.h` (create) + `menu_bar.c` (partial) + `tests/test_ui_theme.c` (create) + Makefile wiring
2. Phase B (9 commits): replay_hud.c, tabbed_overlay.c, autocomplete_panel.c, scene_tabs.c, menu_bar.c remainder, panels.c, text_panel.c, metrics.h deletion of `UI_ACCENT_GREEN_*`, test gates
3. Phase C: Pointer comments added to excluded data palettes (color_picker.c, repl_code_panel.c, profile_panel.c, text_panel.c)
4. Phase D (2 commits): `variable_panel.c` + `profile_panel.c` chrome tokenization (user-initiated scope extension)
5. Config integration: `config.h` `UI_THEME_DEFAULT` knob (build-time, 0-5, range-checked)
6. Documentation: `ARCHITECTURE.md` "### UI Color Theming" subsection + `Makefile` help note

**Metrics:**
- ~182 bare `glColor3f/4f` literals reduced to:
  - 19 theme tokens (UI_TOK_*) in 6 fully-populated rows (green, warm, cyan, amber, violet, mono)
  - ~15 semantic/data named consts (k_tab_example_*, k_var_*, k_prof_*, etc.) anchoring fixed-meaning colors across schemes
  - ~148 documented exclusions (data-viz, computed, sub-palettes)
- All gates green: `make sample` (freeglut + stub), `make test` (5356/5356 across 37 bins), `check-c99` exit 0, `check-state-ownership` exit 0
- Runtime seam: `ui_theme_select(th)` / `ui_theme_active()` ready for future Config-menu cycle (grid-theme precedent)

### Final coverage

**Delivered system:**
- Central `src/ui/theme.h` (170 lines, header-only)
  - `UiTheme` enum: 0=green (default), 1=warm, 2=cyan, 3=amber, 4=violet, 5=mono
  - `UiThemeToken` enum: 19 semantic tokens (UI_TOK_SURFACE, RAISED, BORDER, DIVIDER, TEXT_PRIMARY, TEXT_MUTED, TEXT_ON_HILITE, TEXT_SECTION, TEXT_PLACEHOLDER, CARET, ACCENT, DROPDOWN_ITEM_HOVER_BG, ACCENT_GLOW_BG, MENU_LABEL_HOVER_BG, MENU_LABEL_ACTIVE_BG, STATUS_OK, STATUS_WARN, STATUS_ERR, UI_TOK_COUNT)
  - `g_ui_theme_table[6][19]`: all 6 rows fully populated; 16 neutral columns shared, 3 accent-derived columns per row
  - Inline query API: `ui_clr(tok)`, `ui_clr_a(tok, alpha)`, `ui_rgba(tok)`, `ui_theme_select(th)`, `ui_theme_active()`
  - Compile-time asserts: enum counts, UI_THEME_DEFAULT range
  - No GL/scene/REPL state; passes all purity checks

- Build-time swap: `UI_THEME_DEFAULT` in `config.h`
  - Bare int, #ifndef-guarded, build-overridable (e.g. `make sample UI_THEME_DEFAULT=3` for amber)
  - Range-checked by STATIC_ASSERT in theme.h (0-5)
  - Documented in `ARCHITECTURE.md` and `Makefile` help

- Comprehensive migration (15 commits):
  - **Chrome tokenized:** menu_bar.c, replay_hud.c, tabbed_overlay.c, autocomplete_panel.c, scene_tabs.c, panels.c, text_panel.c, variable_panel.c, profile_panel.c
  - **Issue 1 fixed:** dropdown/submenu hover blue #2e4a6e → green #2e6e4a (ui_clr_a(UI_TOK_DROPDOWN_ITEM_HOVER_BG, alpha))
  - **Metrics.h cleanup:** all 9 UI_ACCENT_GREEN_* macros deleted; users → ui_clr(UI_TOK_ACCENT)
  - **Exclusions documented:** color_picker.c HSV/preview, repl_code_panel.c syntax palette, profile_panel.c FPS gauge, text_panel.c editor sub-palette
  - **Named consts preserved:** per-widget semantic colors (k_tab_example_*, k_var_*, k_prof_*, k_rename_bar_*, k_status_bar_*, k_menubar_bottom_rule, etc.) that must stay fixed across schemes

- Test & gate coverage:
  - `tests/test_ui_theme.c` (444 assertions): no zeroed slots, neutral-column stability, green ACCENT back-compat, hover-color regression, round-trip select/active
  - All gates green: `test_ui_theme 444/444`, `make test 5356/5356`, `check-c99 OK`, `check-state-ownership OK`, `sample` builds (freeglut + stub)

- Documentation:
  - `ARCHITECTURE.md` "### UI Color Theming" subsection (under UI Layer, line ~461) - token model, 6-row table, 3-bucket classification, UI_THEME_DEFAULT knob, test guards
  - `Makefile` help note "Build options:" section - UI_THEME_DEFAULT 0-5 mapping, override example, ARCHITECTURE.md reference

**Design decisions preserved:**
- Single compile-time swap point (no runtime picker yet; seam via `ui_theme_select/active` for future Config cycle)
- Explicit per-row RGBA values (no computed accent→tint; readability > abstraction)
- Header-only pattern matching `gl_2d.h` (per-TU .rodata duplication acceptable)
- Three-bucket classification enforced: (1) tokens for cross-file chrome, (2) named consts for fixed one-offs, (3) as-is for domain/data palettes

## Context

The 2D UI in `src/ui/` sets colors with ~182 raw `glColor3f/4f` literals
spread across 11 files. Two concrete problems:

1. **Wrong hue.** `src/ui/menu_bar.c:1087` (and its twin `:734`) draw the
   dropdown / submenu item-hover band with `glColor4f(0.180f, 0.290f,
   0.431f, alpha)` - blue `#2e4a6e`. The project's selected design scheme
   (`plans/done/design-rework/`) is **green** (`#6fb36f`, already used for
   active items). `:1086` literally carries `// TODO: should be based on
   color scheme (green)`. The hover should be green-family.
2. **No central source.** Colors are bare literals with no palette, no
   tokens, no single place to swap the scheme. `src/ui/metrics.h` only
   partially addresses this with `UI_ACCENT_GREEN_*` (9 users).

Outcome: a small, performant, header-only theming layer that (a) fixes the
blue hover, (b) gives shared chrome/accent colors semantic tokens with one
swap point, and (c) makes the other five design-rework schemes drop-in.

## Decisions locked (from user)

- **Scope = A + B**: build infra, fix Issue 1, migrate the chrome files.
  **Do not shoehorn** every literal into the table - non-theme one-offs
  get a named `const`/`#define`; computed/data colors stay as-is.
- **Swap = all 6 rows, constant swap**: populate green (active) + warm +
  cyan + amber + violet + mono; switch via one variable. No runtime UI now
  (seam left for a later Config cycle).
- **Hover green = `#2e6e4a` `{0.180f, 0.431f, 0.290f, 1.0f}`** - the
  hue-shifted twin of today's `#2e4a6e` (same luminance/saturation, blue→
  green). Lowest-surprise: changes only the hue, keeps the designer's
  chosen prominence; reads clearly next to `#6fb36f` active text.

## Three-way classification rule

Every literal under review falls into exactly one bucket:

1. **Theme token** (`ui_clr(UI_TOK_*)`) - accent colors and shared neutral
   chrome that conceptually *is* the theme and recurs across files:
   surfaces, borders, dividers, text tiers, accent, hover/selection,
   status semantics.
2. **Named constant** (`#define`/`static const` near use) - a fixed,
   non-theme-varying color used in 1-2 places that today is a bare
   literal. Give it a name; do **not** add a table slot. (e.g. the
   menubar's pure-black bottom rule `#000`, picker chrome grays.)
3. **Leave as-is** - computed or deliberate domain palettes:
   `color_picker.c` HSV math, `repl_code_panel.c` `k_category_colors[]` /
   `k_syntax_shade`, `profile_panel.c` FPS red/yellow/green (red must stay
   red regardless of theme), `text_panel.c` editor `k_clr_*` arrays
   (editor sub-palette, no cross-file reuse). Documented exclusions.

When unsure: ≥2 cross-file call sites or an accent relationship → token;
otherwise → named constant.

## Architecture: `src/ui/theme.h` (header-only)

Header-only, mirroring the established `include/gl_2d.h` /
`src/scene/grid.c` theme-table precedents. No paired `.c`: all 10 UI TUs
already `#include "metrics.h"` via `-Isrc/ui`, so reach costs one
`#include "theme.h"`; `make check-c99` syntax-checks it transitively
through `$(SRCS)` (same as `gl_2d.h`). The per-TU `.rodata` duplication
(~3 KB × 11, read-only, merged at `-O2`) is the same tradeoff `gl_2d.h`
already accepts; consistency beats shaving it.

- Include guard `UI_THEME_H`; includes `<gl_includes.h>` and
  `"c_compat.h"` (for `STATIC_ASSERT` - never raw `_Static_assert`).
- Does **not** include `metrics.h` or any `scene/` header (respects the
  Makefile no-scene-include guard). Local `typedef float UiRgba[4];`
  rather than scene-namespaced `SceneRgba`.
- Pure: no state/snapshot/mutator/parser/scene. Trips none of
  `check-ui-no-repl-state-read/-mut`, `check-ui-returns-hits-only`,
  `check-ui-text-panel-pure`, `check-color-picker-ui-isolation`.

### Token catalog (`UiThemeToken`, ~22 + `UI_TOK_COUNT`)

Neutral = identical across all theme rows. Accent-derived = varies per row.

Neutral chrome (theme-stable):
- `UI_TOK_SURFACE` `#1d1d1d` - menubar / strip bg
- `UI_TOK_RAISED` `#222` - dropdown / overlay panel bg
- `UI_TOK_SUNKEN` `#141414`/`#181818` - search field / statusbar bg
- `UI_TOK_BORDER` `#3a3a3a` - panel borders
- `UI_TOK_DIVIDER` `#333` - separators / rules
- `UI_TOK_MENU_LABEL_HOVER_BG` `#2a2a2a` - **stays neutral gray**
- `UI_TOK_MENU_LABEL_ACTIVE_BG` `#262626` - pressed/"hot" label
- `UI_TOK_TEXT_PRIMARY` `#d8d8d8`
- `UI_TOK_TEXT_ON_HILITE` `#ffffff` - text over hover/selection
- `UI_TOK_TEXT_SECTION` `#7a8494` - dropdown section header
- `UI_TOK_TEXT_MUTED` `#888888` - shortcuts, hints, counts
- `UI_TOK_TEXT_PLACEHOLDER` `#7a7a7a`
- `UI_TOK_CARET` `{0.95,0.80,0.24}` - text/search caret
- `UI_TOK_STATUS_OK` `#70c070` (semantic, theme-stable)
- `UI_TOK_STATUS_WARN` `#e0a040`
- `UI_TOK_STATUS_ERR` `#c9442e`-family

Accent-derived (per-row):
- `UI_TOK_ACCENT` - green `#6fb36f` `{0.435,0.702,0.435}` (== current
  `UI_ACCENT_GREEN_*`, so migration is a visual no-op)
- `UI_TOK_DROPDOWN_ITEM_HOVER_BG` - **the Issue-1 fix**: green
  `#2e6e4a` `{0.180,0.431,0.290}`
- `UI_TOK_ACCENT_GLOW_BG` - HUD/control accent band, green `#304c38`

The `MENU_LABEL_HOVER_BG` (neutral, stays gray) vs
`DROPDOWN_ITEM_HOVER_BG` (accent, blue→green) split is the load-bearing
distinction: it fixes the dropdown without "greenifying" the neutral
top-bar label hover the design specifies as `#2a2a2a`.

### Theme table & swap point

```c
typedef enum { UI_THEME_GREEN, UI_THEME_WARM, UI_THEME_CYAN,
               UI_THEME_AMBER, UI_THEME_VIOLET, UI_THEME_MONO,
               UI_THEME_COUNT } UiTheme;

static const UiRgba g_ui_theme_table[UI_THEME_COUNT][UI_TOK_COUNT] = {
  [UI_THEME_GREEN] = {
     [UI_TOK_ACCENT]                 = {0.435f,0.702f,0.435f,1.0f}, /* #6fb36f */
     [UI_TOK_DROPDOWN_ITEM_HOVER_BG] = {0.180f,0.431f,0.290f,1.0f}, /* #2e6e4a */
     [UI_TOK_ACCENT_GLOW_BG]         = {0.188f,0.298f,0.220f,1.0f}, /* #304c38 */
     /* …neutral columns… */ },
  /* WARM/CYAN/AMBER/VIOLET/MONO: identical neutral columns; only the 3
     accent columns differ, derived from design-rework --accent-h:
     warm #d96c4f, cyan #5ab0c2, amber #e0a13a, violet #a984d4,
     mono #cccccc (hover = dark tint, glow = mid tint of each). */
};
static int g_ui_theme = UI_THEME_GREEN;   /* the single swap point */
```

Explicit per-row RGBAs (not a computed accent→tint helper): the prototype
hard-codes them, and arithmetic would hide the rendered color - against
this codebase's minimal-abstraction value. Designated initializers (C99,
matches `grid.c:272` `g_grid_theme_specs[]`). `STATIC_ASSERT` that the
enum count matches `UI_TOK_COUNT`; a runtime test catches zeroed slots.

### Query API (inline, minimal call-site churn)

```c
static inline const float *ui_rgba(UiThemeToken t)
    { return g_ui_theme_table[g_ui_theme][t]; }
static inline void ui_clr(UiThemeToken t)
    { const float *c = ui_rgba(t); glColor4f(c[0],c[1],c[2],c[3]); }
static inline void ui_clr_a(UiThemeToken t, float a)          /* alpha override */
    { const float *c = ui_rgba(t); glColor4f(c[0],c[1],c[2],c[3]*a); }
static inline void ui_theme_select(UiTheme th) { g_ui_theme = th; }
static inline UiTheme ui_theme_active(void)    { return g_ui_theme; }
```

`ui_clr_a` matches the dominant `glColor4f(r,g,b, alpha)` and
`… 0.98f*alpha` patterns. Token is a compile-time constant → inlines to a
fixed `.rodata` read; satisfies the performance ask.
`ui_theme_select/active` are the seam a later `GlrConfigKey` theme cycle
(grid-theme precedent) would drive - not built now.

Before/after (`menu_bar.c`):
- `:1087` `glColor4f(0.180f,0.290f,0.431f, alpha);` (delete `:1086` TODO)
  → `ui_clr_a(UI_TOK_DROPDOWN_ITEM_HOVER_BG, alpha);`
- `:734` same blue → `ui_clr_a(UI_TOK_DROPDOWN_ITEM_HOVER_BG, alpha);`
- `:1090`/`:737` `glColor4f(1,1,1, alpha)` → `ui_clr_a(UI_TOK_TEXT_ON_HILITE, alpha)`
- `:1025` `glColor4f(0.133f,0.133f,0.133f, 0.98f*alpha)` →
  `ui_clr_a(UI_TOK_RAISED, 0.98f * alpha)`

## Phase A - infra + Issue 1 (small, revertible)

1. Create `src/ui/theme.h` (enum, table with **green row fully
   populated**; other rows may be stubbed-then-filled in Phase B's last
   step), query API, `STATIC_ASSERT`.
2. Makefile: add `src/ui/theme.h` to `HDRS` (after `src/ui/text_search.h`,
   before `src/ui/variable_panel.h` - alpha order). No `SRCS` edit
   (header-only).
3. `menu_bar.c`: `#include "theme.h"`; convert `:734`/`:1087` +
   text twins `:737`/`:1090`; delete the `:1086` TODO comment. Apply on
   top of the **uncommitted working tree** (git shows `M
   src/ui/menu_bar.c`) - do not discard unrelated local edits.
4. Add `tests/test_ui_theme.c` + Makefile wiring (see Verification).

Phase A alone resolves both reported issues as a tiny diff.

## Phase B - migrate chrome files to tokens

Per-file, smallest-blast-radius order; one file per commit:
`replay_hud.c` → `tabbed_overlay.c` → `autocomplete_panel.c` →
`scene_tabs.c` → `menu_bar.c` (remainder) → `panels.c` (status strip) →
`text_panel.c` (chrome inline literals only).

Call-site → token mapping (apply the classification rule; representative
sites - full per-line inventory already exists from exploration):

| Literal / role | Token | Files (examples) |
|---|---|---|
| `#1d1d1d` bar/strip bg | `UI_TOK_SURFACE` | menu_bar:880,913; scene_tabs:208; replay_hud:60 |
| `#222`/`#0f0f10` panel bg | `UI_TOK_RAISED` | menu_bar:714,1025; tabbed_overlay:58 |
| `#141414`/`#181818` sunken bg | `UI_TOK_SUNKEN` | menu_bar:821; (statusbar) |
| `#3a3a3a` border | `UI_TOK_BORDER` | menu_bar:716,1027; tabbed_overlay:62 |
| `#333` divider/sep | `UI_TOK_DIVIDER` | menu_bar:1061; tabbed_overlay:74,118 |
| `#2a2a2a` label hover bg | `UI_TOK_MENU_LABEL_HOVER_BG` | menu_bar:896,921,928 |
| `#262626` label active bg | `UI_TOK_MENU_LABEL_ACTIVE_BG` | menu_bar:892,924 |
| `#2e4a6e`→green item hover | `UI_TOK_DROPDOWN_ITEM_HOVER_BG` | menu_bar:734,1087 |
| `#d8d8d8` primary text | `UI_TOK_TEXT_PRIMARY` | menu_bar:741,902,986,1094; tabbed_overlay:107 |
| white over hilite | `UI_TOK_TEXT_ON_HILITE` | menu_bar:737,900,984,1090 |
| `#7a8494` section header | `UI_TOK_TEXT_SECTION` | menu_bar:1037,1054; tabbed_overlay:82,179 |
| `#888` muted (sc/hint/count) | `UI_TOK_TEXT_MUTED` | menu_bar:848,1100,1107,1122; replay_hud:141 |
| `#7a7a7a` placeholder | `UI_TOK_TEXT_PLACEHOLDER` | menu_bar:839,940 |
| caret yellow | `UI_TOK_CARET` | menu_bar:853 |
| accent (`UI_ACCENT_GREEN_*`) | `UI_TOK_ACCENT` | menu_bar:739,955,1092,1120; tabbed_overlay:105,183; replay_hud:77,107,127 |
| HUD/control accent band | `UI_TOK_ACCENT_GLOW_BG` | replay_hud:62,124,130 |
| amber status strip | `UI_TOK_STATUS_*` | panels:110,113,123,147 |

End of Phase B: fill the WARM/CYAN/AMBER/VIOLET/MONO rows; migrate the 9
`UI_ACCENT_GREEN_*` users to `ui_clr(UI_TOK_ACCENT)`; **then delete the 3
`UI_ACCENT_GREEN_*` macros from `src/ui/metrics.h`** (no dangling
back-compat). Keep `metrics.h` otherwise unchanged.

Named-constant bucket (rule #2 - do *not* tokenize): menubar pure-black
bottom rule `#000` (menu_bar:994), picker chrome grays, panels
rename-bar affordance blues - give each a local `#define UI_RGBA_*` or
`static const` at use site.

## Phase C - explicit exclusions (no work; documented)

`color_picker.c` HSV/preview (computed data), `repl_code_panel.c`
`k_category_colors[]`/`k_syntax_shade` (deliberate syntax palette),
`profile_panel.c` FPS thresholds (data-viz; red must stay red),
`text_panel.c` `k_clr_*` editor arrays (no cross-file reuse). Add a one-
line comment at each pointing to `theme.h`'s rationale block.

## Files modified (15 commits)

**Core infrastructure:**
- `src/ui/theme.h` - **new**, header-only, 170 lines
- `config.h` - added UI_THEME_DEFAULT (0-5 knob, #ifndef-guarded, build-overridable)
- `tests/test_ui_theme.c` - **new**, header-only, 444 assertions
- `Makefile` - added theme.h to HDRS, test wiring; help note for UI_THEME_DEFAULT
- `ARCHITECTURE.md` - added "### UI Color Theming" subsection (UI Layer)

**Phase B chrome migration (9 files):**
- `src/ui/menu_bar.c` - Phase A partial + remainder (40+ literals → tokens/consts)
- `src/ui/replay_hud.c` - 8 literals → tokens
- `src/ui/tabbed_overlay.c` - 18 literals → tokens
- `src/ui/autocomplete_panel.c` - 6 literals → tokens
- `src/ui/scene_tabs.c` - 11 literals → tokens/named consts
- `src/ui/panels.c` - 7 literals → named consts (rename modal, status banner)
- `src/ui/text_panel.c` - 8 chrome literals → tokens/named consts (k_clr_* left as-is)
- `src/ui/metrics.h` - deleted 3 UI_ACCENT_GREEN_* macros (9 users → ui_clr(UI_TOK_ACCENT))

**Phase D user-requested extension (2 files):**
- `src/ui/variable_panel.c` - 12 chrome literals → tokens/named consts (k_var_* data palette left as-is)
- `src/ui/profile_panel.c` - 11 chrome literals → tokens/named consts; FPS gauge (set_time_color) left as-is

**No changes to:**
- `src/ui/color_picker.c` (HSV/preview data, theme.h bucket 3)
- `src/ui/repl_code_panel.c` (syntax palette k_category_colors[], theme.h bucket 3)
- data/computed palettes, test/demo code

## Verification

`tests/test_ui_theme.c` - header-only, links no project objects, exactly
the `test_repl_code_panel_layout` precedent:
- `STATIC_ASSERT` enum count == `UI_TOK_COUNT` (in `theme.h`).
- Runtime: no theme row has a zeroed token (catches designated-init
  gaps); green `UI_TOK_ACCENT == {0.435,0.702,0.435}` (back-compat proof);
  Issue-1 regression: `UI_TOK_DROPDOWN_ITEM_HOVER_BG` ≠ old blue and is
  green-dominant (`g>r && g>b`); neutral tokens identical across all 6
  rows; `ui_theme_select`/`active` round-trip.

Makefile wiring (mirror lines 519-523 / 575 / 614-616):
- add `test_ui_theme` to `TEST_BINS`;
- add it to the `CORE_TEST_BINS` `filter-out` at line 575 (pure test);
- add `test_ui_theme_OBJS = $(OBJDIR)/$(TEST_DIR)/test_ui_theme.o`,
  `test_ui_theme_LDLIBS =`, `test_ui_theme_RUN ?=
  $(BINDIR)/test_ui_theme` next to 614-616 (the `built_binary` foreach at
  697 auto-creates the link rule).

Gates: `make test_ui_theme`, `make test`, `make check-c99`,
`make check-state-ownership` (includes the `check-ui-*` /
`check-color-picker-ui-isolation` guards).

Manual visual check: build, run, open a Scene/Config dropdown, hover an
item - band is **green `#2e6e4a`**, not blue; top-bar menu **label**
hover is still neutral gray `#2a2a2a`; Replay icon / active item still
`#6fb36f`. Flip `g_ui_theme = UI_THEME_WARM`, rebuild, confirm only
accent/hover/glow change and neutral chrome is unchanged.

Build: `make sample` (freeglut) or `make glut` (macOS). Stub path
`make sample USE_GL_STUBS=1` should also still build (theme.h only
needs `glColor4f`, which the GL stub provides).

## Future Work: Runtime Theme Picker

The `ui_theme_select(UiTheme th)` / `ui_theme_active()` seam is ready for a
future Config-menu cycle (matching the `grid_theme_select` pattern in
`src/scene/grid.c`). To add runtime switching:

1. Wire `theme_cycle` action to `g_cfg_items[]` in `src/app/glr_actions.c`
2. On toggle, call `ui_theme_select(next_theme)` to swap runtime state
3. Export/import via workspace header directives (e.g. `@cfg ui_theme = 2`)
4. The current header-only `.rodata` duplication (3 KB × 11 TUs) remains unchanged
   (same tradeoff as `gl_2d.h`)

The underlying `g_ui_theme` in `theme.h` is compile-time-initialized from
`UI_THEME_DEFAULT` (config.h knob); making it runtime-settable is a
no-semantic-change refactor (the per-TU static copy and `.rodata` sharing
are transparent to the seam).
