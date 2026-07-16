# Release Cleanup: Code-Smell Scan Findings & Recommended Work

## Status — ACTIVE (2026-07-16)

Pre-release cleanup pass driven by `scripts/code-smells.sh`. All six
checks have now been run (clangd, clang-tidy, lizard, cppcheck, PMD
CPD via Docker, churn×size); the raw outputs live in
`build/code-smells/` (gitignored — regenerate with
`./scripts/code-smells.sh --all`). Every finding referenced below was
manually triaged; false positives are recorded here so the triage does
not need repeating.

Headline: **clangd reported 0 diagnostics** — but see A0: the compile
DB it ran against had a single entry, so flags for the other TUs were
inferred. The inferred flags looked correct in the logs (all TUs share
one flag set), but the claim needs re-verifying after A0 lands. The
real work is (A) scanner fixes so future runs are trustworthy, (B) a
verified dead-code sweep, (C) duplication hoists, and optionally (D)
complexity refactors.

- **A — Fix `scripts/code-smells.sh` signal quality.** NOT STARTED
- **B — Dead-code sweep.** IN PROGRESS — facade-symmetry retains
  landed (intent comments + cppcheck suppressions, 2026-07-16);
  delete candidates below not yet removed.
- **C — Duplication hoists (CPD).** NOT STARTED
- **D — Complexity refactors (opportunistic).** NOT STARTED

## A — Scanner fixes (`scripts/code-smells.sh`)

Do these first; they make every later re-scan trustworthy.

0. **Compile-database integrity.** The scanner trusts any existing
   `compile_commands.json` and ignores Bear build failure
   (`bear -- make ... || true`). At scan time the DB held **1 entry**
   (`src/ui/app/panels.c`, from an incremental rebuild) while
   clangd/clang-tidy analyzed 128 TUs on inferred flags. Fix
   `ensure_compile_commands` to (a) regenerate atomically (build to a
   temp file, move into place only on Bear success), and (b) verify
   coverage — every `.c` in the scanned set must have a DB entry —
   before declaring clangd/tidy results trustworthy.
1. **clang-tidy is missing the macOS sysroot.** The compile DB is
   built with Apple gcc; Homebrew clang-tidy then can't find system
   headers (118 `clang-diagnostic-error` lines like `'ctype.h' file
   not found`), silently degrading analysis of those TUs. Fix: add
   `--extra-arg=-isysroot"$(xcrun --show-sdk-path)"` to the clang-tidy
   invocation in `run_clang_tidy`, guarded on Darwin (`uname` check)
   so the script keeps working on Linux hosts.
2. **Vendored code drowns the clang-tidy summary.** 31,220 raw
   diagnostics → 24,443 after excluding `miniaudio.h` /
   `third_party/freeglut` → **1,425** after also dropping pure-style
   checks. Fix: filter vendored paths out of the diagnostics grep, and
   extend the default `CLANG_TIDY_CHECKS` with
   `-readability-identifier-length,-readability-braces-around-statements,`
   `-readability-uppercase-literal-suffix,-readability-math-missing-parentheses,`
   `-bugprone-easily-swappable-parameters,-misc-include-cleaner`.
3. **cppcheck `unknownMacro` poisons `unusedFunction`.** cppcheck
   cannot expand `REPL_EXPORT_STRINGIFY` (`src/repl/export_display.c`)
   and `REPL_DEFINE_CATALOG_TAG_WRAPPERS` (`src/repl/examples.c`,
   `src/repl/tutorials.c`), aborts those TUs, and then flags everything
   called only from them as unused (verified false: `emit_export_cam_lines`,
   `emit_footer_post_init`, `emit_export_init_section_to_file`,
   `export_source_text_view`, `export_uses_tess_commands` are all live,
   called from `export_display.c`). Fix: pass `-D` stubs for the two
   macros (or the defining header) to cppcheck.
4. **cppcheck has no platform defines.** Without `-D__APPLE__` it
   analyzes `#else` branches of platform-gated code, producing false
   `knownConditionTrueFalse` findings (e.g. "clipboard read always
   returns 0" in `src/app/glr_actions.c` — that's the non-macOS stub
   branch). Fix: add `-D__APPLE__` when the host is macOS.
5. **Summary-count nits.** The cppcheck quick-count grep
   (`'^\[|:[0-9]+:'`) also counts `note:` continuation lines (~3×
   inflation) — count trailing `[checkName]` tags instead. The bear
   comment says `-DGL_STUBS` but the build uses `USE_GL_STUBS=1`.
   lizard, CPD, **and churn** scan only `src/` and skip root
   `gl_repl.c` (its churn×size score of ~24k would place it in the
   top-30 table, so the D ranking is incomplete without it).
6. **After B/C land:** keep `MIN_TOKENS=80` (raising it would blind
   CPD to new 80–119-token duplicates). Instead, check in a baseline
   of the accepted residual blocks (file-pair + token-count
   fingerprints, same remove-only ratchet pattern as
   `scripts/baselines/palette-coverage.txt`) and have the summary
   diff against it, so only *new* duplication surfaces.

## B — Dead-code sweep

Each candidate verified by whole-repo grep (excluding `third_party/`,
`build/`): definition + header declaration exist, zero other
references. Two classes of false positive were hit during triage —
account for both before deleting anything further:

- **Macro aliases.** `repl_eval_predef_count_mut` grep-matches nothing
  by name, but backs the heavily used `g_num_predef_vars_mut` macro
  (`src/repl/eval.h:176`) — it is **live**, not dead. Verification
  must grep for `#define`s wrapping the symbol, not just call sites.
  The remaining candidates below were re-checked for aliases (none).
- **Out-of-language callers.** The `glr_web_*` functions cppcheck
  flagged are `EMSCRIPTEN_KEEPALIVE` exports `ccall`'d from
  `packaging/web/shell.html` (see `Makefile` `EXPORTED_FUNCTIONS`).

**Retained for facade symmetry — DONE (2026-07-16).** These state
facade accessors keep the read/mut/reset families complete even while
uncalled. Each now carries an intent comment plus a
`cppcheck-suppress unusedFunction` marker (the scanner passes
`--inline-suppr`, so they stay out of future scans):

| Function | Definition | Symmetry role |
|---|---|---|
| `editor_state_buffer` | `src/editor/state.c` | buffer view beside line accessors |
| `repl_state_document_cmd_at_mut` | `src/repl/state.c` | `_mut` twin of `repl_state_document_cmd_at` |
| `repl_state_document_capacity` | `src/repl/state.c` | pairs with `repl_state_document_count` |
| `repl_state_flat_program_reset` | `src/repl/state.c` | per-slice reset family |
| `repl_state_import_export_reset` | `src/repl/state.c` | per-slice reset family |

**Delete candidates (non-facade, alias-checked):**

| Function | Definition |
|---|---|
| `repl_expr_cache_bytes` | `src/repl/expr_program.c:168` |
| `scene_snapshot_copy` | `src/repl/scene_snapshot.c:21` |
| `repl_eval_if_condition` | `src/repl/text_helpers.c:593` (plain wrapper; `_captured` variant is the live one) |
| `prof_fps_window_secs` | `src/support/cpuprof.c:282` |

Removals must keep `check-duplicate-api-decls` and `make test-stubs`
green.

## C — Duplication hoists (PMD CPD, 40 blocks @ 80 tokens)

Ranked by value; raw locations in `build/code-smells/cpd.txt`.

1. **`src/app/glr_audio.c` native/web split duplication** — the
   single biggest cluster: five blocks pairing the native half with
   the web half of the same file (69↔925, 320↔1023, 340↔1050,
   600↔1933, 660↔2014; largest 55 lines / 346 tokens). Duplicated
   content: the module static-state block, `reset_audio_module_state`,
   the string helpers (`audio_copy_string`, `audio_basename`,
   `audio_derive_display_name`), and the `glr_audio_set_playlist` /
   `glr_audio_play_music` wrappers. Hoist the shared statics + helpers
   above/outside the `#ifdef` backend split (~120 duplicated lines
   removed, one file).
2. **`src/repl/import.c` payload-parser clones** — the
   `glPointParameterfv` / `glClipPlane` / `glMaterialfv` readers
   repeat one "extract payload between outer parens, then read coeffs
   from `{...}` or the helper call" skeleton (blocks 1306↔1393↔1483,
   1328↔1415, 1339↔1426). One shared extraction helper kills five
   blocks; import.c is also the top lizard-warning file (16), so this
   pays twice. Same skeleton three ways on the export side:
   `src/repl/export_cmd_writer.c:581/635/679`.
3. **Cross-module verbatim copies** (need a shared home, not just a
   local helper):
   - `geometry_guides.c:44` ↔ `transform_guides.c:159` — 43-line
     `*_record_label`; belongs with `guides_shared.h`.
   - `transform_guides.c:132` ↔ `edit_overlays.c:117` —
     `mat4_mul_col_major` / `mat4_point_col_major`; candidate home is
     `src/repl/transform_utils.h` (already the header-only matrix
     helper spot).
   - `transform_guides.c:1043` ↔ `autonormal.c:474` — backward
     transform-stack walk (pop/push/load-identity accounting).
   - `parser.c:158` ↔ `text_helpers.c:250` — funcN-name/alias token
     scan (23 lines).
   - `compile.c:590` ↔ `reformat.c:74` — `static float` decl-prefix
     scan.
4. **Known-deliberate — do not "fix":**
   - `executor.c:195` ↔ `transform_utils.h:42` — documented in
     CLAUDE.md: transform_utils.h deliberately mirrors executor
     transforms to avoid linking executor.h.
   - `export.c:128` ↔ `import.c:2141`
     (`export_line_comment_start`) — adjacent to the documented
     `IMPORT_EXPORT_STATE` macro-block duplication; movable to a
     shared TU if desired, but same-family policy applies.
5. **Accepted small residue** (~81–108-token two-site repeats inside
   single files): `eval.c` scratch-subscript rewriters + for-header
   parsers, `backdrop.c` sky domes, `scene_tabs.c` rounded-rect loops,
   `editor/input.c` commit paths (1591↔1735), `commit.c` 949↔992,
   `menu_bar.c` hit-test, `glr_ctrl_router.c` 405↔427,
   `replay_playback.c`, `tutorial_animation.c`, `text_panel.c`,
   `ui/support/cpuprof.c`, `text_helpers.c` 148↔345, `examples.c`
   363↔621, `compile.c` 2004↔2191. Fine to ship as-is; revisit only
   if touching those files anyway.

## D — Complexity refactors (opportunistic, not release-blocking)

lizard: 229 warnings at CCN 15 / length 150. Top offenders by CCN:

| Function | CCN | NLOC | File |
|---|---|---|---|
| `repl_exec_cursor_step` | 126 | 277 | `src/repl/executor.c:556` |
| `repl_tutorial_validate_entry` | 86 | 213 | `src/repl/tutorials.c:774` |
| `mesh_ply_write` | 84 | 273 | `src/support/mesh_ply.c:274` |
| `parse_command` | 81 | 244 | `src/repl/parser.c:1347` |
| `reformat_var_decl_text` | 73 | 99 | `src/repl/reformat.c:68` |
| `update_autocomplete` | 71 | 156 | `src/app/glr_completion.c:302` |
| `commit_current_input` | 66 | 234 | `src/editor/input.c:764` |

Prioritize by churn overlap: `parse_command`, `commit_current_input`,
and `update_autocomplete` live in the highest-churn files;
`mesh_ply_write` is stable and can wait. The churn×size table
(`churn-size.txt`) puts `src/app/glr_ctrl.c` at 4× the runner-up
(score 854k vs `glr_actions.c` 214k) — continuing the peer-subsystem
extractions is the long-term answer there, not a release task.

## Triaged non-findings (do not re-litigate)

- **cppcheck `uninitvar` in `src/`** — all false positives:
  `export_cmd_writer.c` `step_v` is set (`*cfg->step = 1.0f`) before
  any success return of `repl_eval_parse_for_header`;
  `visible_vars.c` frames are written before `depth++`. The three
  `verr` sites in `compile.c` (2015/2203/2544) rely on the validator's
  write-on-failure contract — optionally add `verr[0] = '\0';` as
  belt-and-suspenders.
- **cppcheck `nullPointerOutOfResources`** — all 68 are unchecked
  `fopen`/`malloc` in `tests/`; not release-relevant.
- **62 `uninitvar` in `tests/test_eval.c`** — one `ASSERT_FOR` macro
  pattern cppcheck can't follow.
- **`knownConditionTrueFalse` on platform-gated code** — artifact of
  scanner issue A4.
- Remaining cppcheck style tiers (41 `variableScope`, 38
  `constVariablePointer`, 47 `staticFunction` in src) are cheap but
  low-value; some `staticFunction` results may be poisoned by A3 —
  re-scan after the scanner fixes before acting.

## Validation

Per stage: `make test` (ASan+UBSan) and `make check-state-ownership`
(includes `check-c99`, include-style, palette, keymap guards). B and C
touch portability-sensitive files — cross-check on gracemont
(`make check-c99 && make test-stubs`) before landing. The
`glr_audio.c` hoist (C1) must keep both backends building: native
`make gl-repl`, stubs `make gl-repl USE_GL_STUBS=1`, and web
`make web` (or `scripts/build-web.sh`).

**A has its own acceptance run** — the make targets above never
invoke the scanner, so after the A fixes land, run
`./scripts/code-smells.sh --all` and verify directly:

- every `.c` in the scanned set appears in both the clangd and
  clang-tidy logs (no TU silently skipped), and the compile DB
  coverage check (A0) passed rather than being bypassed;
- zero `clang-diagnostic-error` in the clang-tidy output (A1);
- zero `unknownMacro` in the cppcheck output (A3);
- no tool reported "skipping" in the run transcript;
- the false `unusedFunction` findings from A3's parse failures are
  gone, and the B facade retains stay suppressed.
