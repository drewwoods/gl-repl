# `src/app` Clarity, Consistency & Maintainability Review

## Status — RECONCILED INDEPENDENT ASSESSMENT (2026-08-14)

This is a read-only review of `src/app` (33 `.c` and 34 `.h` files, about
24,300 lines) against the current ownership documentation, tests, and guards.
It independently checked the prior review rather than accepting its findings
as a starting assumption.

No application code was changed. Line citations refer to the live tree on
2026-08-14.

---

## Verdict first

`src/app` is clear for a composition root of this size. The boot / frame-time
split is real, the controller remains above the editor / REPL / UI / render3d
layers, and the large controller functions mostly express necessary ordering
rather than mixed feature ownership.

**A general cleanup project is not warranted.** Two extension contracts merit
targeted work:

1. make `GlrConfigKey` read/write maps genuinely compiler-exhaustive and fix
   the config-toggle instructions;
2. make the scene-local config roster single-source and test its reset contract.

The modal enum can receive a small exhaustiveness guard when that module is
next touched. The remaining issues are factual comment/API cleanup or
opportunistic deduplication. There is no evidence supporting a controller
rewrite, a file-size campaign, or a naming campaign.

---

## Reconciliation with the previous review

| Previous recommendation | Independent assessment |
|---|---|
| Non-exhaustive `GlrConfigKey` maps are High | **Agree.** This is the one extension path where a missed edit can look wired in the UI yet silently read `0` or reject the write. The comment promising a compiler diagnostic is currently false. |
| Scene-local config is duplicated | **Agree, but narrow the claim.** There are two identical 22-key rosters and a corresponding 20-field presentation reset plus two peer-state writes. Current behavior aligns; the missing guard and contradictory comments are the problem. |
| Move modal prompt formatting into `glr_modal.c` | **Disagree.** Input state, view text, and commit side effects are intentionally separated. Moving presentation formatting into the input-only module weakens that boundary. Add enum exhaustiveness without moving responsibilities. |
| Extract the overlay tail from `glr_ctrl_display_frame` and then ratchet its size | **Disagree.** The function is a readable, profiled, load-bearing sequence. An extraction would mostly pass a large snapshot and hide draw order. Reconsider only when a concrete independently owned stage appears. |
| `glr_camera_export.h` has an orphaned declaration | **Disagree on “orphaned.”** The function is live and canonically declared in `glr_ctrl.h`; the export header contains a wrong-layer duplicate. Remove only that duplicate declaration. |
| Rename `boot/splash` to gain the `glr_` prefix | **Disagree.** This is style-only churn. The module's purpose and boot ownership are unambiguous. |
| Rename Host/Bridge files or standardize all app prefixes | **Disagree.** Host and Bridge have meaningful distinct roles. A couple of historical filenames do not justify public/file renames. Document the vocabulary if a new seam is added. |
| Add tutorial capture/restore helpers for symmetry | **Disagree for now.** Direct struct capture/restore is correct for the current plain state type. Adding an API for a hypothetical future invariant is speculative. |
| Split the dual-backend audio implementation | **Disagree.** The compile-time split is obvious and the playlist model is shared. Correct the backend comments; do not split the file. |
| Extract a helper for three synthetic right-click capture hooks | **Agree as Low/opportunistic.** It is real duplication but not a reason to open the controller by itself. |

---

## Findings

### 1. `GlrConfigKey` extension is silently non-exhaustive, while its docs claim otherwise

**Priority: High**

**Where**

- [`src/app/glr_config.c:224`](../../../src/app/glr_config.c) —
  `config_value_ptr`
- [`src/app/glr_config.c:289`](../../../src/app/glr_config.c) —
  `glr_config_get`
- [`src/app/glr_config.c:391`](../../../src/app/glr_config.c) —
  `glr_config_set`
- [`src/app/glr_config.c:180`](../../../src/app/glr_config.c) —
  `glr_config_validate`
- [`src/app/glr_config.h:16`](../../../src/app/glr_config.h)
- [`src/app/README.md:139`](../../../src/app/README.md)
- [`.claude/skills/gl-repl-config-toggle/SKILL.md:8`](../../../.claude/skills/gl-repl-config-toggle/SKILL.md)
- the `AGENTS.md` config-toggle one-liner

**What is unclear or inconsistent**

The comment above `config_value_ptr` says the compiler flags a new
`GlrConfigKey` that does not claim a storage slot. It does not: both
`config_value_ptr` and `glr_config_get` end in `default:`, suppressing the
missing-enum warning that the comment describes.

A persistable config item currently may require all of these related edits:

1. `GlrConfigKey`;
2. a `g_cfg_items[]` descriptor;
3. `config_value_ptr`;
4. `glr_config_get`;
5. a `glr_config_set` special case when storage is not a plain `int *`;
6. `CFG_DEFAULT_*` / `GLR_STATE_DEFAULTS_INITIALIZER` when it owns storage;
7. the scene-default table when scene-local;
8. symbol metadata and regenerated goldens for a named multi-state value.

The module README calls the descriptor types `ReplConfigKey` /
`ReplConfigItem`; the live types are `GlrConfigKey` / `GlrConfigItem`.
`ReplConfigItem` is instead the serialized `{slug, value}` bag row in
`src/repl/cfg_baseline.h`. The skill and `AGENTS.md` also describe the table
row as if it were the only required edit.

`glr_config_validate` validates key range and slug uniqueness, but not duplicate
actionable keys. A duplicated key would make `find_item_by_key` silently select
the first descriptor.

**Practical cost**

A new menu row can compile and render while reads return `0`, generic writes
no-op, or the wrong descriptor supplies state names. This is exactly the kind
of extension failure that is easy to overlook because the visible wiring
exists.

**Smallest reasonable improvement**

1. Remove `default:` from `config_value_ptr` and `glr_config_get`.
2. Add explicit `GLR_CONFIG_NONE` and `GLR_CONFIG_COUNT` arms, and move the
   `AUDIO_MODE` / `ACCUM_PASSES` read cases into the read switch.
3. Make `glr_config_validate` reject duplicate actionable `GlrConfigKey`
   values while retaining the intentional absence of Audio from the visible
   Config menu.
4. Correct the README, skill, and `AGENTS.md` type names and list the conditional
   extension sites above.

Do not replace the switches with a function-pointer registry. The existing
switches are easier to audit once the compiler can enforce them. Reworking the
`glr_config_set` special-case chain into a larger dispatch table is possible
but not justified by this review.

---

### 2. The scene-local config contract has two rosters, a third implementation shape, and no completeness test

**Priority: Medium**

**Where**

- [`src/app/glr_actions.c:457`](../../../src/app/glr_actions.c) —
  `cfg_key_in_scene_subset`
- [`src/app/glr_actions.c:490`](../../../src/app/glr_actions.c) —
  `k_cfg_scene_defaults`
- [`src/app/glr_state.c:145`](../../../src/app/glr_state.c) —
  `glr_state_presentation_reset_example_defaults`
- [`src/app/glr_ctrl.c:3426`](../../../src/app/glr_ctrl.c) —
  `glr_ctrl_reset_example_chrome`
- [`tests/test_glr_actions.c:2044`](../../../tests/test_glr_actions.c)
- `AGENTS.md`, “Example metadata & presentation reset”

**What is unclear or inconsistent**

The same 22 scene-local keys are represented by:

- a 22-key membership switch;
- a 22-row key/default table;
- 20 direct `GlrPresentationState` writes, followed by the two peer-state
  writes for camera autorotation and variable-panel visibility.

That third form is reasonable: the pure presentation reset should not invoke
`glr_config_set` side effects, and the peer fields do not belong in
`GlrPresentationState`. The duplicated membership switch is not necessary,
because the key/default table can answer membership already.

Current code behavior is aligned. The comments are not:

- `glr_state.c:146-154` says `ortho_mode` is reset “alongside” but outside the
  subset, while `GLR_CONFIG_ORTHO_MODE` is in both 22-key lists;
- the same comment names only three fields outside the reset even though there
  are intentionally session-inspection and interface settings as well;
- `AGENTS.md` says every non-camera presentation setting resets, which is
  broader than the live contract;
- `glr_actions.c:496-499` says `test_glr_actions.c` pins coverage, but the test
  only checks symbolic values for seven selected slugs.

**Practical cost**

The next scene-local toggle can serialize correctly but leak across F12 if the
reset is missed, or reset across examples despite being intended as a
session-inspection setting. A false test-coverage comment makes this more
likely.

**Smallest reasonable improvement**

1. Implement membership by querying `cfg_scene_default_for_key` and remove the
   duplicate switch.
2. Add a test that `fill_scene_subset` and `fill_scene_defaults` have identical
   slug sets.
3. Add a reset-contract test that changes every scene-local value, runs the
   example-presentation reset callback, and compares against the defaults bag.
4. Correct `glr_state.c` and `AGENTS.md` to distinguish scene-local settings
   from session-inspection/interface settings, with
   `k_cfg_scene_defaults[]` named as the authority.

Do not generate the 20 field assignments or drive the reset through
`glr_config_set`. The explicit direct writes preserve the no-side-effect reset
semantics and are acceptable once a completeness test guards them.

---

### 3. Adding a modal kind can compile into a blank, non-typable modal

**Priority: Medium**

**Where**

- [`src/app/glr_modal.h:5`](../../../src/app/glr_modal.h) — `GlrModalKind`
- [`src/app/glr_modal.c:21`](../../../src/app/glr_modal.c) — `glr_modal_begin`
- [`src/app/glr_modal.c:54`](../../../src/app/glr_modal.c) — `modal_char_ok`
- [`src/app/glr_ctrl.c:2363`](../../../src/app/glr_ctrl.c) — prompt text
- [`src/app/glr_actions.c:1493`](../../../src/app/glr_actions.c) — commit effects

**What is unclear or inconsistent**

The three-way split is architecturally sound:

- `glr_modal` owns input state and character admission;
- the controller snapshot supplies presentation text;
- `glr_actions` owns filesystem/scene side effects.

The extension contract is not guarded. The enum has no count sentinel, all
three dispatches have a silent fallback, and `glr_modal_begin` accepts any
nonzero enum value. A newly added but incompletely wired kind therefore can
capture the keyboard, render an empty prompt, reject every printable character,
and ignore Enter.

**Practical cost**

This produces a user-visible input trap from a missed edit, with no compiler
diagnostic identifying the omitted concern.

**Smallest reasonable improvement**

1. Add `GLR_MODAL_COUNT` and reject values outside
   `(GLR_MODAL_NONE, GLR_MODAL_COUNT)` in `glr_modal_begin`.
2. Make the character-policy, prompt, and commit dispatches exhaustive switches
   with explicit `NONE` / `COUNT` handling and no `default:`.
3. Add one test that every valid modal kind has a non-empty prompt and the
   intended typing/confirmation policy.

Do not move prompt formatting or commit effects into `glr_modal.c`; the larger
refactor suggested previously would blur a separation that currently works.

---

### 4. Several comments and one duplicate declaration no longer describe the live API

**Priority: Low**

These are cheap, factual corrections rather than a naming or documentation
campaign.

| Location | Drift | Smallest improvement |
|---|---|---|
| [`src/app/glr_actions.h:16`](../../../src/app/glr_actions.h), [`:110`](../../../src/app/glr_actions.h), [`glr_actions.c:1863`](../../../src/app/glr_actions.c) | `glr_actions_apply_defaults` is described as seeding presentation defaults; it only restores the persisted audio mode. | Correct the contract. Rename to `glr_actions_restore_audio_mode` only if its few call sites are already being edited. |
| [`src/app/glr_actions.h:45`](../../../src/app/glr_actions.h) | PLY export is labeled F11 and `output.ply`; F11 advances tutorials and export uses the active scene path. | Correct the enum comment. |
| [`src/app/glr_actions.h:92`](../../../src/app/glr_actions.h) | Audio menu documentation omits Back 10 / Forward 10 although the enum includes them. | Add the two rows to the layout comment. |
| [`src/app/glr_ctrl.h:29`](../../../src/app/glr_ctrl.h), [`src/app/glr_ctrl.c:4696`](../../../src/app/glr_ctrl.c) | Accum-pass text lists only `1/2/4/8/12/16`; the ladder also accepts 6, 10, and 14. | Refer to `GLR_ACCUM_PASS_LADDER` or list the full current set. |
| [`src/app/glr_state.h:107`](../../../src/app/glr_state.h) | `accum_effect` omits Blur Cam. | Name all four states. |
| [`src/app/glr_compositor.h:4`](../../../src/app/glr_compositor.h), [`src/app/glr_ctrl.c:3333`](../../../src/app/glr_ctrl.c) | “Every 2D layer” / “all drawing” ignores host-owned splash and tour layers that intentionally render after the compositor; “independent” filters are mutually exclusive per frame. | Say “all controller-owned drawing” and “independently owned, mutually exclusive by Post FX Scope.” |
| [`src/app/glr_camera_export.h:16`](../../../src/app/glr_camera_export.h) | The header redeclares controller-owned `glr_ctrl_view_record_external_3d_pose`, already declared in `glr_ctrl.h`. The app-wide duplicate-declaration guard does not scan `src/app`, so it misses this. | Remove the duplicate prototype; leave the pending-pose precondition with the pending-pose API. |
| [`src/app/glr_source_document.c:2`](../../../src/app/glr_source_document.c) | Says `repl_demo` links the full-app adapter and a separate adapter at the same time. | State that the full app uses this adapter and `repl_demo` uses `tools/repl_demo/source_document.c`. |
| [`src/app/README.md:151`](../../../src/app/README.md), [`docs/MODULES.md:485`](../../MODULES.md) | `boot/glr_lint_scenes` is absent from both boot rosters. | Add the existing windowless lint path to both maps and the CLI exit-path comment. |
| [`src/app/glr_ctrl.h:36`](../../../src/app/glr_ctrl.h), [`:139`](../../../src/app/glr_ctrl.h) | One color-picker comment is attached to the scroll setter; the GL-state-panel declaration has two stacked comments. | Move/merge comments onto their declarations. |
| [`src/app/glr_ctrl.c:3380`](../../../src/app/glr_ctrl.c), [`:4710`](../../../src/app/glr_ctrl.c) | An app-service installer comment sits above the example-default table; a router-helper banner sits above `glr_ctrl_tick`. | Remove or relocate the stale banners. |
| [`docs/MODULES.md:415`](../../MODULES.md) | Names nonexistent `glr_menu_route`; the live route ends in `glr_action_menu_item_activate`. | Name the actual action entry point. |

**Practical cost**

These comments send a maintainer to the wrong shortcut, wrong lifecycle, or
wrong frame layer. The camera declaration also obscures API ownership.

**Smallest reasonable improvement**

Apply the table as one comment/declaration sweep. Avoid unrelated renames.
Expanding the duplicate-declaration guard to all app headers is possible, but
its current curated scope is a separate guard-design decision and is not
required to fix this duplicate.

---

### 5. Three capture affordances repeat the same synthetic right-click sequence

**Priority: Low**

**Where**

- [`src/app/glr_ctrl.c:4589`](../../../src/app/glr_ctrl.c) —
  `glr_ctrl_open_gl_state_popup`
- [`src/app/glr_ctrl.c:4610`](../../../src/app/glr_ctrl.c) —
  `glr_ctrl_open_assign_plot`
- [`src/app/glr_ctrl.c:4631`](../../../src/app/glr_ctrl.c) —
  `glr_ctrl_open_command_description`

**What is unclear or inconsistent**

All three build a snapshot, resolve a source row, and send scripted pointer
motion plus right-button down/up. Their comments already describe the shared
shape. The color-picker hook is correctly different because it opens from a
swatch rather than a row right-click.

**Practical cost**

A change to capture coordinate mapping or synthetic click ordering has three
copies to update.

**Smallest reasonable improvement**

If one of these hooks is being modified, extract a file-static
`glr_ctrl_right_click_source_line(int line)` and keep each public function's
distinct success check. Do not open `glr_ctrl.c` solely for this cleanup.

A generalized capture-affordance framework would add indirection and is not
justified.

---

## Large files and dependencies reviewed without a recommendation

- **`glr_ctrl_display_frame` (433 lines):** clear, chronological, profiled, and
  heavily documented where ordering is non-obvious. Its overlay tail is not an
  independently owned subsystem. No extraction or size ratchet is recommended.
- **`glr_ctrl_init_gl` and `glr_action_menu_item_activate`:** long bootstrap and
  dispatch sequences, not unclear mixed algorithms. Splitting them would trade
  local order for navigation overhead.
- **`glr_audio.c` (native + Emscripten):** one explicit compile-time backend
  split over shared playlist semantics. Correct the native-only wording and
  dead nested Emscripten conditionals when audio is next touched; do not split
  the module.
- **`glr_actions` / `glr_config`:** closely coupled because the descriptor
  table feeds both menu actions and keyed config access. Moving the table would
  relocate rather than remove that coupling. The only immediately misleading
  part is the audio-only “apply defaults” name/comment in Finding 4.
- **Host / Bridge adapters:** the concepts are consistent even when two
  historical filenames say `bridge` while installing a Host. The installer
  names and callback types make ownership clear enough; no rename is warranted.
- **`glr_tour_snapshot` tutorial assignment:** symmetric at the data level and
  valid for the current plain struct. Add a restore hook only if tutorial state
  later gains an invariant.
- **`boot/splash`:** the prefix exception is harmless and discoverable in the
  boot map. Renaming files and public functions would be style-only churn.

The targeted guards also pass on the reviewed tree:
`check-app-boot-band`, `check-glr-ctrl-not-editor-mirror`, and the currently
scoped `check-duplicate-api-decls`.

---

## Patterns working particularly well

1. **Boot / frame-time direction is explicit and guarded.** Startup-only code
   belongs in `boot/`; reusable runtime dump formatters remain below it.
2. **`glr_ctrl_internal.h` is a good sibling seam.** Controller satellites
   share private state without inflating the public API.
3. **Physical and scripted input converge below physical tour arbitration.**
   Capture/tour input exercises the normal routed behavior without duplicating
   it.
4. **`GLR_ACCUM_PASS_LADDER` is the right single-source pattern.** Menu labels,
   numeric steps, and validation derive from one list; only prose drifted.
5. **Camera declarations document ordering contracts.** Destination vs live
   camera state, transition settling, and frame-safe external-pose recording
   are explained where callers need them.
6. **Tour responsibilities are well separated.** Catalog, pointer engine,
   rewind snapshot, and presence animation have distinct ownership.
7. **Frame profiling mirrors actual host ownership.** Controller work, host
   overlays, present, and depth capture are bracketed where they truly run.

---

## Minimal execution plan

| Order | Work | Recommendation |
|---|---|---|
| 1 | Config switch exhaustiveness, duplicate-key validation, and config-toggle docs | **Do.** Small change with the largest prevention value. |
| 2 | Scene-subset single-source membership plus completeness/reset tests and corrected contract comments | **Do.** Localized and directly protects the next scene setting. |
| 3 | Modal enum/count/exhaustive-switch guard | **Do when `glr_modal` is next touched.** No responsibility move. |
| 4 | Factual comment/declaration sweep | **Safe but Low.** Bundle as documentation hygiene; avoid opportunistic renames. |
| 5 | Synthetic right-click helper | **Defer until adjacent work.** |

Stop after these targeted changes. Do not start a general “simplify
`src/app`” effort.

## Overall assessment

`src/app` does not need significant cleanup. Its breadth is appropriate for
the application composition layer, and the existing boundaries are more useful
than a lower line count would be. The warranted work is a pair of extension
guards, an optional modal exhaustiveness check, and correction of factual
documentation drift. Everything else should wait for a concrete change that
makes the local cleanup free or necessary.
