# Gold-Standard State Ownership

> **Working directory:** `/Users/drew/src/code/opengl/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/`
> **Branch base:** `imrepl/repl-cleanup-push-refinement`

## Context

The existing `repl_state.h` / `repl_state_views.h` / `repl_state_owners.h` facade is a typed bundle of pointers into globals living in `repl_state.c`. It was a mechanical rewrite of `extern` globals — type-safe, but not actually a layering tool. The R6 split made the *outer* pointer `const`, but the inner fields are still `int *`, `float *`, `char *`, etc. — so `*cp->cursor_px = cursor_x` at `ui_panels.c:250` compiles and writes through a "view" with no compiler complaint. That bug is structural, not a missed call site.

R1 made `scene_*` fully snapshot-driven (zero `repl_state_*` calls). UI is intentionally one rung lower: read-only access via views, mutation routed through actions/stores. But UI still pulls live globals during render (~114 read sites), and the const-doesn't-propagate hole means even reads aren't actually read-only.

This plan dissolves the facade. State migrates into per-domain owner modules. Public headers expose only by-value snapshot accessors and named mutator functions. Renderers consume frozen `const Ui*View *` snapshots — no struct-of-pointers anywhere in any public surface.

The intended outcome:
- Writing through a view fails to compile — not by grep, by type.
- The facade's 23 struct types and ~132 functions disappear; their replacements live with their owners.
- `ui_*.c` renderers are pure functions of a snapshot (modulo GL).
- New checks defined *up front* prevent regression and measure progress as the work lands.

## Tenets

These are the nine rules the end state must satisfy. Every step in the plan below moves at least one of them from "documented" to "compiler-enforced."

1. **Domains own their data; there is no facade.** Each domain's state lives `static` inside its owning `.c` file. No `ReplRuntimeState` aggregate. No struct-of-pointers re-export. The facade headers (`repl_state.h`, `repl_state_views.h`, `repl_state_owners.h`) and `repl_state.c` are deleted at the end.
2. **Two kinds of public function, period.** Each owner header exposes only (a) read accessors that return scalars by value, arrays as `(const T *, int n)` pairs, or aggregates as by-value snapshot structs whose fields are values; and (b) named mutator functions. No `_mut()` accessors. No exposed struct-of-pointers. No field assignment from outside the owner.
3. **Renderers consume only snapshots; render is pure.** The controller (`imrepl_ctrl.c`) builds per-frame `Ui*View` and `Scene*View` snapshots once, then passes `const T *view` to renderers. Renderers are pure functions of their snapshot (modulo GL emission). View files do not include any `repl_*` state header for reading state, and they do not call any mutator function during render.
4. **Render-time mutations flow through intent outputs, not direct writes.** Some state is *computed* during rendering and consumed elsewhere — cursor pixel position is the canonical example: layout produces it as a side effect of drawing, but the autocomplete popup in a different panel needs it. Renderers express these as fields on a `Ui*Output *out` struct passed alongside the const view. After the render call, the controller reads `out` and dispatches to owner setters (`repl_code_panel_set_cursor_pixel(out->cursor_px, out->cursor_py)`). Renderers never call setters directly.
5. **Input mutations go through stores and actions, never field writes.** Every UI input handler and editor verb calls a named function (`repl_action_*`, `repl_*_set_*`, `repl_*_insert`, etc.). Input handlers are *not* renderers; they're allowed to call mutators directly because input dispatch is the controller's job. No `*x->y = z` survives anywhere outside the owner module's own `.c`.
6. **The controller is the only "knows about everything" module.** `imrepl_ctrl.c` includes every owner header for snapshot assembly, output actualization, and lifecycle. No other module composes across domains.
7. **Tests build fixtures, not poke globals.** Each renderer takes `(const Ui*View *in, Ui*Output *out)`. Tests construct `in` in place, call the renderer, and assert against `out` (and any GL-stub effects). No test calls `repl_state_*_mut()` or pokes globals to set up scenarios.
8. **Headers reflect the rule.** Per domain, a single public header (`repl_<domain>.h`) and an optional sibling/test internal header (`repl_<domain>_internal.h`). No "views.h" / "owners.h" split because there is nothing that needs splitting — read access is by-value, write access is functions, and the type signature distinguishes them.
9. **Lifecycle is explicit.** Each owner has `repl_<domain>_init()` and `repl_<domain>_reset()`. The controller calls them in deterministic order. No first-read lazy init.

---

## Render-Time Mutations: Intent Outputs, Controller Actualization

This section makes tenet 4 concrete. It is the only mechanism by which a renderer is allowed to influence model state.

### The problem

Some state is *produced* by rendering. The cursor's pixel position (`cursor_px`, `cursor_py`) is the canonical case:

- The render path in `ui_panels.c` is the only place that knows where the glyph for the cursor's character index actually landed in screen space, because the answer depends on font metrics, line wrapping, scroll offset, and the panel rect.
- The autocomplete popup in `ui_autocomplete_panel.c` needs that screen position one frame later (or even the same frame, in the same paint pass) to anchor itself under the cursor.
- We can't precompute it in the controller — the controller doesn't have the per-glyph layout.
- We can't make it pure-functional (recompute on demand in the popup) without duplicating the layout walk.

The current code solves this by writing through a `const`-typed view pointer into a global. That is the bug.

### The pattern

Every UI render function has the dual signature:

```c
void ui_panels_render(const UiPanelsView *in, UiPanelsOutput *out);
```

- `in` is the read-only snapshot, built once per frame by the controller.
- `out` is a zero-initialized output struct the controller passes by reference. The renderer fills any fields it computed.

Output structs are small, flat, by-value, and explicit. Example for the cursor case:

```c
typedef struct UiPanelsOutput {
    /* Cursor pixel position computed during glyph layout. Valid only if
     * cursor_pixel_valid is set. */
    int cursor_px;
    int cursor_py;
    int cursor_pixel_valid;

    /* If set, the renderer detected scroll has overshot and requests a
     * controller-side clamp. (Hypothetical; real fields determined per panel.) */
    int request_scroll_clamp;
    int requested_scroll;
} UiPanelsOutput;
```

After the render call, the controller actualizes:

```c
UiPanelsView in;
UiPanelsOutput out = {0};
imrepl_ctrl_build_panels_view(&in);
ui_panels_render(&in, &out);

if (out.cursor_pixel_valid)
    repl_code_panel_set_cursor_pixel(out.cursor_px, out.cursor_py);
if (out.request_scroll_clamp)
    repl_code_panel_set_scroll(out.requested_scroll);
```

### What goes in `out`, what doesn't

`out` is **render-side-effect state** — values produced during drawing that other consumers need.

- ✅ `cursor_px / cursor_py` — layout-computed, consumed by `ui_autocomplete_panel`.
- ✅ Hover hit-test results computed during render that input next-frame would re-derive.
- ✅ "I drew a status banner; status TTL should tick" — though most status mutation is input-driven, not render-driven.

What does **not** belong in `out`:

- ❌ Anything triggered by input — that goes through the controller's input router into `repl_action_*` directly. `out` is only for things the *render path itself* discovered.
- ❌ Any mutation that the controller could compute itself given the view. If it's a pure function of the snapshot, compute it controller-side and put it in `in` for the next frame.
- ❌ Cross-panel data flow — that goes through the controller assembling another panel's `in` from owner state, not through one panel's `out`.

### Why not just call the setter from the renderer

It would compile. It would even work. But:

- It puts mutation in the render path, where every other write is forbidden.
- It makes the renderer impure — testing requires owner state to be initialized, can't construct a fixture and assert against a struct.
- It re-creates the layering hole we're closing: "render reads, doesn't write" becomes "render reads, doesn't write *most things*."
- It distributes the actualization decision. With `out`, the controller has one place to enforce ordering, batching, and undo-snapshot timing across all panels.

### Optional outputs

Panels that have no render-side-effect state pass `NULL` for `out` (or take no `out` parameter). Stage 1's `ui_help_overlay` is an example — the help overlay is purely visual; nothing about its render produces state another panel needs. Stage 4's `ui_panels` is the first panel that needs `UiPanelsOutput`.

The convention: if a panel needs `out`, its function is `void ui_<panel>_render(const Ui<Panel>View *in, Ui<Panel>Output *out)`. If not, it's `void ui_<panel>_render(const Ui<Panel>View *in)`. The check `check-ui-renderer-takes-view` (below) accepts both shapes.

### Test pattern

```c
UiPanelsView in = {
    .scroll = 0, .cursor_pos = 12, .doc = { .count = 3, .cmds = ... },
};
UiPanelsOutput out = {0};
ui_panels_render(&in, &out);
assert_int_eq(out.cursor_pixel_valid, 1);
assert_int_eq(out.cursor_px, expected_px);
```

No globals touched. No facade involved. The test is independent of every other module.

---

## Checks Defined Up Front

These checks are added to the `Makefile` (and `scripts/`) **before** the migration work begins. Each starts with a documented allowlist or baseline that ratchets down as steps land. Wired into `make test` so every commit either holds the line or improves it.

The checks are the contract. The migration steps below are *how* we drive them green.

### check-no-write-through-view (NEW)

```makefile
check-no-write-through-view:
	@echo "Checking for writes through view pointers..."
	@bad=$$(grep -nE '^\s*\*[a-z_][a-zA-Z0-9_]*->[a-zA-Z0-9_]+\s*(=[^=]|\+\+|--|\+=|-=|\*=|/=)' \
		$(SCENE_SRCS) $(UI_SRCS) | grep -vE '//.*$$' || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: writes through view-pointer fields:"; \
		echo "$$bad"; exit 1; \
	fi
	@echo "View-pointer write boundary OK"
```

Catches `*cp->cursor_px = ...`. Initial allowlist: `ui_panels.c:250-251`. Goes empty when stage 4 lands.

### check-views-flat-types (NEW)

`scripts/check-views-flat.sh` — parses `repl_state_views.h` and any `Ui*View` / `Scene*View` struct definitions, fails if any struct field is a non-const pointer to mutable data.

```sh
#!/bin/bash
set -euo pipefail
HEADERS="repl_state_views.h ui_*.h scene_render_types.h scene_guides_shared.h"
bad=$(awk '
  /^typedef struct/,/^} [A-Za-z_]+;/ {
    if ($0 ~ /^[[:space:]]+[A-Za-z_][A-Za-z0-9_ ]*\*[[:space:]]*[A-Za-z_]/ \
        && $0 !~ /const/ \
        && $0 !~ /\(\*/) {
      print FILENAME ":" NR ": " $0
    }
  }
' $HEADERS)
if [ -n "$bad" ]; then
  echo "ERROR: view structs contain non-const pointer fields:"
  echo "$bad"
  exit 1
fi
```

Initial baseline: every existing facade struct fails this. Tracked count goes from 23 → 0 across stages.

### check-views-by-value-snapshot (NEW)

```makefile
check-views-by-value-snapshot:
	@echo "Checking new view struct accessors return by-value..."
	@bad=$$(grep -nE 'const\s+[A-Z][A-Za-z0-9_]*View\s*\*' \
		repl_state.h repl_state_views.h $(UI_HDRS) || true); \
	# Allow legitimate "const View *" parameter passing in renderer signatures;
	# the check is for accessor *return types* only.
```

Initial form is informational; fails when an owner accessor newly added during this work returns `const Foo *` instead of `Foo` by value. (Allowlist for legacy facade until stages 3-5 retire each domain.)

### check-ui-renderer-takes-view (NEW)

```makefile
check-ui-renderer-takes-view:
	@bash scripts/check-ui-renderer-signatures.sh
```

`scripts/check-ui-renderer-signatures.sh` — verifies that each registered UI render entry point in the allowlist matches one of two signatures:

```c
void ui_<panel>_render(const Ui<Panel>View *in);
void ui_<panel>_render(const Ui<Panel>View *in, Ui<Panel>Output *out);
```

The allowlist starts as `{ ui_replay_hud_render }` and grows as panels migrate (stage 9). Renderers may not take additional positional arguments — anything else is upgraded to a field on the view.

### check-renderer-no-direct-mutators (NEW)

This is the structural enforcement of tenet 4. Renderers in the allowlist may not call any mutator function directly; they may only fill their `Ui*Output` struct.

```makefile
check-renderer-no-direct-mutators:
	@bash scripts/check-renderer-purity.sh
```

`scripts/check-renderer-purity.sh` — for each function in the migrated-renderers allowlist (starts with `ui_replay_hud_render`, grows per stage 9), greps inside the function body for forbidden patterns:

- Calls to any `repl_*_set_*` function.
- Calls to any `repl_action_*` function.
- Calls to `repl_state_*_mut()` (which by stage 8 doesn't exist anyway).
- Direct assignment through a non-output pointer (`*foo->bar = ...` where `foo` isn't the output param).

Permitted inside renderers: GL/GLU calls, calls to other `ui_*` helpers that themselves take `(in, out)`, calls to `gl2d_*` helpers, calls to formatting/layout helpers in `repl_code_panel_layout.c` and `cmd_format.c` that are pure.

Permitted writes: assignment to fields of the output parameter only (`out->cursor_px = ...`).

The script extracts function bodies by brace-matching from a pattern like `^void ui_<panel>_render\(...\)\s*\{` to its matching `^\}` and applies the forbidden-pattern grep to the body. It maintains an allowlist of "renderer functions audited so far" so partial migrations don't trigger noise on unmigrated panels.

### check-output-actualization (NEW)

For every `Ui*Output` struct used in a render call, the controller must actualize each field. This catches cases where a renderer fills a field but the controller forgets to dispatch it.

```makefile
check-output-actualization:
	@bash scripts/check-output-actualization.sh
```

`scripts/check-output-actualization.sh` — parses every `Ui*Output` struct in `ui_*.h`, lists its fields, then verifies that `imrepl_ctrl.c` references each field name (read of `out.<field>` or `output.<field>`) at least once after the corresponding render call. Misses produce a warning at first; promoted to error after stage 9 stabilizes the pattern.

This is a soft sanity check, not a layering check. It prevents render-side-effects from being silently dropped.

### check-mut-accessor-count (NEW, ratchet)

```makefile
check-mut-accessor-count:
	@count=$$(grep -E 'repl_state_[A-Za-z0-9_]*_mut\s*\(' $(REPL_SRCS) $(UI_SRCS) | wc -l); \
	baseline=$$(cat scripts/baselines/mut-count.txt); \
	if [ $$count -gt $$baseline ]; then \
		echo "ERROR: _mut() accessor calls increased: $$count > $$baseline"; \
		exit 1; \
	fi; \
	echo "_mut() count: $$count (baseline $$baseline)"
```

Ratchet target file: `scripts/baselines/mut-count.txt`. Decreases monotonically. PRs that increase it must justify in commit message.

### check-state-c-shrinking (NEW, ratchet)

```makefile
check-state-c-shrinking:
	@lines=$$(wc -l < repl_state.c); \
	baseline=$$(cat scripts/baselines/state-c-lines.txt); \
	if [ $$lines -gt $$baseline ]; then \
		echo "ERROR: repl_state.c grew: $$lines > $$baseline"; \
		exit 1; \
	fi
```

Initial baseline: current `repl_state.c` line count. Goes to 0 by end of stage 8.

### check-no-facade-include-in-views (TIGHTENED)

Tightens the existing `check-views-no-owners`. After each domain migrates in stages 3-5, its consumers must include the new domain header, not `repl_state.h`. Final form:

```makefile
check-no-facade-include-in-views:
	@bad=$$(grep -lE '#include\s+"repl_state(_views|_owners)?\.h"' \
		$(SCENE_SRCS) $(UI_SRCS) || true); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: views still include facade headers:"; \
		echo "$$bad"; exit 1; \
	fi
```

Allowlist starts as the full set of `ui_*.c`. Each migrated UI panel drops off the list.

### check-domain-owner-encapsulation (NEW)

`scripts/check-domain-encapsulation.sh` — for each `repl_*` domain owner module created in stages 3-5, verifies:
- The module's statics are not declared `extern` in any header.
- No other `.c` file references `g_<domain>_*` symbols by name.
- The owner header exposes only function declarations and by-value snapshot types.

Per-module enforcement; allowlist of "not yet migrated" modules shrinks across stages.

### check-cursor-px-encapsulated (NEW, scoped)

Specific to the bug we found:

```makefile
check-cursor-px-encapsulated:
	@bad=$$(grep -nE 'cursor_p[xy]' $(UI_SRCS) | \
		grep -vE '^(repl_code_panel|ui_replay_hud)\.c:'); \
	if [ -n "$$bad" ]; then \
		echo "ERROR: cursor_px/py referenced outside its owner:"; \
		echo "$$bad"; exit 1; \
	fi
```

Initial allowlist: `ui_panels.c`, `ui_autocomplete_panel.c` (consumer for popup positioning). Goes to empty when stage 4's `repl_code_panel` lands and the field becomes either `repl_code_panel_cursor_pixel()` (if cached) or computed-on-demand from layout.

### Existing checks that stay (and tighten as stages land)

These are already in `Makefile`. Each tightens its allowlist as the corresponding stage completes:

- `check-gl-boundaries` — unchanged
- `check-layer-coupling` — unchanged
- `check-controller-boundaries` — unchanged
- `check-scene-no-repl-state-mut` — unchanged (already passing)
- `check-pure-scene-no-repl-state` — unchanged (already passing)
- `check-state-boundaries` — allowlist shrinks as stages 3-5 land
- `check-views-no-owners` — same
- `check-ui-no-repl-state-mut` — same
- `check-public-api-usage` — unchanged

### Wiring

Add a `check-state-ownership` umbrella target that runs all the new checks plus the tightened existing ones. Wire into `make test` so a single `make test` proves the contract.

```makefile
check-state-ownership: check-no-write-through-view check-views-flat-types \
                      check-views-by-value-snapshot check-ui-renderer-takes-view \
                      check-renderer-no-direct-mutators check-output-actualization \
                      check-mut-accessor-count check-state-c-shrinking \
                      check-no-facade-include-in-views check-domain-owner-encapsulation \
                      check-cursor-px-encapsulated

test: ... check-state-ownership
```

---

## Stages

Each stage is one or more reviewable commits. Every stage ends with:

1. The relevant check's allowlist/baseline tightened in `Makefile` or `scripts/baselines/*.txt`.
2. `make test` green.
3. Manual smoke test of `./sample` (load example via F12, edit, save, reload).

### Stage 0 — Land the checks (no behavior change)

Add every check above to `Makefile` and `scripts/`. Initial allowlists/baselines reflect the *current* state. `make test` is green from commit one.

Files:
- `Makefile` — new check targets, umbrella `check-state-ownership`, wire into `test`
- `scripts/check-views-flat.sh` — new
- `scripts/check-ui-renderer-signatures.sh` — new
- `scripts/check-domain-encapsulation.sh` — new
- `scripts/baselines/mut-count.txt` — new (current count)
- `scripts/baselines/state-c-lines.txt` — new (current `wc -l repl_state.c`)
- `scripts/baselines/views-flat-violations.txt` — new (current per-struct violation count)

This is the contract before any migration. After this commit, every subsequent commit either holds the baselines or improves them.

### Stage 1 — Pilot the pattern on `repl_help`

`ui_help_overlay` is the smallest end-to-end domain (3 fields: `visible`, `tab_idx`, `scroll`). Use it to lock in the migration template that stages 2-5 and 7-9 follow. Help has no render-side-effect state, so this stage demonstrates the **single-arg** renderer signature; stage 4 adds the `out` arg for the cursor case.

Files added:
- `repl_help.h` — public header with by-value `ReplHelpView` (fields by value), `repl_help_view()` accessor, `repl_help_set_visible(int)`, `repl_help_set_tab(int)`, `repl_help_set_scroll(int)`, `repl_help_init()`, `repl_help_reset()`. **No struct-of-pointers, no `_mut()`.**
- `repl_help.c` — moves `g_show_help`, `g_help_tab`, `g_help_scroll` from `repl_state.c` to `static` inside this file.
- `ui_help_overlay.h` — adds `UiHelpOverlayView` (snapshot of help fields plus viewport rect). No `UiHelpOverlayOutput` because help produces no render-side-effect state.
- `ui_help_overlay.c` — `ui_help_overlay_render()` signature changes to `void ui_help_overlay_render(const UiHelpOverlayView *in)`. No `repl_state_*` calls inside. No mutator calls inside (input handlers like F1 toggle remain in the input path, untouched).

Files modified:
- `repl_state.c` — globals removed, facade `ReplHelpState` accessors deleted.
- `repl_state.h` / `repl_state_views.h` / `repl_state_owners.h` — `ReplHelpState` and its accessors deleted.
- `repl_actions.c` — `repl_action_help_tab_next` calls `repl_help_set_tab` instead of `repl_state_help_mut`.
- `imrepl_ctrl.c` — assembles `UiHelpOverlayView` from `repl_help_view()` plus viewport, passes to `ui_help_overlay_render()`. Calls `repl_help_init()` in `imrepl_ctrl_init_gl`.
- `repl_undo.c` — does not capture help state today; no change.
- `Makefile` — `repl_help.c` added to `SRCS` and `CORE_TEST_SRCS`. Allowlists for tightened checks lose `ui_help_overlay.c`.
- `scripts/baselines/*.txt` — counts decremented.

Tests:
- `test_ui_help_overlay.c` (new, small) — constructs a fixture `UiHelpOverlayView`, verifies render-relevant computations on it. Demonstrates the no-globals-in-tests pattern.

Stage 1 exit criteria:
- `make test` green with tightened allowlists.
- `ui_help_overlay.c` includes neither `repl_state.h` nor `repl_state_views.h`.
- `repl_state.c` shrinks by ~10 lines; `mut-count.txt` baseline drops by ~3.

### Stage 2 — Document the pilot pattern

Update `MODULES.md`, `ARCHITECTURE.md`, and `CLAUDE.md` with the migration template extracted from stage 1. One short section: "Adding or migrating an owner module."

This is documentation only, but it's load-bearing for stages 3-5, 7, and 9 because they apply the template repeatedly.

### Stage 3 — Migrate small single-field/few-field domains

Apply the stage-1 template to:

- `repl_variable_panel` (1 field: `visible`) — owner of `g_show_var_panel`
- `repl_profile_panel` (1 field: `mode`) — owner of `g_show_profile_panel`
- `repl_status` (2 fields: text, ttl) — owner of `g_status[]`, `g_status_ttl`
- `repl_viewport` (2 fields: w, h) — owner of `g_win_w`, `g_win_h`
- `repl_pointer` (3 fields: mouse_x, mouse_y, mouse_button) — owner of `g_mouse_*`
- `repl_var_drag` (4 fields) — owner of `g_drag_*` (the `repl_var_drag.c` file already exists; this just moves storage from facade to its statics)
- `repl_search` (9 fields) — owner of `g_search_*` (`repl_search.c` already exists; same pattern)
- `repl_autocomplete` (8 fields plus arrays) — owner of `g_ac_*` (existing `.c` file)
- `repl_selection` (2 fields) — owner of `g_sel_anchor`, `g_sel_end`

Each domain is one commit. Per commit:
1. New `repl_<domain>.h` exposes by-value snapshot type, getters, named setters, init/reset.
2. Globals migrate from `repl_state.c` to static in the owner `.c`.
3. Facade struct and accessors removed.
4. Consumers updated; the owner header replaces `repl_state.h` includes where possible.
5. Baselines tightened.

Stage 3 exit:
- ~30 globals migrated.
- `repl_state.c` ~30% smaller.
- `mut-count.txt` ~30% lower.
- `check-views-flat-types` violation count drops by ~9 structs.

### Stage 4 — Migrate `repl_code_panel` and kill cursor_px through-write

`ReplCodePanelRuntimeState` has 8 fields. Owner of: `g_panel_frac`, `g_resizing_panel`, `g_scroll`, `g_scroll_follow_cursor`, `g_cursor_visible`, `g_blink_tick`, `g_cursor_px`, `g_cursor_py`.

Special handling for `cursor_px` / `cursor_py` — this is the canonical render-side-effect case and the first use of the intent/output pattern.

The fields are *not* dead. `ui_autocomplete_panel.c:29-30` reads them to anchor the popup under the cursor. The write happens during the code-panel render in `ui_panels.c:250-251`, because layout is the only place that knows where the glyph for the cursor's character index landed. We can't precompute (controller doesn't have layout), and we can't push to a pure recompute without duplicating the layout walk in the autocomplete panel.

The redesign uses the intent/output pattern. `ui_panels_render` becomes:

```c
typedef struct UiPanelsView {
    /* read-only snapshot — populated by controller from repl_code_panel_view(),
     * repl_document_view(), search/autocomplete views, etc. */
    int  scroll;
    int  cursor_visible;
    int  cursor_pos;
    /* ... */
} UiPanelsView;

typedef struct UiPanelsOutput {
    int cursor_px;
    int cursor_py;
    int cursor_pixel_valid;
    /* future: scroll-clamp request, hover hits, etc. */
} UiPanelsOutput;

void ui_panels_render(const UiPanelsView *in, UiPanelsOutput *out);
```

Inside the renderer, the bug-line becomes:

```c
out->cursor_px = cursor_x;
out->cursor_py = io_line_y;
out->cursor_pixel_valid = 1;
```

`out` is a stack local of the controller. The compiler enforces tenet 4: there is no symbol named `cursor_px` reachable as a writable global from inside a renderer. The only way to communicate a cursor-pixel value out of `ui_panels_render` is via `out`.

The controller actualizes:

```c
UiPanelsView panels_view;
UiPanelsOutput panels_out = {0};
imrepl_ctrl_build_panels_view(&panels_view);
ui_panels_render(&panels_view, &panels_out);
if (panels_out.cursor_pixel_valid)
    repl_code_panel_set_cursor_pixel(panels_out.cursor_px, panels_out.cursor_py);
```

`ui_autocomplete_panel.c` reads the cursor pixel via *its* view (`UiAutocompletePanelView`), which the controller populates from `repl_code_panel_view()` *after* the panels actualization. This means the autocomplete popup uses the cursor-pixel value computed in this same frame's panel render — exact semantic parity with the global-write today, but explicit and traceable.

Files:
- `repl_code_panel.h` / `repl_code_panel.c` — new owner. Public API: `ReplCodePanelView repl_code_panel_view()`, `repl_code_panel_set_scroll(int)`, `repl_code_panel_set_cursor_visible(int)`, `repl_code_panel_set_cursor_pixel(int x, int y)`, `repl_code_panel_init()`, `repl_code_panel_reset()`. **No struct-of-pointers, no `_mut()`.**
- `repl_state.{h,c,_views.h,_owners.h}` — `ReplCodePanelRuntimeState` removed.
- `ui_panels.h` — adds `UiPanelsView` and `UiPanelsOutput` types.
- `ui_panels.c` — render function takes `(const UiPanelsView *in, UiPanelsOutput *out)`. Lines 250-251 fill `out->cursor_px`, `out->cursor_py`, `out->cursor_pixel_valid`. All `repl_state_*` reads become `in->...` reads. No mutator calls inside the render path.
- `ui_autocomplete_panel.h` — adds `UiAutocompletePanelView` containing `cursor_px`, `cursor_py`, `cursor_visible`, plus its existing autocomplete fields.
- `ui_autocomplete_panel.c` — render takes `const UiAutocompletePanelView *in`; reads `in->cursor_px` instead of `*cp->cursor_px`.
- `imrepl_ctrl.c` — assembles `UiPanelsView` and `UiPanelsOutput`, calls `ui_panels_render`, actualizes the output by calling `repl_code_panel_set_cursor_pixel`, *then* assembles `UiAutocompletePanelView` from the now-updated `repl_code_panel_view()` and calls `ui_autocomplete_panel_render`. Frame ordering documented in the controller. Any render-time lifecycle work discovered in this slice (for example workspace-header refresh style bookkeeping) moves to explicit controller pre-render setup, not renderer bodies.
- `Makefile` — `check-cursor-px-encapsulated` allowlist goes empty. `check-no-write-through-view` allowlist goes empty. `check-renderer-no-direct-mutators` allowlist gains `ui_panels_render` and `ui_autocomplete_panel_render`.

Stage 4 exit: the cursor_px through-write is a compile-time impossibility. The render-side-effect pathway is documented and runs through one canonical pattern — every subsequent panel that has a cursor_px-shaped problem follows the same template.

Note on Option B (kill the cache, recompute in autocomplete): now strictly a follow-up. It's a layout-math change, not an ownership change. Once the intent/output pattern is in place, choosing to remove the field is a one-line change to the output struct and a one-function change to the autocomplete panel. Defer the choice to a separate discussion.

### Stage 5 — Migrate medium domains

Apply the template to:

- `repl_camera` — already has good private logic in `repl_camera_controls.c`; this stage moves the storage out of the facade into the owner. Owner of `g_cam_*` (8 fields).
- `repl_animator` — owner of `g_anim_time`, `g_t_playing`, `g_t_var_idx`. Coordinates with `repl_eval` (which owns `g_predef_vars`).
- `repl_clipboard` — owner of `g_clipboard[]`, `g_clipboard_count`. (`repl_clipboard.c` exists.)
- `repl_input` — owner of `g_input[]`, `g_input_len`, `g_cursor_pos`, `g_newline_buf[]`, `g_newline_len`, `g_inserting` (6 fields).
- `repl_presentation` — large (~25 fields), owner of `g_wireframe`, `g_grid_*`, `g_axes_theme`, `g_show_*` flags, etc.
- `repl_render_quality` — owner of `g_use_accum`, `g_multisample_enabled`, `g_lights[]`, `g_clear_color[4]`, etc. (10+ fields).
- `repl_workspace` — owner of `g_workspace_dir`, `g_example_idx` (formerly part of `ReplSceneRuntimeState`).

Stage 5 exit:
- ~80 globals total migrated.
- `repl_state.c` ~70% smaller.
- `mut-count.txt` baseline near zero for non-large domains.

### Stage 6 — Move cross-layer input routing into the controller

State ownership migration does not by itself guarantee that `imrepl_ctrl.c` is the only mixed-layer module. This stage moves the UI-aware input routing chain out of `repl_editor.c` and into the controller.

Scope:

- `imrepl_ctrl.c` owns key and mouse precedence across help, search, menu bar, color picker, variable panel, code panel, and camera controls.
- `repl_editor.c` stays focused on REPL editing verbs and domain/store calls, with no `ui_*` include knowledge.
- Routing dispatch calls focused action/store APIs and owner setters; no facade reach-through is introduced.

Tests:

- Add focused controller input-routing tests that assert precedence and dispatch outcomes for key and mouse events across the major overlays and interaction modes.

Stage 6 exit:
- `repl_editor.c` no longer composes across UI modules.
- Controller boundary is explicit in both code and tests.

### Stage 7 — Migrate large domains: replay, document, flat_program

Three biggest, most cross-cutting domains.

- `repl_replay` — `repl_replay.c` already owns its private statics; this stage moves the facade-stored fields (`g_replay_*`) into it. The header gains `ReplReplayView repl_replay_view()` and a `UiReplayHudView` builder (already partially there as `UiReplayHudState`).
- `repl_document` — owner of `g_cmds[]`, `g_num_cmds`, `g_edit_line`, `g_normals_dirty`. Coordinates with existing `repl_command_store.c` (which becomes the mutator API). Consumers switch to `repl_document_view()` returning a snapshot containing `(const GLCmd *cmds, int count, int edit_line, ...)` — array stays as a const pointer, scalars are by value.
- `repl_flat_program` — owner of `g_flat_cmds[]`, `g_flat_cmd_local_vars[]`, `g_num_flat_cmds`, `g_flat_dirty`, `g_user_lighting_enabled`, `g_current_block_*`. Already has `FlatProgramView` partially modeling this.

The cursor block highlighting fields (`g_current_block_begin/end/line`) live in `repl_flat_program` but their *update* is driven from `repl_flatten`. Owner-internal seam, no cross-domain changes needed.

Stage 7 exit:
- All globals migrated except `repl_export.c` strings.
- `repl_state.c` only contains `repl_state_init_defaults()` and `repl_state_reset_all()` forwarders.

### Stage 8 — Migrate `repl_export` strings; delete the facade

- `repl_export.c` — already exists and now owns its render-state-strings, cam-lines, workspace-header-lines, scene-name-hint, and pending-* buffers (~7 fields).
- `imrepl_ctrl.c` — calls each domain's `_init()` and `_reset()` in order; the old `repl_state_init_defaults` and `repl_state_reset_all` are dissolved into per-domain functions called from a small `imrepl_ctrl_state_init()`.
- **Delete** `repl_state.c`, `repl_state.h`, `repl_state_views.h`, `repl_state_owners.h`. Replace with nothing — every consumer already includes per-domain headers.
- `repl_undo.c` — captures snapshots by calling each domain's view/serialize accessor, not by reaching into globals.

Stage 8 exit:
- `repl_state.c` line count is 0 (file deleted).
- All allowlists in `check-state-ownership` empty.
- `repl_state.h` no longer exists; any straggler include is a compile error.
- All tenets except full UI snapshot purity (completed in stage 9) are compiler-enforced.

### Stage 9 — UI snapshot completion (intent/output pattern across the board)

Stages 1-8 fix the *write* problem. Stage 9 fixes the *read liveness* problem: panels still read live state during render, just through new domain headers instead of the old facade. After this stage, every UI renderer is pure: `(const Ui*View *in)` or `(const Ui*View *in, Ui*Output *out)` is the only signature shape.

Migrate each `ui_*.c` renderer using `ui_replay_hud.c` (single-arg) and `ui_panels.c` (dual-arg, established in stage 4) as templates. For each panel, decide up front:

- **Single-arg** if the renderer produces no state another consumer needs (`ui_help_overlay`, `ui_profile_panel`, `ui_variable_panel` — purely visual or input-driven).
- **Dual-arg** if the renderer computes state that another component reads (`ui_panels`, possibly `ui_menu_bar` for hover/dropdown geometry).

Order from smallest to largest:

1. `ui_help_overlay` (already done in stage 1, single-arg)
2. `ui_variable_panel` — single-arg
3. `ui_profile_panel` — single-arg
4. `ui_autocomplete_panel` — single-arg (already adapted in stage 4 to consume cursor pixel via view)
5. `ui_color_picker` — likely single-arg; mutations from drag are already routed through `repl_command_store_set_color` (R2a)
6. `ui_menu_bar` — possibly dual-arg if dropdown geometry needs to flow back; otherwise single-arg
7. `ui_panels` — already dual-arg from stage 4; this step extends the view/output structs to cover the remaining ~77 reads

For each panel:
- Add `Ui<Panel>View` (and `Ui<Panel>Output` if needed) to the panel's `.h`.
- Convert the render function to the new signature.
- Replace every `repl_state_*` or per-domain `repl_<domain>_view()` call inside the render body with `in->...`.
- If the panel had a forbidden mutator call inside render (none should exist after stage 8, but verify), move it to an `out` field and actualize in the controller.
- Add the controller-side assembly: `imrepl_ctrl_build_<panel>_view(&view)`; `Ui<Panel>Output out = {0};`; `ui_<panel>_render(&view, &out);` and the actualization block.
- Add the panel to the `check-renderer-no-direct-mutators` allowlist.
- Add a focused test that constructs a fixture view, runs the renderer, and asserts on `out` plus GL-stub state.
- Tighten allowlists in `check-no-facade-include-in-views`, `check-ui-renderer-takes-view`, `check-output-actualization`.

Stage 9 exit:
- Every UI renderer is a pure function of its inputs (modulo GL emission and the optional `out` parameter).
- `check-renderer-no-direct-mutators` allowlist contains every UI render entry point — proof that the rule is enforced everywhere, not just on a sample.
- No `ui_*.c` includes any per-domain `repl_*` state header for *reading* — only the controller does. Action headers (`repl_actions.h`, `repl_command_store.h`) are still included for input-handler dispatch; that's expected and outside the render path.
- `check-output-actualization` is promoted from warning to error.

### Stage 10 — Final cleanup and documentation

- Remove the now-empty allowlists from every check.
- Update `ARCHITECTURE.md`, `MODULES.md`, and `CLAUDE.md` to describe the gold standard, not the staged transition.
- Mark `feature/push-architecture-refinement.md` Phase 2 complete with reference to this plan as the structural follow-through.
- Open a follow-up for cursor_px Option B (kill the cache, recompute in the autocomplete panel) if Option A was taken in stage 4.
- Keep the `sample` to `imrepl` rename as a separate mechanical follow-up after ownership stabilization; do not bundle it into this migration.

---

## Critical Files (modified across the plan)

Per-domain new files (stages 1, 3, 4, 5, 6):
- `repl_help.{h,c}`, `repl_variable_panel.{h,c}`, `repl_profile_panel.{h,c}`, `repl_status.{h,c}`, `repl_viewport.{h,c}`, `repl_pointer.{h,c}`, `repl_selection.{h,c}`, `repl_input.{h,c}`, `repl_animator.{h,c}`, `repl_workspace.{h,c}`, `repl_render_quality.{h,c}`, `repl_code_panel.{h,c}`, `repl_presentation.{h,c}`, `repl_document.{h,c}`, `repl_flat_program.{h,c}`
- (Existing files extended into full owners: `repl_camera_controls.{h,c}`, `repl_clipboard.{h,c}`, `repl_search.{h,c}`, `repl_autocomplete.{h,c}`, `repl_var_drag.{h,c}`, `repl_replay.{h,c}`, `repl_export.{h,c}`)

Files heavily modified across stages:
- `repl_state.{h,c}`, `repl_state_views.h`, `repl_state_owners.h` — shrink to nothing, deleted in stage 8
- `imrepl_ctrl.c` — config build expands to per-domain snapshot calls
- `repl_editor.c` — input routing/composition surface shrinks in stage 6
- `repl_undo.c` — capture switches to per-domain snapshots
- `Makefile` — checks added in stage 0, allowlists shrink across stages
- `MODULES.md`, `ARCHITECTURE.md`, `CLAUDE.md` — updated stages 2 and 10

Files where `*.h` becomes a `Ui*View` consumer (stage 9):
- `ui_panels.{c,h}`, `ui_menu_bar.{c,h}`, `ui_color_picker.{c,h}`, `ui_variable_panel.{c,h}`, `ui_autocomplete_panel.{c,h}`, `ui_profile_panel.{c,h}`, `ui_help_overlay.{c,h}` (stage 1)

Existing utilities reused:
- `SceneRenderConfig` builder pattern in `imrepl_ctrl.c:imrepl_ctrl_build_scene_config` — generalize to per-UI-panel snapshot builders.
- `UiReplayHudState` in `ui_replay_hud.h` — the view struct template.
- `repl_command_store.{h,c}` — already a store-style mutator module; keep, refactor under `repl_document` owner.
- `repl_actions.{h,c}` — already action-style; keep, route new mutations through it.
- `FlatProgramView` in `repl_flatten.h` — already a by-value snapshot type for arrays.
- `ReplPredefView` in `repl_eval.h` — already by-value snapshot pattern.

---

## Verification

Per stage:

1. `make clean && make sample` — main build green.
2. `make sample USE_GL_STUBS=1` — stub build green.
3. `make test` — runs all checks including `check-state-ownership`.
4. `make check-state-ownership` directly — verify boundaries and baselines.
5. `./sample` smoke test:
   - Startup with no args.
   - Load built-in example via F12 cycle.
   - Edit, save (Ctrl+S), exit, reload `./sample output.c`.
   - Toggle wireframe, grid theme, axes theme.
   - Replay start/pause via Ctrl+G.
   - Camera orbit/pan/zoom.
   - Help overlay (F1) — sanity check after stage 1.
   - Color picker open/edit (after `repl_command_store_set_color` covers in stage 7).
   - Search (Ctrl+F) — sanity check after stage 3.
6. Per-stage focused tests:
   - Stage 1: new `test_ui_help_overlay` runs with fixture, no globals.
   - Stage 6: add controller input-routing precedence tests for key and mouse dispatch across overlays.
   - Stages 3-8: existing tests (`test_repl_core_*`, `test_repl_editor`, `test_scene_render`, etc.) pass without modification — if a test was poking globals, it migrates to fixture-based.

Final verification (after stage 10):

- `repl_state.c` no longer exists (`ls repl_state.c` fails).
- `grep -r 'repl_state_' .` returns matches only inside `scripts/`, `MODULES.md`, `ARCHITECTURE.md`, and historical commit messages.
- `make check-state-ownership` green with all allowlists empty.
- All nine tenets are compiler-enforced (or, where C cannot enforce, grep-enforced with empty allowlists).
- No `*x->y = z` write-through-view pattern compiles anywhere in the tree.
- Every UI render entry point matches one of the two canonical signatures: `(const Ui*View *in)` or `(const Ui*View *in, Ui*Output *out)`. No third shape exists.
- Every `Ui*Output` field has at least one actualization site in `imrepl_ctrl.c`.
- Controller input-routing tests prove key/mouse precedence across help/search/menu/color-picker/variable-panel/code-panel/camera paths.

---

## Risks and notes

- **Stage 7 (replay/document/flat_program) is the largest commit.** Consider splitting the document migration into a separate sub-stage if review burden is high.
- **Tests that currently call `repl_state_*_mut()` directly** (e.g., `test_ui.c:167-168`) will break in their stage. They migrate to per-domain setters or owner-internal headers — *not* by exposing the mut accessor through a back door.
- **Stage 6 input routing is cross-cutting.** Keep it scoped to routing ownership and precedence tests; do not hide unrelated editor or UI behavior refactors in the same commits.
- **The `ReplUndoSnapshot` struct** captures full editor state. After stage 8, it gathers per-domain snapshots instead of poking globals. The serialization format does not change; only the assembly pathway does.
- **`repl_eval.c` predefined variables** (`g_predef_vars`, `g_num_predef_vars`) are already domain-encapsulated via `repl_eval_predef_view()` (R4b). No facade migration needed; just confirm `repl_animator` reads through that view, not via extern.
- **R10 (dissolve `repl_core.c`)** runs in parallel with this plan but they don't conflict. R10 moves *behavior* (parse pipeline, reformat, scope queries) to natural owners; this plan moves *state* into per-domain owners.
- **Cursor_px Option B** (kill the cache instead of routing it) is deferred to a follow-up after stage 4. With the intent/output pattern in place, the change is mechanical: drop the `cursor_px` fields from `UiPanelsOutput`, drop the controller's actualization, recompute in `ui_autocomplete_panel.c` from layout. Decided as a separate pass.
- **Frame ordering becomes load-bearing.** Once panels share data via the controller (e.g., `ui_panels_render` produces `cursor_px`, `ui_autocomplete_panel_render` consumes it), the order of view-build / render / actualize matters. The controller documents the ordering in a single comment block at the top of `imrepl_ctrl_display_frame`. Tests should construct multi-panel scenarios in fixture form rather than relying on global state to glue them together.
- **`Ui*Output` proliferation.** It would be tempting to put everything-the-renderer-might-want-to-mutate on `out`. Resist. The bar is "another consumer reads this state." If only the renderer reads it back next frame, it's a `repl_*` setter, not an `out` field.
- **Foreseeable scope creep**: per-domain modules may pull adjacent helpers along. Resist — keep each stage's diff focused on state migration. Helper relocation is R10's job.
