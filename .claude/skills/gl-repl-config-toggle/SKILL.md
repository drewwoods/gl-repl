---
name: gl-repl-config-toggle
description: Add or change a gl-repl config toggle / GlrConfigKey — the g_cfg_items[] descriptor row, section placement, keymap binding, and the example-golden regeneration a new key forces. Use when asked to add a setting, toggle, menu option, or config key, or when touching glr_actions.c / glr_config.c / glr_defaults.h.
---

# Adding a config toggle

## The one required edit

Append a `ReplConfigItem` descriptor to `g_cfg_items[]` in
`src/app/glr_actions.c`, under the right `### ` section marker. The count
auto-computes and flyout membership is automatic — do not hand-maintain either.

`### ` rows define sections. Each becomes one parent menu row plus a synthetic
trailing **All** row, with hover-opening flyouts (generic engine in
`src/ui/app/menu_bar.c`, shared with the Scene and Tutorials menus). Parent and
tag rows are **inert on click** — hover-open only, the activate branches return
0. Flyout item clicks route via `route_submenu_item_hit` → `glr_cfg_cycle_row`;
right-press cycles backward.

## Defaults

`CFG_DEFAULT_*` in `src/app/glr_defaults.h` is the single source of truth.
Reuse the macros — never duplicate the literal value at a second site.

Every example load resets non-camera presentation settings to `CFG_DEFAULT_*`
before applying the example's `@cfg`, so a new presentation-scoped key needs a
default that is sane for a bare scene.

## Tests must not assume the shipped default

Pin or derive the value under test from the `CFG_DEFAULT_*` macro. Tests that
hardcode "the current default" break every time a default is retuned.

## Golden-fixture consequence (the step people miss)

A new `GlrConfigKey` adds one `@cfg` line to **all 32 example goldens**.
Regenerate rather than hand-editing:

- `make test_X` defaults to `BUILD=release`; rebuild with `BUILD=debug` before
  regenerating fixtures.
- Regen via the stub test binary's `--dump-index` path.
- `make test_repl_core_examples` is the focused suite.

## Config bridge

`repl_cfg_get_int` / `_set_int` and friends go through the installed config
bridge only — guard `check-repl-export-via-bridge` enforces it. Export/import
must not reach around the bridge to touch config state directly.

`glr_config_set`'s tail notifies the tutorial runner; a toggle that a tutorial
step can `SET` or `REQUIRE` depends on that path staying intact.

## Keyboard binding (optional)

If the toggle earns a key: one `keymap.h` pair per action
(`#define GLR_<ACTION> <key>, <mods>`). Call sites use
`keymap_event_is(key, GLR_X)` and never spell out modifiers; `KM_KEY`/`KM_MODS`
for case labels and initializers.

```bash
make keymap-list          # current bindings + free slots
make check-keymap-no-dup  # guard
```

F2–F10 cycle bound configs forward (Shift+F = backward) — that binding lives in
the descriptor row, not in a switch.

## Verify

```bash
make test_repl_core_examples
make check-state-ownership   # includes check-palette, check-keymap-no-dup, …
make test
```
