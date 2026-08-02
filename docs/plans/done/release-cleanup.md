# Release Cleanup: Code-Smell Scan Findings & Recommended Work

## Status - ACTIVE (2026-07-16)

Pre-release cleanup pass driven by `scripts/code-smells.sh`. All six
checks have now been run (clangd, clang-tidy, lizard, cppcheck, PMD
CPD via Docker, churn×size); the raw outputs live in
`build/code-smells/` (gitignored - regenerate with
`./scripts/code-smells.sh --all`). Every finding referenced below was
manually triaged; false positives are recorded here so the triage does
not need repeating.

Headline: **clangd reported 0 diagnostics**, re-verified after A0 with
all 128 scanned translation units present in the compile database.
The real work is now (B) a verified dead-code sweep, (C) duplication
hoists, and optionally (D) complexity refactors; the Phase A scanner
fixes and acceptance run are complete.

- **A - Fix `scripts/code-smells.sh` signal quality.** ✅ DONE (A0–A5;
  A6's CPD baseline is now unblocked and still open)
- **B - Dead-code sweep.** ✅ DONE - facade-symmetry retains annotated,
  non-facade candidate functions deleted (2026-07-16).
- **C - Duplication hoists (CPD).** ✅ DONE (2026-07-16) - C1, C2, and
  all five C3 pairs hoisted; see the per-item notes below. Validated:
  `make test` (48/48 binaries), `make check-state-ownership`,
  `check-duplicate-api-decls`, native `make gl-repl`, stubs
  `make gl-repl USE_GL_STUBS=1`, web `scripts/build-web.sh`, and
  real-gcc `make check-c99` + `make test-stubs` on gracemont.
- **D - Complexity refactors (opportunistic).** NOT STARTED

## A - Scanner fixes (`scripts/code-smells.sh`)

Do these first; they make every later re-scan trustworthy.

0. **Compile-database integrity.** ✅ DONE - `ensure_compile_commands`
   now compares the normalized unique DB file set against every scanned
   `.c` TU (counts alone cannot catch duplicates masking omissions). An
   incomplete DB is regenerated when allowed or hard-fails the analyzer;
   Bear writes to a same-directory candidate that is validated before the
   atomic rename, so a failed/partial rebuild preserves the previous DB.
1. **clang-tidy is missing the macOS sysroot.** ✅ DONE - added
   `--extra-arg=-isysroot$(xcrun --show-sdk-path)` to `run_clang_tidy`;
   gated on `xcrun` availability so non-macOS hosts skip it.
2. **Vendored code drowns the clang-tidy summary.** ✅ DONE - diagnostics
   grep now filters `miniaudio.h` / `third_party/freeglut` lines; default
   `CLANG_TIDY_CHECKS` extended with
   `-readability-identifier-length,-readability-braces-around-statements,`
   `-readability-uppercase-literal-suffix,-readability-math-missing-parentheses,`
   `-bugprone-easily-swappable-parameters,-misc-include-cleaner`.
3. **cppcheck `unknownMacro` poisons `unusedFunction`.** ✅ DONE - added
   `-Isrc` (matches build's include path) so cppcheck resolves project
   headers; added `-D` stubs for `REPL_EXPORT_STRINGIFY{,2}` so the
   `#ifndef` guard in `export.h` skips the stringification definition.
   Excluded `tools/repl_live_demo/scenes/`, whose staged REPL snippets
   (including top-level loops) are not standalone C translation units.
4. **cppcheck has no platform defines.** ✅ DONE - added `-D__APPLE__`
   on macOS (detected via `uname -s`).
5. **Summary-count nits.** ✅ DONE - cppcheck count grep now matches
   trailing `[checkName]` tags; bear comment fixed (`USE_GL_STUBS=1`);
   lizard, CPD, and churn all include root `gl_repl.c` (CPD switched
   from `--dir` to `--file-list`), and churn applies the same explicit
   2026-05-23 cutoff to both the root file and post-reorg `src/` paths.
6. **After B/C land:** keep `MIN_TOKENS=80` (raising it would blind
   CPD to new 80–119-token duplicates). Instead, check in a baseline
   of the accepted residual blocks (file-pair + token-count
   fingerprints, same remove-only ratchet pattern as
   `scripts/baselines/palette-coverage.txt`) and have the summary
   diff against it, so only *new* duplication surfaces.

## B - Dead-code sweep

Each candidate verified by whole-repo grep (excluding `third_party/`,
`build/`): definition + header declaration exist, zero other
references. Two classes of false positive were hit during triage -
account for both before deleting anything further:

- **Macro aliases.** `repl_eval_predef_count_mut` grep-matches nothing
  by name, but backs the heavily used `g_num_predef_vars_mut` macro
  (`src/repl/eval.h:176`) - it is **live**, not dead. Verification
  must grep for `#define`s wrapping the symbol, not just call sites.
  The remaining candidates below were re-checked for aliases (none).
- **Out-of-language callers.** The `glr_web_*` functions cppcheck
  flagged are `EMSCRIPTEN_KEEPALIVE` exports `ccall`'d from
  `packaging/web/shell.html` (see `Makefile` `EXPORTED_FUNCTIONS`).

**Retained for facade symmetry - DONE (2026-07-16).** These state
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

**Delete candidates (non-facade, alias-checked) - ✅ DELETED (2026-07-16):**

| Function | Definition | Status |
|---|---|---|
| `repl_expr_cache_bytes` | `src/repl/expr_program.c:168` | Deleted |
| `scene_snapshot_copy` | `src/repl/scene_snapshot.c:21` | Deleted |
| `repl_eval_if_condition` | `src/repl/text_helpers.c:593` (plain wrapper; `_captured` variant is the live one) | Deleted |
| `prof_fps_window_secs` | `src/support/cpuprof.c:282` | Deleted |

Removals must keep `check-duplicate-api-decls` and `make test-stubs`
green.

## C - Duplication hoists (PMD CPD, 40 blocks @ 80 tokens) - ✅ DONE (2026-07-16)

Ranked by value; raw locations in `build/code-smells/cpd.txt`. How each
landed:

1. shared `#define`s, ~20 backend-shared statics,
   `reset_audio_shared_state()` (each backend's
   `reset_audio_module_state` now calls it plus its own fields), the
   three string helpers, and the `glr_audio_set_playlist` /
   `glr_audio_play_music` wrappers hoisted above the
   `#if defined(__EMSCRIPTEN__)` split.
2. import side: one `import_parse_payload_call()` (prefix match →
   outer-paren payload → peel N enum tokens → `{...}`-or-helper value
   list → C-to-REPL convert) backs all three readers. Export side: one
   `export_parse_vector_call()` front half backs the three
   `write_*_as_c89` writers.
3. all five pairs: `render3d_guide_record_label` →
   `guides_shared.h` (header-only, keeps the isolated guides test
   link-free); `mat4_mul_col_major` / `mat4_point_col_major` +
   the new `TransformScopeScan` backward-walk iterator →
   `transform_utils.h`; `repl_scan_func_name_token` (lexical half;
   callers keep their own alias resolution) and
   `repl_scan_decl_float_prefix` (also covers the second copy inside
   `compile.c`) → `text_helpers.{c,h}`.

Original findings:

1. **`src/app/glr_audio.c` native/web split duplication** - the
   single biggest cluster: five blocks pairing the native half with
   the web half of the same file (69↔925, 320↔1023, 340↔1050,
   600↔1933, 660↔2014; largest 55 lines / 346 tokens). Duplicated
   content: the module static-state block, `reset_audio_module_state`,
   the string helpers (`audio_copy_string`, `audio_basename`,
   `audio_derive_display_name`), and the `glr_audio_set_playlist` /
   `glr_audio_play_music` wrappers. Hoist the shared statics + helpers
   above/outside the `#ifdef` backend split (~120 duplicated lines
   removed, one file).
2. **`src/repl/import.c` payload-parser clones** - the
   `glPointParameterfv` / `glClipPlane` / `glMaterialfv` readers
   repeat one "extract payload between outer parens, then read coeffs
   from `{...}` or the helper call" skeleton (blocks 1306↔1393↔1483,
   1328↔1415, 1339↔1426). One shared extraction helper kills five
   blocks; import.c is also the top lizard-warning file (16), so this
   pays twice. Same skeleton three ways on the export side:
   `src/repl/export_cmd_writer.c:581/635/679`.
3. **Cross-module verbatim copies** (need a shared home, not just a
   local helper):
   - `geometry_guides.c:44` ↔ `transform_guides.c:159` - 43-line
     `*_record_label`; belongs with `guides_shared.h`.
   - `transform_guides.c:132` ↔ `edit_overlays.c:117` -
     `mat4_mul_col_major` / `mat4_point_col_major`; candidate home is
     `src/repl/transform_utils.h` (already the header-only matrix
     helper spot).
   - `transform_guides.c:1043` ↔ `autonormal.c:474` - backward
     transform-stack walk (pop/push/load-identity accounting).
   - `parser.c:158` ↔ `text_helpers.c:250` - funcN-name/alias token
     scan (23 lines).
   - `compile.c:590` ↔ `reformat.c:74` - `static float` decl-prefix
     scan.
4. **Known-deliberate - do not "fix":**
   - `executor.c:195` ↔ `transform_utils.h:42` - documented in
     CLAUDE.md: transform_utils.h deliberately mirrors executor
     transforms to avoid linking executor.h.
   - `export.c:128` ↔ `import.c:2141`
     (`export_line_comment_start`) - adjacent to the documented
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

## D - Complexity refactors (opportunistic, not release-blocking)

lizard: 230 warnings at CCN 15 / length 150 (including root
`gl_repl.c`). Top offenders by CCN:

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
(`churn-size.txt`) puts `src/app/glr_ctrl.c` at 3× the runner-up
(score 566k vs `src/ui/app/repl_code_panel.c` 189k) - continuing the
peer-subsystem extractions is the long-term answer there, not a
release task.

## Triaged non-findings (do not re-litigate)

- **cppcheck `uninitvar` in `src/`** - all false positives:
  `export_cmd_writer.c` `step_v` is set (`*cfg->step = 1.0f`) before
  any success return of `repl_eval_parse_for_header`;
  `visible_vars.c` frames are written before `depth++`. The three
  `verr` sites in `compile.c` (2015/2203/2544) rely on the validator's
  write-on-failure contract - optionally add `verr[0] = '\0';` as
  belt-and-suspenders.
- **cppcheck `nullPointerOutOfResources`** - all 68 are unchecked
  `fopen`/`malloc` in `tests/`; not release-relevant.
- **62 `uninitvar` in `tests/test_eval.c`** - one `ASSERT_FOR` macro
  pattern cppcheck can't follow.
- **`knownConditionTrueFalse` on platform-gated code** - artifact of
  scanner issue A4.
- Remaining cppcheck style tiers (41 `variableScope`, 38
  `constVariablePointer`, 47 `staticFunction` in src) are cheap but
  low-value; some `staticFunction` results may be poisoned by A3 -
  re-scan after the scanner fixes before acting.

## Validation

Per stage: `make test` (ASan+UBSan) and `make check-state-ownership`
(includes `check-c99`, include-style, palette, keymap guards). B and C
touch portability-sensitive files - cross-check on gracemont
(`make check-c99 && make test-stubs`) before landing. The
`glr_audio.c` hoist (C1) must keep both backends building: native
`make gl-repl`, stubs `make gl-repl USE_GL_STUBS=1`, and web
`make web` (or `scripts/build-web.sh`).

**A has its own acceptance run** - the make targets above never
invoke the scanner, so after the A fixes land, run
`./scripts/code-smells.sh --all` and verify directly:

- every `.c` in the scanned set appears in both the clangd and
  clang-tidy logs (no TU silently skipped), and the compile DB
  coverage check (A0) passed rather than being bypassed;
- zero `clang-diagnostic-error` in the clang-tidy output (A1);
- zero `unknownMacro` in the cppcheck output (A3);
- zero `syntaxError` in cppcheck (only actual translation units scanned);
- no tool reported "skipping" in the run transcript;
- the false `unusedFunction` findings from A3's parse failures are
  gone, and the B facade retains stay suppressed.
