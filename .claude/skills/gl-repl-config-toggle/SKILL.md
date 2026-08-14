---
name: gl-repl-config-toggle
description: Add or change a gl-repl config toggle / GlrConfigKey - the g_cfg_items[] descriptor row, section placement, keymap binding, and the example-golden regeneration a new key forces. Use when asked to add a setting, toggle, menu option, or config key, or when touching glr_actions.c / glr_config.c / glr_defaults.h.
---

# Adding a config toggle

## The two required edits

A new toggle is **a descriptor row plus a storage claim**. The row alone
compiles and renders a menu item whose reads return `0` - the storage switches
below are what make it a setting.

## 1. The descriptor row

Append a `GlrConfigItem` descriptor to `g_cfg_items[]` in
`src/app/glr_actions.c`, under the right `### ` section marker. The count
auto-computes and flyout membership is automatic - do not hand-maintain either.
Give every actionable row an explicit lowercase `slug` (for example,
`.slug = "auto_normals"`). Slugs are serialized identifiers and must remain
stable when a menu label changes; they are not derived from `.label`.

`### ` rows define sections. Each becomes one parent menu row plus a synthetic
trailing **All** row, with hover-opening flyouts (generic engine in
`src/ui/app/menu_bar.c`, shared with the Scene and Tutorials menus). Parent and
tag rows are **inert on click** - hover-open only, the activate branches return
0. Flyout item clicks route via `route_submenu_item_hit` → `glr_cfg_cycle_row`;
right-press cycles backward.

## 2. Storage: the two switches in `src/app/glr_config.c`

Every `GlrConfigKey` needs an arm in **both** `config_value_ptr()` (the
write-side key → `int *` ownership map) and `glr_config_get()` (the read-side
twin over the const state views). Neither switch has a `default:`, and the
build carries `-Werror=switch`, so a key with no arm is a compile error rather
than a silently-zero setting - do not "fix" that error by adding a `default:`.

- Storage that is a plain `int` field (a `GlrPresentationState` /
  `GlrRenderState` member, a peer module's `int`) returns its address from
  `config_value_ptr()` and nothing more is needed: the generic tail of
  `glr_config_set()` writes through the pointer.
- Storage that is not a plain `int *` - an enum field needing a cast, a
  lifecycle action (replay start/stop), a module-owned value (audio), or a
  cycle index that maps to something else (accum passes) - returns `NULL` from
  the pointer map and needs a matching `else if` arm in `glr_config_set()`.
  `NULL` with no set arm means the value is read-only or a pure action row.

Validation of the table itself (`glr_config_validate`, run at startup and by
`test_glr_actions`) rejects a missing/duplicate slug and a `GlrConfigKey`
claimed by two actionable rows, which `find_item_by_key()` would resolve to the
first row only.

## Is the setting per-scene or per-session?

If it belongs to a scene (it should travel in a `.glr`/example `@cfg` header
and must not leak across an F12 example switch), add it to
`k_cfg_scene_defaults[]` in `glr_actions.c` **and** to
`glr_state_presentation_reset_example_defaults()` in `glr_state.c`. That table
is the roster: `cfg_key_in_scene_subset()` is derived from it, and
`test_glr_actions.c` pins subset/defaults agreement plus the reset contract.
Session-inspection settings (profilers, depth/stencil viz, post FX) and
interface settings (code panel, syntax highlight) stay out of both.

## Defaults

`CFG_DEFAULT_*` in `src/app/glr_defaults.h` is the single source of truth.
Reuse the macros - never duplicate the literal value at a second site.

Every example load resets non-camera presentation settings to `CFG_DEFAULT_*`
before applying the example's `@cfg`, so a new presentation-scoped key needs a
default that is sane for a bare scene.

## Tests must not assume the shipped default

Pin or derive the value under test from the `CFG_DEFAULT_*` macro. Tests that
hardcode "the current default" break every time a default is retuned.

## Anything that is not a plain on/off exports a symbol, not a number

A row with `state_count > 2` (or a 2-state row whose states aren't "on" and
"off") needs a **symbol table** so `@cfg` reads
`/* @cfg auto_normals = REPL_AUTONORMAL_SMOOTH */` instead of a bare `2`. Two
edits in `src/app/glr_actions.c`:

1. A `cfg_<thing>_symbols[]` array, generated from the owning enum's
   `X_LIST(X)` macro so the names cannot drift from the enum:

   ```c
   static const char *cfg_auto_normal_symbols[REPL_AUTONORMAL_COUNT] = {
   #define AUTO_NORMAL_SYMBOL_ENTRY(name, str) [REPL_AUTONORMAL_##name] = "REPL_AUTONORMAL_" #name,
       REPL_AUTONORMAL_LIST(AUTO_NORMAL_SYMBOL_ENTRY)
   #undef AUTO_NORMAL_SYMBOL_ENTRY
   };
   ```

   Generate `.state_names` (the menu labels) from the same list macro. If the
   enum has no list macro yet, add one where the enum lives - that is the
   pattern every other multi-state key follows.

2. A `strcmp` arm in `cfg_symbol_table_for_slug()` keyed on the explicit
   **slug**: "Auto-normals" uses `auto_normals`. Old files keep loading - the
   reader falls back to an integer literal when the text matches no symbol.

Use `make config-list` to inspect the authoritative tab-separated slug table,
and `make check-config-slugs` to verify scene/example `@cfg` headers against
it.

## Golden-fixture consequence (the step people miss)

A new `GlrConfigKey` adds one `@cfg` line to **all 32 example goldens**, and
adding a symbol table to an existing key rewrites that key's line in every one
of them. Regenerate rather than hand-editing:

- `make rebuild-golden BUILD=debug` rewrites all of them (wraps the stub test
  binary's `--update-golden`); `--dump-index N` dumps a single example.
- `make test_X` defaults to `BUILD=release`; rebuild with `BUILD=debug` before
  regenerating fixtures.
- `make test_repl_core_examples` is the focused suite.

## Config bridge

`repl_cfg_get_int` / `_set_int` and friends go through the installed config
bridge only - guard `check-repl-export-via-bridge` enforces it. Export/import
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

F2-F10 cycle bound configs forward (Shift+F = backward) - that binding lives in
the descriptor row, not in a switch.

## Verify

```bash
make test_repl_core_examples
make check-state-ownership   # includes check-palette, check-keymap-no-dup, …
make test
```
