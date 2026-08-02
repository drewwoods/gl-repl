# C99 sample build (tests stay C2x)

Status: **DONE - implemented as a C99 syntax guard (not a binary-standard
flip)** on branch `enum-path-generalization`. The design body below is
the historical record; what actually shipped is summarized next.

## Implementation outcome

After the "make it easier" review, the chosen architecture is the
**guard** path, not flipping the sample binary's standard:

- **Default build is unchanged C2x** (`make sample`, `make test`, CI).
  No object-tree split - the §5 binary-flip approach (and its
  OBJDIR-by-standard ripple) was deliberately *not* taken. This is the
  single biggest effort/risk reduction and is behavior-neutral.
- **`make c99`** (`scripts/check-c99.sh`) syntax-checks the sample
  source set under `-std=c99 -pedantic-errors` on the GL-stub include
  path (`-fsyntax-only`, no codegen, no link). Wired into the
  `check-state-ownership` runner, so it is part of the standard
  `make test-stubs` gate (`▶ check-c99 … OK`), matching the
  `check-no-point-parameter-builds` idiom.

**Probe result (the key de-risk).** Running the bar over all 75 sample
TUs proved the audit *exhaustive*: exactly the four documented blockers,
zero extras (no `#warning` issue, no empty-TU in the product set). The
effort estimate collapsed from a range to a fixed list. Notably this
**confirmed blocker #4 is a real `-pedantic-errors` error** (6×
`-Wstrict-prototypes` on the `void (*)()` GLU casts) - correcting the
earlier review note that doubted it.

Fixes landed (all behavior-neutral, valid under both C2x and C99):

1. `include/c_compat.h` - portable `STATIC_ASSERT`; 3 `_Static_assert`
   sites switched (`glr_debug.c`, `glr_ctrl.c` ×2); `snapshot.h`
   comment refreshed.
2. `src/repl/parser.c` - `, ##__VA_ARGS__` → `, __VA_ARGS__` (fixes all
   18 expansions); unused `WRITE_TEXT_APPEND` deleted.
3. `VariablePanelValueChange` - sole definition moved to
   `variable_panel_drag.h`; `variable_panel_state.h` includes it
   instead of re-typedefing.
4. `src/repl/executor.c` - self-owned prototyped `ReplGluCallback`
   typedef replaces the 6 old-style `void (*)()` casts;
   `src/repl/export.c` emits a matching `_GluCb` typedef so generated
   standalone C is C99-clean too.
5. `scripts/check-c99.sh` + Makefile `c99`/`check-c99` wiring;
   `CLAUDE.md` C99 portability contract section.

Gate green: `make c99` OK, `make test` 4596/4596 (C2x), `make
test-stubs` 5080/5080 (incl. `check-c99`), `make sample` (GL + stubs),
`repl_demo`, `scene_demo`, `make check-state-ownership`.

Deviation from the written design, deliberate: §5's "make the sample
*binary* C99 + OBJDIR split" was replaced by a `-fsyntax-only` guard
(levers 1+3). The earlier locked answer was already "make c99 target +
standard gate"; the binary-flip only entered via a later manual edit,
so the guard path both honors the original decision and is materially
simpler. Folder: moved to `plans/done/`.

---

## Original design (historical record - binary-flip path, not taken)

## Context

This is a deliberately retro OpenGL project; the user wants the
user-facing sample binary to build clean as **C99**. The test suite and
CI can remain on `-std=c2x`; only `make` / `make sample` should move to
C99. Today the sample source compiles only because GCC/Clang accept
C11/GNU constructs as extensions in `-std=c2x`. An audit of the
sample/product translation units found these known blockers for a
`gcc/clang -std=c99 -pedantic-errors` build/syntax check:

1. **C11 `_Static_assert`** - 3 sites: `src/app/glr_debug.c:107`,
   `src/app/glr_ctrl.c:1238`, `src/app/glr_ctrl.c:1240`. `_Static_assert`
   is a C11 keyword; under `-std=c99 -pedantic-errors` it errors.
2. **GNU `, ##__VA_ARGS__`** - `WRITE_TEXT` / `WRITE_TEXT_APPEND` macros
   in `src/repl/parser.c:257` / `:261`. The GNU comma-elision is not ISO
   C and `-pedantic-errors` rejects it.
3. **C11-compatible duplicate typedef redefinition** -
   `VariablePanelValueChange` is forward-typedefed in
   `src/widgets/variable_panel_drag.h:18` and typedefed again with the
   struct body in `src/widgets/variable_panel_state.h:42`. Clang accepts
   that as a C11 typedef-redefinition extension, but rejects it under
   `-std=c99 -pedantic-errors`.
4. **Old-style function-pointer casts for GLU tessellation callbacks** -
   `src/repl/executor.c:137`-`:146` casts callback functions to
   `void (*)()`. Clang diagnoses that as a strict-prototypes error under
   the C99 pedantic bar. `src/repl/export.c:561`-`:566` emits the same
   cast shape into exported C; update it too if exported output is covered
   by the C99 promise.

Everything else in the current sample/product TU set is expected to be
clean for the chosen bar, but `make sample` / `make c99` compiler output
is authoritative:

- `_Alignof` is already gated (`REPL_HAS_ALIGNOF` in `glr_debug.c`:
  `_Alignof` under C11+, GNU `__alignof__` otherwise) - C99-safe as-is.
- `__attribute__((constructor/format/unused))` - GCC/Clang accept these
  under `-std=c99 -pedantic-errors` without diagnostic; the behavioral
  `((constructor))` sites (`src/repl/state.c:198`,
  `src/widgets/tutorial_state.c:25`) are already
  `#if defined(__GNUC__) || defined(__clang__)`-gated. **Chosen bar is
  gcc/clang**, so no change and no behavior change.
- The `union` in `src/ui/editor.h:27` is a *named* member (`} state;`),
  not a C11 anonymous union - fine.
- Sweep found no statement-expressions, `typeof`, `_Generic`, binary
  literals, case-ranges, `[[attributes]]`, `nullptr`, or VLAs.

Scope note: the C99 contract applies to `make sample` / the sample
product source set, not every target. `make test` remains C2x.
Generated / export harness files such as `tests/export_trace_driver.c`
require build-time macros, and some tests use test-only idioms such as
empty variadic initializer macros that are not part of the C99 sample
compatibility promise.

Outcome: `make` / `make sample` produce the sample binary with C99.
`make test` and CI continue using C2x. The standard gate runs both paths
so neither the sample's C99 contract nor the test build silently
regresses.

## Approach

### 1. Portable `STATIC_ASSERT` shim - new header `include/c_compat.h`

`include/` is the documented home for header-only helpers (CLAUDE.md) and
is on every TU's `-Iinclude` path.

```c
#ifndef C_COMPAT_H
#define C_COMPAT_H

/* Real _Static_assert under C11+; C99 negative-array-size fallback
 * otherwise. The fallback drops the message text (only the
 * negative-size error shows) - acceptable. Distinct __LINE__ per use
 * keeps typedef names unique; current call sites are all on separate
 * lines. Valid at file and block scope (typedef). */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)
#else
#  define STATIC_ASSERT__CAT2(a, b) a##b
#  define STATIC_ASSERT__CAT(a, b)  STATIC_ASSERT__CAT2(a, b)
#  define STATIC_ASSERT(expr, msg) \
     typedef char STATIC_ASSERT__CAT(c_compat_static_assert_at_line_, __LINE__) \
         [(expr) ? 1 : -1]
#endif

#endif /* C_COMPAT_H */
```

Then:

- `src/app/glr_debug.c`: `#include <c_compat.h>`; replace the
  `_Static_assert(...)` at line 107 with `STATIC_ASSERT(...)` (keep it
  inside the existing `#if defined(REPL_HAS_ALIGNOF)` guard, same block
  scope).
- `src/app/glr_ctrl.c`: `#include <c_compat.h>`; replace both
  `_Static_assert(...)` (lines 1238, 1240) with `STATIC_ASSERT(...)`
  (file scope).
- `src/ui/snapshot.h:52`: refresh the stale comment that says
  "`_Static_assert` in glr_ctrl.c" → "`STATIC_ASSERT` in glr_ctrl.c".

Keep argument order/messages identical so the c2x build (which takes the
real `_Static_assert` path) is byte-for-byte unchanged.

### 2. Fix the duplicate `VariablePanelValueChange` typedef

Move the complete typedef into `src/widgets/variable_panel_drag.h` and
remove the second typedef from `src/widgets/variable_panel_state.h`.
Then include `widgets/variable_panel_drag.h` from
`variable_panel_state.h` before the handler prototypes.

Target shape:

```c
/* src/widgets/variable_panel_drag.h */
typedef struct VariablePanelValueChange_s {
    char  name[16];
    float value;
} VariablePanelValueChange;
```

`variable_panel_state.h` should use the type but not redeclare it. That
keeps existing include order working for `glr_ctrl.c`,
`variable_panel_drag.c`, and tests that include either header first.

### 3. Replace old-style GLU callback function-pointer casts

The real GLU headers declare `gluTessCallback` with an old-style
callback pointer on some platforms, and the local GL stubs currently use
a cleaner `GLUfuncptr` typedef. Keep source casts C99-clean by adding a
single local callback-pointer typedef in `src/repl/executor.c`, for
example:

```c
typedef void (*ReplGluCallback)(void);
```

Then cast callback registrations to `ReplGluCallback` instead of spelling
`void (*)()` inline. If the real platform header still exposes an
old-style callback parameter and clang diagnoses the header itself, keep
`make c99` on the local GL stubs path or mark external GL includes as
system includes for this target; do not weaken source-owned diagnostics.

Also update the export template in `src/repl/export.c` so generated code
emits a named C99-clean callback typedef and uses that typedef in the
`gluTessCallback` lines, rather than emitting `(void (*)())`.

### 4. Drop the GNU comma-elision in `src/repl/parser.c`

Audit result: **every** `WRITE_TEXT(...)` / `WRITE_TEXT_APPEND(...)` call
passes at least one variadic argument after the format string (verified
across all ~20 call sites; no `WRITE_TEXT(fmt)`-only call exists). So the
`##` is never functionally needed - replace `, ##__VA_ARGS__` with
`, __VA_ARGS__` in both macro definitions (lines 257-264). Zero
behavioral change in either standard mode.

- `WRITE_TEXT_APPEND` is **defined but unused** (only the `#define`
  matches; no call sites). Default: delete it (and its `#undef`). If a
  reviewer prefers a minimal diff, just convert its `##__VA_ARGS__`.

### 5. Make `sample` C99; keep `test` C2x (Makefile)

- Keep the test/development standard as C2x. `make test` should continue
  to compile with `-std=c2x`.
- Make the sample path explicitly C99. `make` (which currently aliases
  `sample`) and `make sample` should compile/link the sample binary with
  `-std=c99 -pedantic-errors`.
- Avoid sharing object paths across different standards. If sample TUs
  build as C99 while tests build as C2x, the object directory or object
  naming must include the standard (for example `build/release-c99/...`
  vs. `build/release-c2x/...`) or use separate sample/test object roots.
  Otherwise a prior `make test` can leave C2x objects that `make sample`
  accidentally reuses, or vice versa.
- Preferred variable shape:

  ```make
  TEST_STD ?= c2x
  SAMPLE_STD ?= c99
  SAMPLE_STANDARD_CFLAGS = -std=$(SAMPLE_STD) -pedantic-errors
  TEST_STANDARD_CFLAGS = -std=$(TEST_STD)
  ```

  Exact variable names can differ, but the Makefile must make it obvious
  that sample and tests intentionally use different standards.
- Keep an explicit C99 guard target, e.g. `make c99` / `check-c99`.
  Since `make sample` is now C99, this target can either build the sample
  through the normal C99 path or run a faster `-fsyntax-only` check over
  the sample/product source set. It should not include generated
  harnesses or test-only files unless it supplies their required build
  context.
- Mark external GL/freeglut include directories as system includes for
  C99 builds, or otherwise keep the C99 guard focused on source-owned
  code. The default real-GL `sample` build must not fail because a
  platform OpenGL header exposes old-style callback typedefs.
- Mirror the project's guard idiom: add `check-c99` into the
  `check-state-ownership` aggregator / documented standard gate, and keep
  `make test` in CI as the C2x path. Match the existing `▶ … OK`
  sub-target formatting.
- The compiler output is the authoritative blocker list; the audit above
  is the expected, not exhaustive, set. Most likely extra is the
  `#warning` in `glr_debug.c` (GCC/Clang generally accept it under
  `-pedantic-errors`; if it errors, gate it the same
  `__GNUC__`/`__clang__` way used nearby). The C99 fallback typedef may
  trip `-Wunused-local-typedefs` at the block-scope site only if
  `-Wextra` is added - current flags are `-Wall` only, so not expected;
  add `-Wno-unused-local-typedefs` to the `c99` target only if it
  surfaces.

### 6. Docs

`CLAUDE.md`: document that `make` / `make sample` build the sample under
C99 (`-std=c99 -pedantic-errors`) while `make test` remains C2x; the
`STATIC_ASSERT` shim (`include/c_compat.h`) is the required spelling for
compile-time asserts (no raw `_Static_assert`); callback function-pointer
typedefs must be named/prototyped rather than old-style `void (*)()`;
`WRITE_TEXT`-style variadic macros must use plain `__VA_ARGS__` (no
`, ##`). Add the C99 sample guard to the Boundary Checks / gate list.

## Critical files

- `include/c_compat.h` - **new**, portable `STATIC_ASSERT`.
- `src/app/glr_debug.c` - include shim; line 107 → `STATIC_ASSERT`.
- `src/app/glr_ctrl.c` - include shim; lines 1238/1240 → `STATIC_ASSERT`.
- `src/ui/snapshot.h` - line 52 comment refresh.
- `src/widgets/variable_panel_drag.h` - own the full
  `VariablePanelValueChange` typedef.
- `src/widgets/variable_panel_state.h` - include/use that typedef; remove
  the duplicate typedef body.
- `src/repl/executor.c` - replace old-style `void (*)()` callback casts
  with a named C99-clean callback typedef.
- `src/repl/export.c` - update exported tessellation callback setup so
  generated C does not emit old-style `void (*)()` casts.
- `src/repl/parser.c` - lines 257-264: drop `, ##`; delete unused
  `WRITE_TEXT_APPEND`.
- `Makefile` - make sample objects/binary use C99, keep test objects on
  C2x, prevent cross-standard object reuse, add `c99` / `check-c99`, and
  wire the C99 guard into aggregator/gate.
- `CLAUDE.md` - document `make sample` C99 and `make test` C2x.

## Reuse / conventions followed

- Feature gating mirrors existing patterns: `REPL_PRINTF_LIKE`
  (`src/repl/util.h`, `__GNUC__`/`__clang__`) and
  `REPL_HAS_ALIGNOF`/`REPL_ALIGNOF` (`src/app/glr_debug.c`,
  `__STDC_VERSION__ >= 201112L`). The shim uses the same
  `__STDC_VERSION__ >= 201112L` test.
- `include/` placement matches CLAUDE.md ("`include/` is for header-only
  helpers and vendored single-header dependencies").
- `make c99` + aggregator wiring matches the established
  `check-state-ownership` standing-guard culture.

## Verification

1. Sample C99 path green: `make sample`, `make sample USE_GL_STUBS=1`,
   and `make c99` / `check-c99`.
2. Test C2x path unchanged and green: `make test`, `make test-stubs`,
   `make check-state-ownership`, plus any existing CI sequence.
3. Confirm standards are actually split: inspect the printed compile
   command or failure output from `make sample` / `make c99` to ensure
   sample TUs use `-std=c99 -pedantic-errors`; inspect `make test` output
   to ensure test TUs still use `-std=c2x`.
4. Confirm no cross-standard object reuse: run `make clean`, `make test`,
   then `make sample`, and verify the sample objects are rebuilt under
   the C99 object path/flags rather than reusing C2x objects.
5. Behavior stays identical: `VariablePanelValueChange` representation
   stays the same, and the parser/export/executor changes are
   spelling-only at runtime. Expect the same pass counts (~4590
   `make test`, ~5074 `make test-stubs`).
6. Spot-check the C99 fallback fires: temporarily break one
   `STATIC_ASSERT` condition, confirm `make c99` fails with the
   negative-array-size error, then revert.

## Folder note

**Completed.** Lifecycle reached its end: in-review → `done/`.
Implemented as the C99 syntax guard (`make c99` / `check-c99` in the
standard gate); the default build stays C2x. See "Implementation
outcome" at the top for the as-built summary and the deliberate
deviation from the binary-flip design below. This file now lives in
`plans/done/` as the historical design + outcome record.
