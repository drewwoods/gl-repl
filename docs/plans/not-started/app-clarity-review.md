# `src/app` Clarity, Consistency & Maintainability Review

## Status - NOT STARTED (2026-08-13)

A read-only review of `src/app` (33 `.c` + 34 `.h`, ~24,200 lines: 22,450 in the
frame-time controller band, 1,722 in `boot/`) against the same four questions
the `src/repl` review asked: are responsibilities clear from names and
organization, is anything duplicated or over-complicated, will the interfaces
absorb the next feature, and are the conventions internally consistent.

No code was changed. Every finding carries a file:line citation so it can be
re-checked before anyone acts on it.

## Verdict first

**`src/app` is in good shape, and the one structural problem it has is one the
module already names in its own README.** The layering is real and enforced,
the boot/controller split is documented and guarded, and the header comments
are consistently better than the code they describe. This review deliberately
did *not* manufacture findings for the following, all examined and found sound:

- **The two-band split** (`boot/` vs frame-time controller) and its one-way
  `check-app-boot-band` guard. `boot/glr_boot_dumps.c` consuming `GlrCliOptions`
  while the formatters stay in controller-band `glr_debug.c` is exactly the
  shape the rule should force, and both headers argue their own placement.
- **`glr_ctrl_router.c`'s dispatch sandwich.** Every GLUT entry point is a thin
  shim around a `*_dispatch` body, with the shape written down once at
  `glr_ctrl_router.c:2548-2575` and followed by all five event families. The
  physical-vs-scripted split (tour arbitration on top, shared dispatch beneath)
  is a genuinely clean solution to a fiddly problem.
- **The `glr_frame_*` vs `glr_ctrl_*` naming break.** `glr_ctrl.h:235-263`
  explains why the frame boundary gets the bare `glr_` prefix - the application
  owns the frame, the controller is one stage inside it - and cites the ~10 ms
  tour caption that a controller-scoped bracket hid. That is a deliberate,
  argued exception, not drift.
- **`g_presentation_rgba`'s "one variable, two vintages"** (`glr_ctrl.c:1036-1058`).
  Read ordering as the mechanism instead of a second stored copy, with the
  load-bearing read positions named and pinned by a test.
- **`glr_camera.h` and `glr_tour_snapshot.h`** are the two best headers in the
  module. `glr_camera.h` documents every non-obvious call-ordering constraint
  (`glr_camera_destination` vs `glr_camera()`, `set_target_decay` after
  `ease_to`, `settle_target` vs `controls_reset`); `glr_tour_snapshot.h` states
  what the baseline deliberately excludes and why.
- **The depth-snapshot lifecycle** (`glr_ctrl.c:1327-1485`). Every early return
  invalidates rather than leaving pixels stale, the read is checked per capture
  rather than trusting the startup probe, and freshness is keyed on the undo
  generation so ordinary document replacement is covered without enumerating
  paths. The call-order contract is asserted by a dedicated guard
  (`check-depth-capture-after-finish`).

What follows is the residue: ten places where a convention is spelled
inconsistently, a fact is stated twice without a tie, or the next feature will
cost more than it should.

---

## Findings

Ranked by (cost of leaving it) × (cheapness of fixing it). The first two are
worth doing on their own schedule; 3-6 are worth doing when adjacent code is
already open; 7-10 are cleanups flagged for the record.

---

### 1. A `GlrConfigKey` lives in five places, two of the dispatches are non-exhaustive, and the skill says there is one required edit

**Priority: High.**

**What.** Adding a config key touches, at minimum:

| Site | File:line | What happens if you miss it |
|---|---|---|
| the `GlrConfigKey` enum | `glr_config.h:31-82` | won't compile |
| the `g_cfg_items[]` descriptor row | `glr_actions.c:294-438` | no menu row, no slug, no shortcut |
| the write map `config_value_ptr()` | `glr_config.c:232-287` | **`glr_config_set` silently returns** |
| the read map `glr_config_get()` | `glr_config.c:292-347` | **reads 0 forever** |
| `glr_config_set`'s if-ladder | `glr_config.c:391-478` | needed instead of the write map for enum-typed, lifecycle, or derived values |
| `glr_cfg_cycle_row`'s pre/post special cases | `glr_actions.c:1088-1247` | no status line, no side effects |

Three of those dispatch on the same enum in three different shapes: a `switch`
returning `int *`, a `switch` returning `int`, and an `if / else if` chain of
~15 arms. Both switches end in `default:`, so a new key compiles cleanly.

The read map's own comment (`glr_config.c:289-291`) says *"Read-side twin of
config_value_ptr … Keep the two switches' arms in sync when adding a key."*
They are **not** twins and cannot be kept in sync literally: four keys
(`WIREFRAME`, `XFORM_GUIDE_MODE`, `ORTHO_MODE`, `PROJECTION`) are deliberately
`NULL` in the write map and fully present in the read map, because their
backing fields are enum-typed. So the instruction is either wrong or means
something it does not say.

Meanwhile `.claude/skills/gl-repl-config-toggle/SKILL.md` opens with a section
headed **"The one required edit"**, naming only the descriptor row.
`glr_config.h:16-18` names two ("extend `GlrConfigKey` and add the matching
`GlrConfigItem` row"). Neither mentions `glr_config.c` at all.

**Why it matters.** The failure mode is silent and complete: the Config menu
renders the new row with its label and state names (both come from the
descriptor table), clicking it cycles nothing, `@cfg` round-trips a value that
never reaches storage, and the example goldens pick up a line that is always
the default. Nothing warns. `tests/test_glr_actions.c:564` and `:1405` sweep
every key, but both assert *"value unchanged"*, which a permanently-zero key
passes.

This is the module's most-used extension point - 47 keys and counting - and its
documentation understates the work by a factor of three.

**What to change.** Two independent steps, either useful alone:

1. **Drop `default:` from both switches in `glr_config.c` and enumerate every
   key.** `-Werror=switch` then makes it impossible to add a `GlrConfigKey`
   without consciously answering "where does this live?" twice. This is the
   idiom the project already documents (Makefile: *"dropping one is how a fold
   declares 'this list is exhaustive on purpose'"*) and the same fix the
   `src/repl` review recommends for `attrib_bits.c`. `config_value_ptr` already
   spells `case GLR_CONFIG_NONE: case GLR_CONFIG_COUNT:` explicitly, so it is
   one `default:` line away.
2. **Correct the skill and `glr_config.h:16-18`** to list all the required
   edits, and replace the false "twin" comment with what the two switches
   actually are: a write map that returns `NULL` for anything `glr_config_set`
   handles by hand, and a read map that must be total.

A larger refactor - collapsing the three dispatches into one per-key descriptor
carrying a getter/setter pair - is possible but probably not justified: the
setters genuinely differ in kind (plain store, enum cast, lifecycle call,
derived recompute), and a function-pointer table would trade the compiler's
exhaustiveness check for a runtime `NULL`.

---

### 2. Five functions in `src/app` are larger than the two the project formally ratchets as god-functions, and nothing holds the line

**Priority: High.**

**What.** `scripts/baselines/tier-c-function-size.txt` ratchets exactly two
functions, both in `src/repl`, with the contract stated as *"a commit can shrink
either function but never grow it past the baseline"*:

```
parse_command:  289
flatten_range:   89
```

Measured against that bar, `src/app` currently holds:

| Function | File:line | Lines |
|---|---|---|
| `glr_ctrl_display_frame` | `glr_ctrl.c:2935` | **433** |
| `glr_ctrl_init_gl` | `glr_ctrl.c:4191` | **286** |
| `glr_ctrl_build_ui_snapshot` | `glr_ctrl.c:2246` | **284** |
| `glr_action_menu_item_activate` | `glr_actions.c:1585` | **277** |
| `glr_ctrl_build_scene_config` | `glr_ctrl.c:1611` | **251** |

And at file scale, `glr_ctrl.c` is 4,886 lines - 1.6× `glr_ctrl_router.c`, the
next largest, and larger than `src/repl`'s `compile.c` (3,781), which that
module's review explicitly declined to split.

`src/app/README.md:19-25` already names this:

> Its hub, `glr_ctrl.c`, is meant to be a router and frame/snapshot
> coordinator… **The current file is broader than that target.** It still
> carries too much mixed policy for routing, frame order, snapshot assembly,
> timers, and transitional glue. **That bloat is a known design pressure, not a
> license to add new feature behavior there by default.**

**Why it matters.** The project has the diagnosis, the stated intent, *and* the
enforcement idiom - and has applied the idiom to the other module and not this
one. "Known design pressure, not a license" is a social contract with no
mechanism behind it, and the measurements above are what a decade of that
contract looks like. The concrete cost is already visible in this review:
findings 5, 6 and 8 are all things that accreted into `glr_ctrl.c` because
there was no friction against adding one more block.

`glr_ctrl_display_frame` is the specific worry. It is a linear script - fifteen
`prof_begin`/`prof_end` stages, each with load-bearing ordering explained in a
comment - and the ordering constraints are stated per-stage rather than in one
place, so a reader must hold 433 lines to know whether a new stage may move.

**What to change.** Add `glr_ctrl.c`'s five functions (or just
`glr_ctrl_display_frame`) to the existing ratchet, at their current sizes. That
is a ~15-line addition to `check-tier-c-function-size.sh` plus baseline rows,
and it converts the README's prose into the same "don't make it worse" contract
`src/repl` already gets. It requires no refactor and blocks nothing.

The natural first extraction, if anyone wants one: `glr_ctrl_display_frame`'s
panel-rendering tail (`glr_ctrl.c:3228-3342`, from `PROF_CODE_PANEL` through
`PROF_COMPOSITOR`) is ~115 lines that consume only `ui_snap` and `cp_out` and
have no ordering relationship with anything above `render3d_draw_scene`. Lifting
it to `glr_ctrl_render_overlay_panels(const UiRenderSnapshot *, UiCodePanelOutput)`
is mechanical. That is a suggestion, not a prerequisite - the ratchet is the
finding.

---

### 3. The "scene subset" roster is spelled four times across three files, and the comment claiming a test pins it is wrong

**Priority: Medium.**

**What.** Which settings belong to a scene (and therefore reset on every example
load and serialize into a scene's `@cfg`) is written down four separate times:

| Form | Site | Entries |
|---|---|---|
| `switch` over `GlrConfigKey` | `glr_actions.c:460-487` | 22 |
| `{key, CFG_DEFAULT_*}` table | `glr_actions.c:505-528` | 22 |
| field assignments on `GlrPresentationState` | `glr_state.c:145-179` | 20 |
| two non-presentation resets | `glr_ctrl.c:3427-3429` | 2 (`camera_rotate`, `variable_panel`) |

The third list is 20 rather than 22 because `CAMERA_ROTATE` and
`VARIABLE_PANEL` are not `GlrPresentationState` fields - they live on the
camera and the variable-panel peer, so `glr_ctrl_reset_example_chrome` resets
them separately. That is correct, but it means the roster is genuinely split
across two modules with the split undocumented at three of the four sites.

`glr_state.h:136-142` says the third list *"Mirrors the cfg-bridge
`fill_scene_subset` whitelist in glr_actions.c"* - true, but a reader following
that pointer lands on `glr_export_cfg_fill_scene_subset` (`glr_actions.c:889`),
which is a loop over the descriptor table, not a list; the actual roster is one
level down in `cfg_key_in_scene_subset`.

`glr_actions.c:496-500` states:

> Keep it complete against `cfg_key_in_scene_subset()`: a missing row makes the
> .glr writer emit that slug unconditionally rather than only when it differs…
> **`test_glr_actions.c` pins the coverage.**

It does not. `tests/test_glr_actions.c:2044-2082` is the only test touching
either bridge callback, and its seven assertions check *symbolic encoding*
(`"grid" == "GRID_THEME_RADAR"`), not roster completeness. Nothing anywhere
calls `fill_scene_defaults`.

**Why it matters.** Two costs, in increasing order. The stated one is degraded
`.glr` output - a slug written unconditionally instead of only on difference.
The unstated and worse one is the third list: a key added to
`cfg_key_in_scene_subset` but not to `glr_state_presentation_reset_example_defaults`
serializes per-scene while never resetting, so it **leaks across F12 example
switches** - the exact failure the two comments at `glr_state.c:146-154` and
`:175-178` were written to prevent for `ortho_mode` and `projection_mode`
individually.

And a false "a test pins this" comment is worse than no comment: it is the
reason someone doesn't write the test.

**What to change.** Cheapest credible fix, and it closes the whole class: make
`k_cfg_scene_defaults[]` the single roster and derive
`cfg_key_in_scene_subset()` from it (`return cfg_scene_default_for_key(key, NULL);`).
That collapses two of the four lists into one by construction, for a ~4-line
diff. Then either write the test the comment claims exists - walk
`k_cfg_scene_defaults[]`, assert `glr_config_get(key)` equals the row's value
after `glr_ctrl_reset_example_chrome(-1)`, which covers list three as a
side effect - or delete the claim.

---

### 4. The host/bridge installer idiom has three naming shapes and two placement policies, with no stated rule

**Priority: Medium.**

**What.** Eleven bridges/hosts are installed from one place,
`glr_ctrl_install_app_services()` (`glr_ctrl.c:3996-4023`). This is the module's
main extension seam - seven of the eleven exist specifically so a subsystem
stays linkable in its standalone demo. The idiom is spelled three ways:

| Installer name | Static instance | Lives in |
|---|---|---|
| `glr_color_picker_install_host()` | `g_glr_cp_host` | own TU |
| `glr_assign_plot_install_host()` | `g_glr_ap_host` | own TU |
| `glr_camera_export_install_bridge()` | `g_glr_export_camera_bridge` | own TU |
| `glr_actions_install_export_cfg_bridge()` | `g_glr_export_cfg_bridge` | `glr_actions.c` |
| `glr_clipboard_install()` | `g_bridge` | own TU |
| *(none - inline)* | `g_glr_host_effects` | `glr_ctrl.c:3894` |
| *(none - inline)* | `g_export_projection_bridge_impl` | `glr_ctrl.c:3512` |
| *(none - inline)* | `g_export_light_bridge_impl` | `glr_ctrl.c:3533` |
| *(none - inline)* | `g_glr_var_value_source` | `glr_ctrl.c:3992` |
| *(none - inline)* | `g_glr_help_fkey_provider` | `glr_ctrl.c:3977` |

Three verb positions (`<module>_install_host`, `<module>_install_bridge`,
`<module>_install`), three prefix conventions for the static
(`g_glr_*`, `g_*`, `g_*_impl`), and no rule for whether a bridge earns its own
translation unit. `glr_clipboard.c:243`'s `g_bridge` is the only file-private
static in `src/app` with no module prefix at all.

**Why it matters.** This is the seam a contributor reaches for whenever a
subsystem needs something from the app without depending on it, and there is
nothing to copy from - five of the eleven live inline in the largest file in the
module, five have dedicated TUs, and the naming gives no signal about which is
which. The predictable outcome is that the next one lands inline in
`glr_ctrl.c` (the path of least resistance), which is finding 2's mechanism.

**What to change.** Write the rule down, in `src/app/README.md` next to the
existing boot/controller Membership rule, and make it one sentence: *a bridge
whose implementation needs more than a handful of adapter functions gets its own
`glr_<subsystem>_bridge.c`; anything smaller stays inline in `glr_ctrl.c`;
installers are named `glr_<module>_install_<what>` and their static is
`g_glr_<module>_<what>`.* Then rename `g_bridge` → `g_glr_clipboard_bridge`
(one file, two lines) so nothing contradicts it. Renaming the existing
installers is optional and probably not worth the churn; the rule matters more
than retroactive uniformity.

---

### 5. `GlrModalKind` is dispatched in two files, both with a silent `default:`, and the presentation half is four copies of one format string

**Priority: Medium.**

**What.** Each modal kind needs two things written in two modules:

- its **commit behavior** - `glr_action_modal_commit` (`glr_actions.c:1493-1580`),
  a `switch` ending in `default: return 0;`
- its **prompt text** - inline in `glr_ctrl_build_ui_snapshot`
  (`glr_ctrl.c:2365-2397`), a `switch` ending in `default: break;`

Four of the five prompt arms are the same format string with two words swapped:

```c
"New workspace: %s_   %s%s[Enter] create   [Esc] cancel"
"Save workspace as: %s_   %s%s[Enter] save   [Esc] cancel"
"Open workspace: %s_   %s%s[Enter] open   [Esc] cancel"
"Save scene as: %s_   %s%s[Enter] save   [Esc] cancel"
```

with the identical `text, error[0] ? error : "", error[0] ? "   " : ""` argument
triple repeated four times.

**Why it matters.** A new modal kind that reaches `glr_ctrl.c`'s `default:`
opens a prompt with an **empty message** - the modal is active and swallowing
every keystroke, and the UI says nothing about what it wants. That is a worse
failure than a compile error and there is nothing to catch it, because a
whole-UI-snapshot builder is the last place anyone looks for modal text.

Separately, prompt strings sitting inside a 284-line snapshot builder is a
placement nobody would choose deliberately - `glr_modal.c` already owns the
kind, the text buffer and the error buffer.

**What to change.** Move the message formatting to `glr_modal.c` behind
`glr_modal_prompt(char *out, size_t out_sz)`, backed by a small table:

```c
static const struct { GlrModalKind kind; const char *noun, *verb; }
    k_modal_prompts[] = {
        { GLR_MODAL_WORKSPACE_NEW,       "New workspace",     "create" },
        { GLR_MODAL_WORKSPACE_SAVE_AS,   "Save workspace as", "save"   },
        …
    };
```

with the confirm-delete kind keeping its own arm. `glr_ctrl_build_ui_snapshot`
then becomes two lines, the format string exists once, and a kind missing from
the table is one grep from its own definition rather than one file away.
Dropping `default:` from `glr_action_modal_commit` while there closes the other
half.

---

### 6. Comment/declaration drift: two orphaned doc comments, one duplicated pair, one misplaced comment, one stale section banner

**Priority: Medium** (individually trivial; collectively they are the module's
main readability tax, because the comments are otherwise so good that readers
trust them).

**What.** Five concrete instances, all consistent with a past migration of doc
comments from `.c` files into headers:

1. **`glr_ctrl.h:36-45`** - the doc comment for `glr_ctrl_open_color_picker`
   sits above the doc comment for `glr_ctrl_set_code_panel_scroll`, which sits
   above `set_code_panel_scroll`'s declaration; `open_color_picker`'s own
   declaration is two lines further down. A reader attaches the first comment to
   the wrong function.
2. **`glr_ctrl.h:139-148`** - `glr_ctrl_build_gl_state_panel_view` carries two
   stacked comment blocks, both opening *"Per-frame view for the … OpenGL-state
   popup"*. The second supersedes the first; the first was never deleted.
3. **`glr_ctrl.c:965-970`** - the comment above `glr_ctrl_restore_hidden_code_panel`
   begins mid-sentence: `/* The action\n * writes glr_state_presentation_mut(), …`.
   Its opening clause is now in `glr_ctrl.h:108-114`.
4. **`glr_ctrl.c:981-986`** - same shape above `glr_ctrl_reset_transients`:
   `/* The body reaches\n * the camera, menu bar, …`.
5. **`glr_ctrl.c:3380-3383`** - `/* Idempotent app-service installer required
   for any REPL loading/export path (including CLI). */` sits immediately above
   `static const GlrExampleTagDefault k_example_tag_defaults[]`. It describes
   `glr_ctrl_install_app_services()`, 600 lines away at `:3996`.
6. **`glr_actions.h:110-117`** - the comment for `glr_actions_apply_defaults()`
   is separated from its declaration by two `#define`s and an unrelated
   function declaration.
7. **`glr_ctrl.c:4710-4728`** - a 19-line banner headed *"Router helpers:
   non-editor input concerns"* describing the `glr_ctrl_router_*` family. No
   router helper has been defined in this file since the carve-out to
   `glr_ctrl_router.c`; `glr_ctrl.c` contains exactly two mentions of
   `glr_ctrl_router_`, both call sites. The banner now sits immediately above
   `glr_ctrl_tick()`, which it does not describe. The same text already lives
   correctly at `glr_ctrl.h:323-337`.

**Why it matters.** In a module where nearly every non-obvious decision carries
a paragraph explaining it, comments are load-bearing navigation. Item 7 is the
costly one: a reader scrolling `glr_ctrl.c` for the input router finds a
detailed section header for code that is not there, which is worse than finding
nothing. Items 3-5 make the reader wonder what got deleted.

**What to change.** Delete items 2, 5 and 7 (all fully redundant with a correct
copy elsewhere); move items 1 and 6 next to the declarations they document;
restore the missing opening clause on items 3 and 4 or delete them in favour of
the header text. Fifteen minutes, no behavior change, no test impact.

---

### 7. `splash` is the only module in `src/app` that does not carry the `glr_` prefix

**Priority: Low.**

**What.** `src/app/boot/splash.{c,h}` exports `splash_active()`,
`splash_skip()`, `splash_render()`. Every other file in both bands - including
all five of its `boot/` siblings (`glr_cli`, `glr_boot_dumps`, `glr_init_trace`,
`glr_capture_env`, `glr_frame_pacer`) - is `glr_*.{c,h}` with `glr_*` symbols.
The file's own include guard is `GLR_SPLASH_H` (`splash.h:1`), so the intent was
clearly `glr_splash_*`.

CLAUDE.md's Conventions section states the rule (*"Prefixes express ownership:
… `glr_*` (app shell/controller/services)"*), and `check-module-prefixes.sh` is
a **denylist of specific removed names**, not a blanket sweep - so this passes.
`docs/MODULES.md:756` renders the anomaly visibly: *"glr_cli · glr_boot_dumps ·
glr_init_trace · glr_capture_env · glr_frame_pacer · splash"*.

**Why it matters.** Low. It is one module, and the naming guard's denylist
design (documented and correct) means it will never be caught automatically.
The cost is that the convention now has an undocumented exception, which is how
conventions stop being load-bearing.

**What to change.** Rename to `glr_splash.{c,h}` / `glr_splash_*`. Five call
sites: `gl_repl.c:103,105,181,356,372`, plus `Makefile:1086-1091`
(`test_splash_OBJS`) and `docs/ARCHITECTURE.md:150,185`. Alternatively, if the
short name is wanted for the minimal test link set, document it as a sanctioned
exception in `docs/MODULES.md` - either resolution is fine; the current state
(silent exception) is the problem.

---

### 8. The three capture-affordance `glr_ctrl_open_*` entry points are the same six lines three times, and the code says so

**Priority: Low.**

**What.** `glr_ctrl_open_gl_state_popup` (`glr_ctrl.c:4589`),
`glr_ctrl_open_assign_plot` (`:4614`) and `glr_ctrl_open_command_description`
(`:4637`) each run the identical sequence:

```c
glr_ctrl_build_ui_snapshot(&snap);
if (!ui_repl_code_panel_source_line_point(&snap, line, &x, &y)) return 0;
glr_ctrl_scripted_passive_motion(x, y);
glr_ctrl_scripted_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
glr_ctrl_scripted_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
```

and differ only in the predicate that checks which popup came up. The comments
acknowledge it: `:4622` says *"Same shape as glr_ctrl_open_gl_state_popup"* and
`:4645` says *"Third of the same shape"*.

**Why it matters.** Genuinely low - the duplication is honest, documented, and
the routing behavior it exercises is the whole point (a capture must take the
same path a user's click does). The only real cost is that a fourth right-click
popup adds a fourth copy, and the shared half is where the subtle part lives:
returning 0 for an off-screen row so the capture hook retries next frame.

**What to change.** Extract
`static int glr_ctrl_right_click_source_line(int line)` returning 0 when the row
is not on screen, and let each entry point call it and then check its own
predicate. Three functions drop to ~6 lines each and the retry contract is
stated once. Worth doing the next time this file is open for another reason;
not worth a dedicated change.

---

### 9. `glr_audio.c` holds two complete backends behind a header comment that describes one

**Priority: Low.**

**What.** `glr_audio.c` is 2,463 lines split at `:332` / `:1036` into an
Emscripten Web Audio backend (~700 lines, including seven `EM_JS` blocks of
100-230 lines each) and a native miniaudio backend (~1,400 lines with a
worker-thread model). Both implement the same `glr_audio.h` surface.

Neither entry point mentions the split. `glr_audio.h:4-36` describes the module
as a *"Thin wrapper over miniaudio"*; the web build contains no miniaudio at
all. `glr_audio.c:1-51`'s 50-line header comment is entirely about miniaudio,
`MINIAUDIO_IMPLEMENTATION` ownership, and the worker-thread rationale - a reader
looking for the browser autoplay/manifest behavior gets no signal that it is in
this file, 300 lines down.

There are also two `static void reset_audio_module_state(void)` definitions
(`:564` and `:1136`), correctly mutually exclusive but confusing to a grep.

**Why it matters.** Low, and lower than the line count suggests: the linker
catches a backend that fails to define a symbol, and the two halves genuinely
share the playlist/path preamble at `:81-330`. The cost is discoverability -
"which file owns web audio" has no answer that either header gives.

**What to change.** Smallest useful fix: two paragraphs. Say in `glr_audio.h`
that the module has two backends and which one each build gets, and in
`glr_audio.c` mark the two regions with banners naming the line ranges. A split
into `glr_audio_web.c` / `glr_audio_miniaudio.c` over a shared
`glr_audio_common.c` is the larger version and is defensible - it would make the
shared surface explicit - but it is a real refactor of working, well-tested code
with no current bug pressure behind it. Not recommended now.

---

### 10. `glr_tour_snapshot` handles the tutorial slice differently from the twelve others

**Priority: Low.**

**What.** `glr_tour_snapshot.c` composes thirteen by-value owner slices. Twelve
go through a symmetric `_capture(&s->x)` / `_restore(&s->x)` pair. The
thirteenth reaches for the accessors directly:

```c
s->tutorial = tutorial_state_view();       /* :73 */
*tutorial_state_mut() = s->tutorial;       /* :109 */
```

**Why it matters.** Low. It works, and the tutorial state is a plain value
struct. But it is the one write-through-a-`_mut()`-accessor in a module that
otherwise routes everything through named capture/restore APIs, in a tree that
maintains a `check-mut-accessor-count` ratchet and a
`check-no-write-through-view` guard for exactly this shape.

Related, and also low: nothing ties `struct GlrTourSnapshot`'s member list to
the two ordered call lists in `capture` and `restore`. A fourteenth owner added
to the struct and to `capture` but not `restore` compiles and silently fails to
rewind - the same class as the `ReplCheckpointState` finding in the `src/repl`
review, and with the same available answers (a nested struct, or a round-trip
test that mutates every slice and asserts restore). `tests/test_glr_tour_snapshot.c`
exists and should be where that goes.

**What to change.** Add `tutorial_state_capture` / `tutorial_state_restore` to
`subsystems/tutorial/tutorial_state.h` (two trivial functions) so all thirteen
slices read alike. Consider the round-trip sweep the next time an owner is added.

---

## Patterns worth copying

Called out because other `src/app` code should follow them, and because two of
them are better than anything the `src/repl` review found:

- **`glr_camera.h`'s constraint documentation.** Every accessor that can be
  called at the wrong moment says so and names the wrong moment
  (`glr_camera_destination` vs `glr_camera()` during a view-mode transition;
  `set_target_decay` only after `ease_to`; `settle_target` vs the cancel that
  `controls_reset` performs). This is the model for any stateful app service.
- **`glr_ctrl.h:235-263`'s frame-boundary rationale.** A naming exception that
  argues for itself, names the bug that motivated it, and states the unpaired-call
  contract. Exceptions documented like this stop being drift.
- **The `boot/` band rule + its guard.** A one-sentence membership rule
  (`src/app/README.md:108-110`), a one-sentence direction rule, and
  `check-app-boot-band` enforcing the arrow. `glr_boot_dumps` vs `glr_debug`
  is the worked example of the rule forcing a split, and both headers explain
  their own side of it.
- **`glr_config.h`'s `GLR_ACCUM_PASS_LADDER` X-macro** (`:112-134`). Three
  consumers - menu labels, the int step table, the `GLR_ACCUM_PASSES` startup
  validator - derived from one list, with the header naming all three and
  explaining why `render3d` deliberately is not a fourth. This is the right
  answer to the "same list in several forms" problem, and finding 3 is a place
  the same technique would apply.
- **`glr_ctrl_router.c`'s dispatch-shape banner** (`:2548-2575`). The shim/
  dispatch/effect-flush contract written out as pseudocode once, then followed
  by five event families without restating it. Compare finding 8, where the
  shared shape is copied three times instead.

---

## Overall assessment

**Significant cleanup is not warranted.** `src/app` is a coherent module with a
real, enforced layering, documentation that is unusually honest about its own
weak points, and a comment density that makes the non-obvious decisions
recoverable. Most of what looks wrong at first read turns out to be argued
somewhere.

The two things worth acting on are cheap and mostly non-code:

1. **Finding 1** - drop two `default:` arms and correct the config-toggle skill.
   This is the module's busiest extension point and its documentation currently
   understates the work threefold, with a silent failure mode at the end.
2. **Finding 2** - extend the existing size ratchet to `glr_ctrl.c`. The README
   has already made the argument; this just gives it teeth, and it is the only
   finding here that addresses a trend rather than a state.

Findings 3-6 are worth folding into whatever change next opens those files.
Findings 7-10 are cleanups; none of them is currently costing anything.

The one thing this review would push back on is the temptation to decompose
`glr_ctrl.c` as a project. It is large, and the README is right that it is
larger than its target - but every one of its five big functions is a coherent
linear sequence with load-bearing ordering, not a grab-bag, and splitting a
frame pipeline across files trades one long readable script for a call graph
that hides the ordering constraints the comments currently make visible. The
ratchet is the right intervention: it stops the growth without paying for a
refactor nobody has a bug to justify.

---

## Recommended sequencing

1. **Finding 6** - the comment deletions/moves. Fifteen minutes, zero risk,
   and two of them are actively misdirecting readers today.
2. **Finding 1** - the two `default:` arms plus the skill and header
   corrections. Do this before the next config key lands.
3. **Finding 2** - add the ratchet rows. Independent of any refactor.
4. **Finding 3** - derive `cfg_key_in_scene_subset()` from
   `k_cfg_scene_defaults[]`, and either write the claimed test or delete the
   claim.
5. **Finding 4** - write the bridge-installer rule into `src/app/README.md`;
   rename `g_bridge`.
6. **Finding 5** - move the modal prompt strings to `glr_modal.c`.
7. **Findings 7, 8, 10** - when adjacent code is next open.
8. **Finding 9** - documentation only; the split is not recommended.

## Explicitly not recommended

- **Do not split `glr_ctrl.c` on size alone.** See the assessment above. The
  router carve-out already took the half that had a clean seam; what remains is
  one frame pipeline plus the app-service composition root, and both are
  supposed to know about everything - that is what a composition root is.
- **Do not replace the config dispatches with a function-pointer table.** The
  per-key setters differ in kind, not just in target, and a table would trade
  the compiler's exhaustiveness check (finding 1's actual fix) for a runtime
  `NULL`.
- **Do not split `glr_audio.c` right now.** Two backends behind one header is a
  normal shape; the defect is that neither header says so. Fix the comments.
- **Do not make `check-module-prefixes.sh` a blanket sweep** to catch finding 7.
  Its denylist design is deliberate and documented, and borrowed cross-module
  API types are correct C. Rename the one module instead.
