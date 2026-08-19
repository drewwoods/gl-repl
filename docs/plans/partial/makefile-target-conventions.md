# Makefile target conventions

**Status: partial.** Steps 1-4 of the sequencing in §6 landed 2026-08-19
(commits `build: one help doc per Make target, guarded`, `build: generate make
help from the Makefile itself`, `build: derive .PHONY from the target docs`,
`build: make report shape and check-skipping axes, not targets`). Steps 5-6 --
the `PLATFORM`/`GL_BACKEND` sugar and the verb renames with their grammar
guard -- are **deferred on purpose**: the plan itself calls them optional, and
they carry the ~150-file reference blast radius that steps 1-4 do not. The
text below §1 is the original proposal, unedited; this section records what
landed and where the implementation deviated from it.

### What landed

| Step | Delivered |
|---|---|
| 1 | `bench-code-panel-stencil` and `bench-vertex-labels` adopt `bench-code-panel-text`'s shape (branch the build rules, hoist one documented run rule, gate the recipe with `ifdef <NAME>_OK`); `all` and `require-emcc` gain a `## `; guards `check-make-target-documented` + `check-make-no-duplicate-help` |
| 2 | `help` (pointer tier), `help-%` pattern rule, `help-vars`, generated `help-details` (`scripts/make-help.awk`); `#? ` variable annotations + `PUBLIC_MAKE_VARS`; guard `check-make-var-documented`; the printf prose moved to `docs/ADVANCED_USAGE.md` |
| 3 | `.PHONY` derived from the `## ` docs; the five hand lists deleted; guard `check-make-phony-derived` |
| 4 | `FORMAT=human\|csv` and `SKIP_CHECKS=1`; the five `-csv`/`no-checks` targets kept as forwarders; guard `check-make-fix-pairs` |
| 4.1 | `check-make-target-grammar` **as a ratchet**, ahead of the renames - see below |

All six guards of §4 are in `CHECK_TARGETS`.

### The grammar guard without the renames (§4.1, landed 2026-08-19)

§4.1 reads as the last step of the rename pass, because its allow-lists were
drafted as the *result* of the renames. It does not have to be: the guard's
value is stopping the *next* badly-named target, and that does not depend on
fixing the previous ones. Landed as a ratchet instead:

- `MAKE_TARGET_VERBS` lives in the Makefile, not the guard, so the grammar
  and `make help-<verb>` cannot disagree about what a verb is.
- `ROOT_TARGETS` (23) is the artifact-named set - permanent, allowed to stay
  any size, which is exactly why it is a separate list.
- `LEGACY_TARGETS` (28) freezes the names that predate the grammar. It is
  checked **both ways**: a non-conforming target missing from it fails, and an
  entry that no longer names an existing non-conforming target fails - so a
  rename cannot leave its line rotting, and the count can only fall. Emptying
  it is step 6, and nothing has to move for the guard to be worth having.
- `FOREVER_ALIASES` / `DEPRECATED_ALIASES` are not created yet: with no
  renames there are no old spellings to carry, and the five step-4 forwarders
  (`bench-csv`, `test-only`, ...) already satisfy the grammar on their own.

134 of the 185 documented targets already conformed before this landed.

### Deviations from the proposal, and why

- **`#? ` is its own comment line, not a trailing comment.** §3 shows
  `BUILD ?= release        #? Build mode: ...`. GNU make (3.81 on macOS and
  4.3 on Linux alike) keeps the whitespace *before* a comment inside the
  value, so that spelling sets `BUILD` to `"release        "` -- and
  `SAN ?= address  #? ...` trips its own `ifneq ($(SAN),address)` error arm.
  Verified before choosing the two-line form. The documentation still lives at
  the declaration, which was the point.
- **A `## ` inside a description truncates help.** Every scraper splits on
  `:.*## ` and awk matches leftmost-longest, so a description mentioning the
  marker loses everything before its last occurrence. Found by writing one;
  `check-make-target-documented` now rejects it.
- **`HASH := \#` for the `.PHONY` scrape.** `#` starts a comment in a variable
  assignment (recipe lines are exempt, which is why the help recipes can spell
  the marker literally), so the scrape silently returned empty until the
  marker was smuggled in through a variable.
- **`help-details` groups by *family*, not by verb.** A family is any leading
  dash-segment with 3+ members, computed from the names themselves. With the
  renames deferred, a verb grouping would have put ~50 targets in "other";
  the data-driven grouping needs no list and sharpens automatically if step 6
  ever lands.
- **The five step-4 names are forwarders, not deletions** -- the alternative
  §6 step 4 explicitly offers. They print a deprecation note on stderr,
  where it cannot corrupt a piped CSV. Because they survive, the
  `MAKECMDGOALS` filter (§5 item 0) needed no edit: both names still
  resolve, and the recursive lane recomputes `BUILD=debug` from its own goal.
- **The `fix-`/`check-` pair guard is `check-make-fix-pairs`**, not
  §4.5's `check-make-fix-pairs-check`.
- **`check-make-target-grammar` landed before the renames**, as a
  shrink-only ratchet rather than the post-rename assertion §4.1 describes.
- **`FORMAT` is scoped to the lanes that implement it**, not to the whole
  bench family as §2 assumes: only `bench_repl`, `bench_extedit` and
  `bench_render` have a `--csv` mode. The windowed benches refuse
  `FORMAT=csv` (`BENCH_FORMAT_UNSUPPORTED`) instead of silently returning
  human output, on the same principle §2 states for `ARGS` - a target that
  ignores an axis it is documented to have is a bug. The two build-only lanes
  take neither.
- **`.PHONY` gained three names the hand lists had dropped**: `test-no-checks`,
  `test-only` and (new) `help-vars`. That drift is the concrete argument for
  §4.4.

Verified on macOS (make 3.81, BWK awk) and on gracemont (Ubuntu 24.04, make
4.3, gawk 5.2.1): all five guards, `make check`, `make test-stubs`,
`make test-stubs SKIP_CHECKS=1`, `make run-test-eval`, and the help tiers.

`make help-details` is 182 lines and hides all 92 `check-*` targets. The
problem is not the count -- it is that `help-details` is hand-maintained prose
over a target set with no grammar, so it cannot be generated and a reader
cannot predict a name.

**The two halves of this plan are independent and should be judged
separately.** Generated help (§3) and the mechanical cleanups (§6 steps 1-3)
work on today's names, change no invocation, and carry most of the value;
step 4 is small but does delete five public names. The
naming grammar (§2) is a larger, optional second pass with a ~150-file
reference blast radius.

## 1. The inventory today

All counts below are reproduced by `scripts/make-target-inventory.sh` (checked
in with this plan). **Two universes exist and a guard must declare which it
scrapes:**

- **source** -- names literally declared in the Makefile text: 192
  declarations, **185 unique names**. This is what awk/grep see.
- **evaluated** -- what Make knows after `$(eval)`: the `built_test_binary`
  /`RUN_TEST_TARGETS` machinery generates **258** further `test-*` / `run-test-*`
  aliases from 86 `TEST_BINS` -- `TEST_TARGET_NAMES`, `RUN_TEST_TARGETS` and
  `RUN_TEST_FILE_TARGETS` are 86 names each (**258**), and the inventory
  script's 267 is those plus the 9 hand-written `test-*` lanes its regex also
  matches. A source scraper cannot see the generated ones; only `make -pnR` can.

`make help-details` is 182 lines and scrapes only the source universe, minus
the 92 `check-*` targets it filters out (`$$1 !~ /^check-/`).

Prefix distribution of the documented source set:

| Prefix | Count | Nature |
|---|---|---|
| `check-` | 92 | guards (the `check` aggregator is separate) |
| `bench-` | 16 | benchmarks |
| `test-` | 10 | hand-written lanes (+258 generated aliases) |
| `callgraph-` | 6 | analysis |
| `*-demo` | 9 | **suffix**, not prefix (`HEADLESS_DEMO_TARGETS`) |
| `release-`, `icon-`, `install-`, `fix-`, `audit-`, ... | ~25 | one-offs |

**Only two targets are genuinely undocumented**: `all` (no recipe) and
`require-emcc` (internal). An earlier draft claimed twelve -- that was an
artifact of counting *declarations* rather than names: `gl-repl` and `demos`
are each declared twice with the `## ` on only one arm, and the `else`-branch
error stubs of three benches carry none. All are documented targets.

### The four axes the names conflate

1. **What it does** -- build / run / test / bench / check / report.
2. **What it does it to** -- the app, a demo, a subsystem.
3. **Where it runs** -- native, wasm, OSMesa, real-GL.
4. **How it reports** -- human table vs `--csv`.

Axis 3 appears as a prefix in `web`, `web-serve` and as a suffix in
`test-web`, `bench-web`, `bench-web-gl4es`. Axis 4 appears as a suffix in
`bench-csv`, `bench-render-csv`, `bench-web-csv` -- three targets that exist
only to pass one flag. Axis 2 is a prefix for `bench-*` and `check-*` but a
suffix for `*-demo`.

### Concrete drift found while surveying

- **Duplicated help metadata, not duplicated targets.** Five names are declared
  more than once in source: `gl-repl`, `demos`, and three benches
  (`bench-code-panel-stencil`, `bench-code-panel-text`, `bench-vertex-labels`).
  Every one is legitimate -- the benches are per-OS `ifeq` arms with different
  link lines. The *defect* is narrower: exactly **two** names
  (`bench-code-panel-stencil`, `bench-vertex-labels`) repeat the `## ` doc
  across arms, so each prints twice in `help-details`.
  `bench-code-panel-text` is declared twice and carries **one** `## ` -- it
  already has the shape the other two should adopt, so the fix is a known-good
  in-repo template, not a new invention (see 4.3).
- `test-only` exists solely as an alias for `test-no-checks`; `test-no-checks`
  is a copy of the `test-stubs` recipe minus the `check` prereq.
- `bench-glut-bitmap-build` is a build-only target in a family where every
  other member runs.
- `gl-` means two different things: the product (`gl-repl`,
  `gl-repl-unchained`) and "real GL context" (`gl-tests`). `glut` and `glprobe`
  share the prefix and mean a third and fourth thing.
- Five target-group variables (`BUILD_TARGETS`, `PACKAGE_TARGETS`,
  `TEST_TARGETS`, `BENCH_TARGETS`, `MAINTENANCE_TARGETS`) are hand-maintained
  lists that exist only to feed `.PHONY`. `CHECK_PHONY_TARGETS` already derives
  itself from the source with awk -- the other five have not followed.

### The env-var / argument surface

| Kind | Variables |
|---|---|
| Build mode | `BUILD`, `SAN`, `NO_SAN` (+ `NOSAN`, `ASAN` spellings), `CC`, `CFLAGS` |
| Link/backend | `FREEGLUT_OSMESA`, `FREEGLUT_VENDOR_LINUX`, `FREEGLUT_LIB_PATH`, `FREEGLUT_INCLUDE_DIR`, `USE_GL_STUBS`, `WEB` |
| Target selection | `SAMPLE`, `ENTRY`, `TEST_CASE`, `EXAMPLES_CATALOG` |
| Run-time passthrough | `ARGS`, four `*_BENCH_ARGS`, `TEST_JOBS`, `TEST_VERBOSE`, `V`/`VERBOSE` |
| Destinations | `MUSIC_DEST`, `ZSHRC`, `CALLGRAPH_FILES_GROUP_CONFIG` |

The real problem here is **discoverability, not spelling**: `ARGS` is honoured
by 18 sites across the bench and run-test families, there is a second
per-bench channel (`*_BENCH_ARGS`, 4 of them), and none of it is documented
outside one printf at the bottom of `help-details`.

## 2. Proposed grammar

> **A target name is `<verb>-<subject>[-<subject-qualifier>]`. Axes 3 and 4 --
> where it runs and how it reports -- are never name segments; they are
> variables.**

### Verbs

| Verb | Meaning | Exit contract |
|---|---|---|
| *(none)* | build a **named artifact** (the target *is* the artifact) | build failure |
| `run-` | build **and execute** | the program's status |
| `test-` | a test lane | test failure |
| `bench-` | a benchmark lane | run failure |
| `check-` | a guard: findings **are** violations | non-zero on any finding |
| `fix-` | the mutating twin of a `check-` (must pair 1:1) | non-zero on failure to fix |
| `show-` | print a report to stdout; writes no files | see below |
| `gen-` | regenerate a checked-in artifact | non-zero on failure |
| `clean-` | remove build output | non-zero on failure |
| `install-` | mutate the developer's environment / fetch assets | non-zero on failure |
| `release-` | release orchestration | non-zero on failure |
| `help-` | print target/variable documentation | non-zero on failure |
| `internal-` | implementation detail; never typed by a human | n/a |

A **bare verb is always a valid target** (`test`, `bench`, `check`, `clean`,
`release`, `help`), so the aggregates need no allow-listing.

#### The `show-` exit contract

`show-*` means **"findings are not violations"** -- not "always exits 0". A
report still exits non-zero when it cannot produce a trustworthy report:
a prerequisite fails to build, a required tool (`cflow`, `valgrind`, `python3`)
is missing, or the generator itself errors. What it must *not* do is exit
non-zero because the report contains rows someone dislikes. That is the whole
distinction from `check-`, and it is why `analyze` becomes `show-analyze`
rather than `check-analyze`: the static analyzer's output is informational,
and there is no ratchet gating it today.

`check-make-target-grammar` cannot verify this contract mechanically. It is a
review rule, stated here so the split stays meaningful.

### Complete before/after inventory

Every target with a recipe, classified. 185 target names; the residual
unclassified set is **empty** (verified by scraping the Makefile and
subtracting renames, artifact roots, bare verbs and verb-prefixed names).

**Artifact-named roots** (build a named thing; no verb, allow-listed):
`all`, `gl-repl`, `gl-repl-unchained`, `render3d-asset-builder`, `app`, `web`,
`demos`, `glut`, plus `FORCE` (a Make idiom).

**Already conforming, unchanged:** the 92 `check-*`, the 10 `test-*` lanes
kept, 16 `bench-*`, 4 `release-*`, `fix-doc-links`, `fix-unicode`,
`install-hooks`, `install-completions`, `help`, `help-details`, all
`run-test-*` and `internal-*`.

**Renamed (~41):**

| Before | After |
|---|---|
| `render3d-hot-lib` | `internal-render3d-hot-lib` (its doc already says "invoked by the running hot host") |
| `require-emcc` | `internal-require-emcc` |
| `gl-tests` | `test-gl` |
| `coverage` | `test-coverage` (it runs the suite; the report is a side effect) |
| `extract` | `run-extract` (it runs a program to emit `.ply`/`.glr`) |
| `glprobe`, `glprobe-preload` | **unchanged** -- both only *build* (`built $(GLPROBE_BIN)`); `run-` would misdescribe them. They join `ROOT_TARGETS` as artifact builds with a required `SAMPLE=`. |
| `keymap-list`, `config-list`, `palette-list` | `show-keymap`, `show-config`, `show-palette` |
| `capacity-matrix`, `unicode-count` | `show-capacity`, `show-unicode` |
| `lines`, `lines-test` | `show-lines`, `show-lines SCOPE=test` |
| `find-trailing-whitespace` | `show-trailing-whitespace` |
| `audit-editor-ownership` | `show-editor-ownership` |
| `analyze` | `show-analyze` |
| `callgraph-static`, `-static-entry`, `-profile`, `-graphviz`, `-html`, `-files` | **`gen-callgraph KIND=static\|profile\|graphviz\|html\|files`** (+ `ENTRY=`) -- 6 collapse to 1. `gen-`, not `show-`: these write `callgraph*.mmd` / `.dot` / `.html` / `callgrind.out*` into the repo root (`clean` deletes them at Makefile 3066). Keep each recipe's trailing "saved to <path>" echo and any visualizer hint. |
| `icon-regen`, `icon-cube`, `icon-cube-strong` | `gen-icon ICON=retro-a\|cube\|cube-strong` -- 3 collapse to 1 |
| `rebuild-golden` | `gen-golden` |
| `distclean` | `clean-all` **(forwarder kept forever; the name predates this repo)** |
| `fetch-music` | `install-music` |
| `debug` | `all BUILD=debug` |
| `debug-msan` | **unchanged.** It is not merely `SAN=memory`: the recipe also sets `CC=$(MSAN_CC)` and skips with a warning on Darwin. Folding it into a `BUILD=`/`SAN=` spelling loses both -- the same oversimplification as `PLATFORM=web` vs `make web`. It stays a named wrapper, like `test-msan`. |

**Deleted (3):** `test-only` (alias), `test-no-checks` (-> `SKIP_CHECKS=1`),
`bench-glut-bitmap-build` (build-only member of a run family).

**Alias policy -- three buckets, not one.** An earlier draft had every rename
enter a `DEPRECATED_ALIASES` list "a follow-up commit empties", while §6 also
promised some names a forwarder forever. Those contradict: emptying the list
makes the grammar guard reject the permanent names. The buckets are:

| Bucket | Members | Guard treatment |
|---|---|---|
| **(a) Never renamed** | `test-stubs`, `test-web`, `web-serve`, `freeglut-clean`, `glprobe`, `glprobe-preload`, `debug-msan`, and the nine `*-demo` if that rename is dropped | canonical names; nothing to allow-list beyond `ROOT_TARGETS` |
| **(b) `FOREVER_ALIASES`** | `distclean`, `keymap-list`, `gl-tests` | renamed, but the old name is load-bearing in hooks/CI/muscle memory; allow-listed **permanently** |
| **(c) `DEPRECATED_ALIASES`** | the rest (`capacity-matrix`, `fetch-music`, `unicode-count`, ...) | allow-listed temporarily; a follow-up commit empties this list only |

Note (a) is the largest bucket and it needs no forwarder at all -- those names
were never in the rename table. Only (b) and (c) are forwarders.

### The two axes that become variables

#### Platform and GL backend are **two** axes, not one

An earlier draft proposed a single `PLATFORM=native|web|osmesa|stubs`. That is
wrong: it mixes *where the code runs* with *what GL it links*, and the existing
lanes already use combinations a single enum cannot express.

| Lane today | Runs on | GL linked |
|---|---|---|
| `make test` / `test-stubs` | native | stubs |
| `make test-web` | wasm/node | stubs (`WEB=1 USE_GL_STUBS=1`) |
| `make gl-tests` | native | system GL (real context) |
| `make gl-repl FREEGLUT_OSMESA=1` | native | OSMesa (headless, still native) |
| `make web` | wasm/browser | gl4es -> WebGL2 |

`test-web` is the proof: it needs `WEB=1` **and** `USE_GL_STUBS=1`
simultaneously. OSMesa is itself native. So:

```make
# Deliberately UNSET by default -- see below.
PLATFORM   ?=             #? Where it runs: native | web  (default: per-target)
GL_BACKEND ?=             #? What GL is linked: stubs | system | gl4es | osmesa
```

**Neither may carry a global default.** `USE_GL_STUBS ?=` is empty today, so a
global `GL_BACKEND ?= stubs` that mapped to `USE_GL_STUBS=1` would build
`gl-repl`, `app`, `glut` and `debug` against the no-op stubs -- not today's
contract. The sugar therefore applies **only when the variable is passed on the
command line** (`$(filter command line,$(origin GL_BACKEND))`); when unset,
every target keeps the flags it passes today. `test`/`test-stubs` already
forward `USE_GL_STUBS=1` into a recursive `make` and need no default at all.

Valid combinations (the guard rejects the rest with a usage line):

| `PLATFORM` | `GL_BACKEND` | Meaning | Replaces |
|---|---|---|---|
| `native` | `stubs` | headless, no GL libs | `test-stubs` default |
| `native` | `system` | real GL context | `gl-tests`, `make gl-repl` |
| `native` | `osmesa` | headless real rasterizer | `FREEGLUT_OSMESA=1` |
| `web` | `stubs` | wasm under node, no GL | `test-web`, `bench-web` |
| `web` | `gl4es` | wasm + gl4es -> WebGL2 | `make web` |
| `web` | `osmesa` | **invalid** (Makefile 118-119 says so explicitly) | -- |

Two further native backends do **not** fit the enum and must not be
forced into one: `FREEGLUT_VENDOR_LINUX=1` (vendored static freeglut on Linux,
required for capture) and `make glut` (`FREEGLUT_VENDOR=0`, Apple GLUT
framework). Adding `vendor-linux` and `apple-glut` values would conflate *which
GL* with *which freeglut*, repeating the mistake this section exists to fix.

**Therefore the raw flags stay public and supported.** `PLATFORM`/`GL_BACKEND`
are *sugar* that set `WEB` / `USE_GL_STUBS` / `FREEGLUT_OSMESA`; they name the
five valid combinations in the table above and nothing else. `FREEGLUT_VENDOR_LINUX`,
`FREEGLUT_LIB_PATH`, `FREEGLUT_VENDOR` and `make glut` keep working exactly as
today and are documented in `help-vars` alongside the sugar. If the sugar
cannot express a combination, the flags are the answer -- not a new enum value.

**These are global, parse-time flags -- there is no per-target backend.**
`USE_GL_STUBS` / `WEB` / `FREEGLUT_OSMESA` are consumed by `ifeq` at parse
time, so they bind once per `make` invocation. A target-specific
`gl-repl: GL_BACKEND = system` cannot reach those `ifeq`s. The only existing
per-goal mechanism is the `MAKECMDGOALS` filter at Makefile 336, already used
for `BUILD`.

So the model is: **`PLATFORM`/`GL_BACKEND` are global sugar that set the raw
flags; the named targets remain the dispatchers.** `test` is always stubbed,
`gl-tests` is always a real context, `web` is always gl4es. An earlier draft
promised `make test GL_BACKEND=system` == `gl-tests` and a different default
for `gl-repl` -- that is not expressible, and `make test gl-repl` in one
invocation cannot have two backends anyway. Retracted.

If a dispatcher is ever wanted, it must be written as an explicit `ifeq`
inside the recipe, with mixed goals documented as sharing one backend.

Also: `GL_BACKEND=system` under `PLATFORM=web` would mean gl4es->WebGL2, which
overloads "system" past usefulness. The web-real value is spelled **`gl4es`**.

**`test-stubs` is NOT renamed or removed.** A repo-wide scan (excluding
`docs/plans/`, see §5) finds **16 files** referencing `make test-stubs` --
both CI workflows, the pre-push hook, CLAUDE.md, four test sources, five
READMEs and one skill. It is the documented headless contract. It stays a first-class named target that
forwards `USE_GL_STUBS=1` recursively, exactly as it does today. The two-axis
sugar exists to make the *other* combinations expressible from the command
line, not to re-implement the common one.

`bench-web-gl4es` does not collapse -- it is a genuinely different benchmark
subject (browser draw path), so it becomes `bench-gl4es` and asserts
`PLATFORM=web GL_BACKEND=gl4es`.

#### Report format (`FORMAT`)

Scope is the **bench family only**. `gen-callgraph` picks its output shape with
`KIND=` (mermaid/dot/html), which is a different axis with no CSV member;
`show-*` targets print one human table. Widening `FORMAT` to "bench/gen" would
promise `gen-callgraph FORMAT=csv`, which means nothing.

```make
FORMAT ?= human           #? Report format for bench/show targets: human | csv
```

`make bench FORMAT=csv` replaces `bench-csv`; the same line covers
`bench-render` and `bench PLATFORM=web`. Three targets disappear and the axis
becomes available to every current and future bench.

#### Check skipping (`SKIP_CHECKS`)

`test-no-checks` duplicates the `test-stubs` recipe purely to drop one
prerequisite. Make can express that directly, with no second recipe:

```make
test-stubs: $(if $(filter 1,$(SKIP_CHECKS)),,check)
```

`make test SKIP_CHECKS=1` replaces `test-no-checks`; `test-only` (its alias)
and `test-no-checks` both delete.

### Argument conventions (a rule, not a per-target choice)

- `ARGS` is the **only** passthrough for a target that executes something.
  Every `run-*` and `bench-*` honours it. The demo targets do **not**: they only
  link and `ln -sfn` a binary (`repl-demo`, Makefile 1804-1805), so there is
  nothing to pass arguments to. A target that runs a binary
  and does not honour `ARGS` is a bug.
- A target that needs a **required** operand takes it in one variable named
  for the operand (`SAMPLE=`, `ENTRY=`, `TEST_CASE=`), and errors with a usage
  line when it is empty. Several today just fail obscurely.
- **Keep the alias spellings.** An earlier draft proposed dropping
  `NOSAN`/`ASAN=0` and `VERBOSE`. That was wrong: `ASAN=0` is deliberately
  documented as the positive-polarity spelling of `NO_SAN=1` (Makefile
  276-278), `BUILD_REPORT_PARAMS` (364-368) goes out of its way to echo back
  *whichever spelling the caller used*, and `V`/`VERBOSE` is the kbuild
  convention already collapsed at its single use site. Deleting them breaks
  muscle memory and outer scripts for no new capability. `help-vars` names one
  canonical spelling per switch and lists the others as accepted.
- Document both passthrough channels together: `ARGS` (18 sites, every `run-*`
  and `bench-*`) and the four per-bench `*_BENCH_ARGS` defaults.

## 3. Peeling the onion: three-tier help

`help-details` is unmaintainable because it is one flat sorted dump plus 90
lines of hand-written prose about variables. Replace with:

**Tier 1 -- `make help`** (unchanged in spirit, ~15 lines): the ten things a
newcomer runs.

**Tier 2 -- `make help-<verb>`**: **one generic pattern rule**, not one target
per verb -- zero maintenance as verbs are added:

```make
# NOT .PHONY-able: .PHONY has no effect on pattern rules, so a file named
# `help-check` in the repo root would make Make report "up to date" and skip
# the recipe (verified). FORCE (already defined at Makefile 582) is the fix.
help-%: FORCE ## Show targets for one verb, e.g. help-check, help-bench, help-show.
	@printf "Targets: %s-*\n\n" "$*"
	@awk -F':.*## ' -v p="$*" '$$1 ~ ("^" p "(-|$$)") {printf "  %-28s %s\n", $$1, $$2}' \
		$(MAKEFILE_LIST) | sort
```

`make help-check` finally makes the 92 guards discoverable; `make help-bench`, `make help-test`, `make help-show` likewise.
This is the actual onion-peel: nobody needs all 185 at once, they need one
verb's worth.

**Tier 3 -- `make help-vars`**: the variable reference, generated the same way
`## ` generates the target list. Give each variable a declaration-site comment
and scrape it:

```make
BUILD ?= release        #? Build mode: release | quick | debug | coverage
PLATFORM   ?= native    #? Where it runs: native | web
GL_BACKEND ?= stubs     #? What GL is linked: stubs | system | gl4es | osmesa
FORMAT     ?= human     #? Report format for bench targets: human | csv
SKIP_CHECKS ?=          #? 1 skips the `check` prereq on test lanes
NO_SAN     ?=           #? 1 disables debug sanitizers (accepted: NOSAN=1, ASAN=0)
ARGS ?=                 #? Extra arguments passed to the executed binary
```

That deletes ~70 lines of printf prose from `help-details` and, more
importantly, puts the documentation **at the definition**, where it goes stale
visibly. The long-form notes that survive (freeglut vendoring, GLR_* runtime
env, UI_THEME_DEFAULT) are runtime/build *documentation*, not Make interface,
and belong in `docs/ADVANCED_USAGE.md` with `help-details` linking to it --
which is exactly the split CLAUDE.md already applies to itself.

`make help-details` stays as the everything dump for grep, but generated: verb
sections, no hand-maintained prose.

## 4. Guards

Six new checks, all in `scripts/check/`. They follow `check-completions` only
in *spirit* -- "the docs and the thing must agree", enforced by scraping.
`check-completions` compares `gl-repl --help` against the zsh completion files
and has nothing to do with Make target names, so it is a stylistic precedent,
not a code one.

**Each guard must state which universe it scrapes** (source text vs
`make -pnR`). The 258 generated `test-*`/`run-test-*` aliases are invisible to
a source scraper, so every guard below is scoped to **source declarations**
and explicitly exempts `internal-*`, `FORCE`, `.PHONY` helpers, `ifeq`-`else`
error stubs, and generated pattern aliases.

### 4.1 `check-make-target-grammar`

Every target with a recipe must match one of exactly five cases:

```
bare verb | <verb>-* | ROOT_TARGETS | FOREVER_ALIASES | DEPRECATED_ALIASES
```

`ROOT_TARGETS` is the full artifact/never-renamed set -- **24 names**, not
nine:

```make
ROOT_TARGETS := all gl-repl gl-repl-unchained render3d-asset-builder app web \
                demos glut FORCE \
                web-serve freeglut-clean debug-msan glprobe glprobe-preload \
                render3d-demo render3d-hot repl-demo repl-live-demo \
                editor-demo memprof-demo cpuprof-demo variable-panel-demo \
                color-picker-demo assign-plot-demo
```

The second block is bucket (a) from §2 -- never renamed, so never a forwarder.
`web-serve`, `freeglut-clean` and `debug-msan` are there precisely because
`serve-`, `freeglut-` and `debug-` are not verbs and these names are staying. The inventory above is the proof that this closes today
with an empty residual; the guard keeps it closed.

### 4.2 `check-make-target-documented`

Every non-`internal-*` source-declared target carries a `## ` on at least one
declaration. Only **two** fail today (`all`, `require-emcc`) -- the guard is
cheap to land because the tree is already almost conformant. This
is the guard that lets generated help *replace* the hand-written one.

### 4.3 `check-make-no-duplicate-help`

**Not** a duplicate-*target* guard. As P1 established, a target legitimately
declares once per arm of a platform conditional, and a naive scraper cannot
tell that from copy-paste. What is never legitimate is the same target name
carrying a `## ` doc more than once -- that is precisely the defect (help
prints twice), it is platform-independent, and it is greppable without
evaluating the Makefile. The whole guard is one line -- fail if it prints
anything:

```sh
dupes=$(awk -F':.*## ' '/^[a-zA-Z0-9_.-]+:.*## /{print $1}' Makefile | sort | uniq -d)
[ -z "$dupes" ] || { printf 'target documented more than once:\n%s\n' "$dupes" >&2; exit 1; }
```

**The exit status must be taken from the capture, not the pipeline.**
`... | uniq -d` exits 0 whether or not it printed anything, so the bare
pipeline is a guard that can never fail -- verified. Same trap applies to any
`grep`-less scraper in 4.1/4.2/4.5.

`bench-code-panel-text` is the in-repo proof this works: it is declared under
`ifneq ($(filter Linux Darwin,$(UNAME_S)),)` with a supported arm and an error
arm, and carries exactly **one** `## `. The fix for the other two is to adopt
its shape -- hoist the run rule and its single doc out of the conditional,
branching only the build rules:

```make
ifeq ($(UNAME_S),Darwin)
  $(VERTEX_LABEL_BENCH_BIN): ...        # Darwin link line
  VERTEX_LABEL_BENCH_OK := 1
else ifeq ($(UNAME_S),Linux)
  $(VERTEX_LABEL_BENCH_BIN): ...        # Linux link line
  VERTEX_LABEL_BENCH_OK := 1
endif

bench-vertex-labels: $(if $(VERTEX_LABEL_BENCH_OK),$(VERTEX_LABEL_BENCH_BIN)) ## Benchmark ...
ifdef VERTEX_LABEL_BENCH_OK
	$(VERTEX_LABEL_BENCH_BIN) $(VERTEX_LABEL_BENCH_ARGS) $(ARGS)
else
	@echo "ERROR: bench-vertex-labels targets Linux/macOS (needs freeglut)." >&2; exit 1
endif
```

One declaration, one doc, all three platform behaviours preserved.

A genuine duplicate-*recipe* guard is still worth having, but it must read
Make's **evaluated** database (`make -pnR` on the current host), not the source
text -- and it therefore only proves the invariant for the platform CI runs on.
Both CI lanes (Linux and macOS) already exist, so running it in each covers the
two arms that matter. Documented as a limitation rather than pretended away.

### 4.4 `check-make-phony-derived`

**The scrape replaces the hand-maintained lists only for documented source
targets. It must NOT be the whole `.PHONY`.** `TEST_TARGET_NAMES`,
`RUN_TEST_TARGETS` and `RUN_TEST_FILE_TARGETS` expand to **86 names each --
258 generated aliases** that no source scraper can see, plus five
`internal-*` procedural targets that carry no `## `. Dropping them from
`.PHONY` means a stray file named `test-eval` or `run-test-eval` in the repo
root silently satisfies the target. That hazard is why the hand lists exist;
it is not only about `check-*`.

```make
PHONY_TARGETS := $(sort $(shell awk -F: '/^[a-zA-Z0-9_.-]+:.*## / {print $$1}' \
                                 $(firstword $(MAKEFILE_LIST))))
.PHONY: $(PHONY_TARGETS) $(ROOT_TARGETS) $(FOREVER_ALIASES) $(DEPRECATED_ALIASES) \
        $(TEST_TARGET_NAMES) $(RUN_TEST_TARGETS) $(RUN_TEST_FILE_TARGETS) \
        $(BENCH_TARGET_NAMES) $(INTERNAL_TARGETS)
```

The generated and internal families stay **explicit**, next to the scrape.
What the scrape kills is the five hand-copied inventories of *documented* names
(`BUILD_TARGETS`, `PACKAGE_TARGETS`, ...), which is where the drift actually is.

Guard form: fail if a **documented source** target is missing from `.PHONY`.
It must not fail on a generated alias, which it cannot enumerate, and it must
exempt **pattern rules** (`help-%`): `.PHONY` does not apply to them, so they
carry a `FORCE` prerequisite instead (see §3). A guard that demanded `help-%`
be phony would be demanding something Make cannot honour.

### 4.5 `check-make-fix-pairs-check`

Every `fix-X` has a `check-X`. Two pairs exist today (`doc-links`, `unicode`)
and both conform; the guard keeps `fix-` from becoming a junk drawer.

### 4.6 `check-make-var-documented`

**`#?` is opt-in, not universal.** The Makefile has **58** top-level `?=`
declarations, of which **26** are generated `test_*_RUN` runner definitions,
plus `SANITIZER_CHECKERS`, `ANALYZE_EXCLUDE`, `GLUT_BITMAP_BENCH_ARCH`,
`EXTRA_LDFLAGS`, `DEBUG_INFO_CFLAGS`, `GL4ES_DIR` and similar internals.
Requiring `#?` on all of them would rebuild, in `help-vars`, exactly the
70-line dump this plan exists to delete.

Instead, declare the public interface explicitly:

```make
PUBLIC_MAKE_VARS := BUILD SAN NO_SAN ARGS V TEST_JOBS FORMAT SKIP_CHECKS \
                    PLATFORM GL_BACKEND SAMPLE ENTRY TEST_CASE ...
```

Guard form: every name in `PUBLIC_MAKE_VARS` has a `#?` comment at its
declaration, and `help-vars` renders exactly that set. Private variables
(`*_RUN`, `*_CFLAGS`, `*_DIR`) stay undocumented by design.

All six join `CHECK_TARGETS` so `make check` runs them.

## 5. Blast radius

Recounted with the correct universe -- an earlier draft's "95 files" for
`test-stubs` counted `docs/plans/done/`, which is 86 historical records, not
callers:

```sh
rg --hidden -l -g '!docs/plans/**' -g '!.git/**' 'make[^\n]*\btest-stubs\b' .
```

| Target | Files | Notes |
|---|---|---|
| `test-stubs` | **16** | both CI workflows, pre-push hook, CLAUDE.md, 4 test sources, 5 READMEs, 1 skill |
| `gl-tests` | 11 | |
| `test-web` | 7 | |
| `web-serve` | 6 | |
| `keymap-list`, `freeglut-clean`, `fetch-music` | 5 each | |
| `config-list` | 4 | |
| `rebuild-golden` | 3 | |
| `distclean` | 0 | free to rename; kept as a `FOREVER_ALIAS` on name-age grounds only |

Every row above is measured with the command below, not the earlier
`docs/plans`-polluted universe.

`coverage`'s earlier "1 file" was the opposite error -- the bare string matches
~50 non-target hits (`BUILD=coverage`, lcov paths), so it needs a
target-anchored recount before step 6.

**16, not 95, is still decisive**: it includes both CI workflows and the
pre-push hook. But the earlier claim that "all four `.claude/skills/`"
reference it was wrong -- only `gl-repl-new-command` does.

Callers to update on any rename, in dependency order:

0. **`Makefile` itself, line 336** -- the `MAKECMDGOALS` filter hard-codes
   `test test-detailed test-stubs test-no-checks test-only test-msan test-full
   test-scenes` to force `BUILD=debug`. Renaming, adding or deleting a test
   lane silently changes its build mode unless this list is updated. It
   already omits `test-web` (which runs release wasm deliberately) -- so the
   list is a hand-maintained inventory of exactly the kind this plan is trying
   to eliminate, and step 4 must touch it.
1. `.githooks/pre-push` -- `check-trailing-whitespace`, `test-stubs BUILD=quick`.
2. `.github/workflows/ci-linux.yml`, `ci-macos.yml` -- `check-c99`,
   `test-stubs`, `gl-repl`.
3. `.claude/skills/` -- `gl-repl-new-command` (`test-stubs`),
   `gl-repl-config-toggle` (`config-list`, `keymap-list`), `gl-repl-capture`
   (`FREEGLUT_OSMESA=1`, `fetch-music`).
4. `CLAUDE.md` / `AGENTS.md` build+test tables, `docs/CONTRIBUTING.md`,
   `docs/RELEASE.md`, `docs/ADVANCED_USAGE.md`, `tests/README.md`,
   `packaging/web/README.md`.
5. `scripts/completions/` -- **only if** a Make-target completion file is ever
   added. `check-completions` compares `gl-repl --help` against the zsh
   completions and has nothing to say about Make target names, so it is not
   currently in this blast radius.

The 95-file `test-stubs` count is the reason 2.x keeps it as a first-class
name rather than folding it into `PLATFORM`/`GL_BACKEND`.

## 6. Suggested sequencing

**The help work is independent of the grammar and ships first.** Generated
help and derived `.PHONY` work on today's names and need no renames.
**Steps 1-3 are invocation-neutral; step 4 is not** -- it deletes five public
names (`bench-csv`, `bench-render-csv`, `bench-web-csv`, `test-no-checks`,
`test-only`). Either it keeps `.PHONY` forwarders for those five, or it joins
the announced cut with 5-6. It is listed at 4 because it is small and
self-contained, not because it is free. The grammar is a *later, optional*
pass -- if steps 1-4 land and the tree reads fine, step 5-6 may simply not be
worth the ~150 doc/CI reference updates.

1. **Help-metadata dedupe.** Adopt `bench-code-panel-text`'s shape in
   `bench-code-panel-stencil` and `bench-vertex-labels` (hoist run rule + one
   `## ` above the conditional, per 4.3). Add `## ` to `all` and
   `require-emcc`. Land `check-make-no-duplicate-help` and
   `check-make-target-documented`. **No target deleted, no `ifeq` arm touched,
   no invocation changed.** Verify on macOS *and* Linux.
2. **Generated help.** `help-%` pattern rule, `help-vars`, `#?` variable
   annotations; move runtime-env and freeglut prose to
   `docs/ADVANCED_USAGE.md`. Land `check-make-var-documented`. This is where
   the 182 lines actually shrink, and it is the step with the most value per
   unit of risk.
3. **Derive `.PHONY`.** Replace the five hand-maintained `*_TARGETS` lists with
   the single-pass scrape. Land `check-make-phony-derived`. Pure deletion.
4. **`FORMAT` + `SKIP_CHECKS`** *(first step that changes public names)*.
   Delete `bench-csv`, `bench-render-csv`, `bench-web-csv`; convert
   `test-no-checks`/`test-only` to `SKIP_CHECKS=1` **in the same commit** that
   removes them; keep `.PHONY` forwarders for all five unless this step is
   deferred into the announced cut. Update the `MAKECMDGOALS` filter at
   Makefile 336, which names `test-no-checks` and `test-only` by hand.
5. *(optional)* **`PLATFORM`/`GL_BACKEND` sugar.** Additive only: the raw flags
   keep working, `test-stubs`/`test-web`/`glut` keep their names. Update the
   `MAKECMDGOALS` filter (which omits `test-web` today) and CI in the same
   commit. `test-full` stays a named meta-lane and is
   **not** a `PLATFORM`x`GL_BACKEND` cell -- it sequences demos, sanitized
   stubs, scenes, msan, `gl-repl`, `gl-tests`, `bench` and `glut`, each with
   its own backend.
6. *(optional)* **Verb renames + grammar guard**, with `DEPRECATED_ALIASES`
   forwarders.

### Names that keep a forwarder permanently

Exactly bucket (b) from §2 -- three names, renamed but with the old spelling
allow-listed forever in `FOREVER_ALIASES`:

`gl-tests` (11 referencing files), `keymap-list` (5), `distclean` (0 files,
kept on name-age grounds alone).

Everything else that "must not break" is bucket (a): `test-stubs`, `test-web`,
`web-serve`, `freeglut-clean`, `debug-msan`, `glprobe*` and the nine demos are
**never renamed**, so they need no forwarder at all. Conflating the two is the
contradiction the three-bucket table exists to prevent.

## 7. What deliberately does not change

- `check-*` stays 92 targets. They are a guard *inventory*, and the fix for
  their bulk is `make help-check` plus keeping them out of the default help --
  not merging them. Merging would cost the per-guard failure message, which is
  the whole value.
- `test-stubs` and `test-web` keep their names permanently (16 and 7
  referencing files). The sugar expresses combinations; it does not rename the
  common lanes.
- **The `*-demo` -> `demo-*` rename is dropped.** Two independent reasons:
  `repl-demo` matches `tools/repl_demo/` and the on-disk binary (the same
  artifact-named exemption `gl-repl` gets), and a `demo-` *verb* would have to
  mean build-and-run for `ARGS` to apply -- which would make `make demo-repl`
  launch an interactive window and, worse, make `test-full` do so when it
  builds `HEADLESS_DEMO_TARGETS` as compile proofs. The nine names join
  `ROOT_TARGETS` and nothing changes.
- `gl-repl` keeps its name. It is the product.
- `BUILD` keeps its name and values.
- `internal-*` stays the marker for "implementation, do not type this".
