# Makefile target conventions

**Status:** proposal. Nothing implemented.

`make help-details` is 182 lines and hides all 92 `check-*` targets. The
problem is not the count -- it is that `help-details` is hand-maintained prose
over a target set with no grammar, so it cannot be generated and a reader
cannot predict a name.

**The two halves of this plan are independent and should be judged
separately.** Generated help (§3) and the mechanical cleanups (§6 steps 1-4)
work on today's names, change no invocation, and carry most of the value. The
naming grammar (§2) is a larger, optional second pass with a ~150-file
reference blast radius.

## 1. The inventory today

All counts below are reproduced by `scripts/make-target-inventory.sh` (checked
in with this plan). **Two universes exist and a guard must declare which it
scrapes:**

- **source** -- names literally declared in the Makefile text: 192
  declarations, **185 unique names**. This is what awk/grep see.
- **evaluated** -- what Make knows after `$(eval)`: the `built_test_binary`
  /`RUN_TEST_TARGETS` machinery generates **267** further `test-*` / `run-test-*`
  aliases from 86 `TEST_BINS`. A source scraper cannot see these; only
  `make -pnR` can.

`make help-details` is 182 lines and scrapes only the source universe, minus
the 92 `check-*` targets it filters out (`$$1 !~ /^check-/`).

Prefix distribution of the documented source set:

| Prefix | Count | Nature |
|---|---|---|
| `check-` | 92 | guards (the `check` aggregator is separate) |
| `bench-` | 16 | benchmarks |
| `test-` | 10 | hand-written lanes (+267 generated aliases) |
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
| `demo-` | build a standalone feature demo | build failure |
| `test-` | a test lane | test failure |
| `bench-` | a benchmark lane | run failure |
| `check-` | a guard: findings **are** violations | non-zero on any finding |
| `fix-` | the mutating twin of a `check-` (must pair 1:1) | non-zero on failure to fix |
| `show-` | print a report; never mutates project state | see below |
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

**Renamed (43):**

| Before | After |
|---|---|
| `repl-demo`, `repl-live-demo`, `editor-demo`, `render3d-demo`, `render3d-hot`, `memprof-demo`, `cpuprof-demo`, `variable-panel-demo`, `color-picker-demo`, `assign-plot-demo` | `demo-repl`, `demo-repl-live`, `demo-editor`, `demo-render3d`, `demo-render3d-hot`, `demo-memprof`, `demo-cpuprof`, `demo-variable-panel`, `demo-color-picker`, `demo-assign-plot` |
| `render3d-hot-lib` | `internal-render3d-hot-lib` (its doc already says "invoked by the running hot host") |
| `require-emcc` | `internal-require-emcc` |
| `gl-tests` | `test-gl` |
| `coverage` | `test-coverage` (it runs the suite; the report is a side effect) |
| `extract`, `glprobe`, `glprobe-preload` | `run-extract`, `run-glprobe`, `run-glprobe-preload` |
| `keymap-list`, `config-list`, `palette-list` | `show-keymap`, `show-config`, `show-palette` |
| `capacity-matrix`, `unicode-count` | `show-capacity`, `show-unicode` |
| `lines`, `lines-test` | `show-lines`, `show-lines SCOPE=test` |
| `find-trailing-whitespace` | `show-trailing-whitespace` |
| `audit-editor-ownership` | `show-editor-ownership` |
| `analyze` | `show-analyze` |
| `callgraph-static`, `-static-entry`, `-profile`, `-graphviz`, `-html`, `-files` | `show-callgraph KIND=static\|profile\|graphviz\|html\|files` (+ `ENTRY=`) -- 6 targets collapse to 1 |
| `icon-regen`, `icon-cube`, `icon-cube-strong` | `gen-icon ICON=retro-a\|cube\|cube-strong` -- 3 collapse to 1 |
| `rebuild-golden` | `gen-golden` |
| `distclean` | `clean-all` **(forwarder kept forever; the name predates this repo)** |
| `fetch-music` | `install-music` |
| `debug`, `debug-msan` | `all BUILD=debug`, `all BUILD=debug SAN=memory` |

**Deleted (3):** `test-only` (alias), `test-no-checks` (-> `SKIP_CHECKS=1`),
`bench-glut-bitmap-build` (build-only member of a run family).

**Deprecation.** Every rename keeps a `.PHONY` forwarder listed in a
`DEPRECATED_ALIASES` variable. `check-make-target-grammar` allow-lists exactly
that list and nothing else, so the forwarders are visibly temporary rather than
silently permanent, and CI does not fail during the transition. A follow-up
commit empties the list.

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
PLATFORM   ?= native      #? Where it runs: native | web
GL_BACKEND ?= stubs       #? What GL is linked: stubs | system | osmesa
```

Valid combinations (the guard rejects the rest with a usage line):

| `PLATFORM` | `GL_BACKEND` | Meaning | Replaces |
|---|---|---|---|
| `native` | `stubs` | headless, no GL libs | `test-stubs` default |
| `native` | `system` | real GL context | `gl-tests`, `make gl-repl` |
| `native` | `osmesa` | headless real rasterizer | `FREEGLUT_OSMESA=1` |
| `web` | `stubs` | wasm under node, no GL | `test-web`, `bench-web` |
| `web` | `system` | wasm + gl4es -> WebGL2 | `make web` |
| `web` | `osmesa` | **invalid** (Makefile 118-119 says so explicitly) | -- |

Two further native backends do **not** fit a three-value enum and must not be
forced into one: `FREEGLUT_VENDOR_LINUX=1` (vendored static freeglut on Linux,
required for capture) and `make glut` (`FREEGLUT_VENDOR=0`, Apple GLUT
framework). Adding `vendor-linux` and `apple-glut` values would conflate *which
GL* with *which freeglut*, repeating the mistake this section exists to fix.

**Therefore the raw flags stay public and supported.** `PLATFORM`/`GL_BACKEND`
are *sugar* that set `WEB` / `USE_GL_STUBS` / `FREEGLUT_OSMESA`; they name the
three common combinations and nothing else. `FREEGLUT_VENDOR_LINUX`,
`FREEGLUT_LIB_PATH`, `FREEGLUT_VENDOR` and `make glut` keep working exactly as
today and are documented in `help-vars` alongside the sugar. If the sugar
cannot express a combination, the flags are the answer -- not a new enum value.

**Defaults are chosen so no current invocation changes meaning.** `make test`
stays the stubbed headless gate, period -- its default is
`GL_BACKEND=stubs`, not `system`, because that *is* the contract 95 files
depend on. `make test GL_BACKEND=system` is `gl-tests` (the small display-
requiring opt-in set), **not** "the whole suite against a window". `make
gl-repl` defaults `GL_BACKEND=system` in its own rule, because building the
app against stubs is the unusual case.

That per-target default is deliberate and is the one place the axis is not
uniform. The alternative -- one global default -- would silently change either
`make test` or `make gl-repl`.

**`test-stubs` is NOT renamed or removed.** A repo-wide scan finds **95 files**
referencing `make test-stubs` (CI, the pre-push hook, CLAUDE.md, all four
`.claude/skills/`, docs). It is the documented headless contract. It stays a
first-class named target that happens to be spelled
`test PLATFORM=native GL_BACKEND=stubs` internally. The two-axis model exists
to make the *other* combinations expressible, not to rename the common one.

`bench-web-gl4es` does not collapse -- it is a genuinely different benchmark
subject (browser draw path), so it becomes `bench-gl4es` and asserts
`PLATFORM=web GL_BACKEND=system`.

#### Report format (`FORMAT`)

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
  Every `run-*`, `demo-*` and `bench-*` honours it. A target that runs a binary
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
- Document both passthrough channels together: `ARGS` (18 sites, every `run-*`,
  `demo-*` and `bench-*`) and the four per-bench `*_BENCH_ARGS` defaults.

## 3. Peeling the onion: three-tier help

`help-details` is unmaintainable because it is one flat sorted dump plus 90
lines of hand-written prose about variables. Replace with:

**Tier 1 -- `make help`** (unchanged in spirit, ~15 lines): the ten things a
newcomer runs.

**Tier 2 -- `make help-<verb>`**: **one generic pattern rule**, not one target
per verb -- zero maintenance as verbs are added:

```make
help-%: ## Show targets for one verb, e.g. help-check, help-bench, help-show.
	@printf "Targets: %s-*\n\n" "$*"
	@awk -F':.*## ' -v p="$*" '$$1 ~ ("^" p "(-|$$)") {printf "  %-28s %s\n", $$1, $$2}' \
		$(MAKEFILE_LIST) | sort
``` `make help-check` finally makes the 92 guards
discoverable; `make help-bench`, `make help-test`, `make help-show` likewise.
This is the actual onion-peel: nobody needs all 185 at once, they need one
verb's worth.

**Tier 3 -- `make help-vars`**: the variable reference, generated the same way
`## ` generates the target list. Give each variable a declaration-site comment
and scrape it:

```make
BUILD ?= release        #? Build mode: release | quick | debug | coverage
PLATFORM ?= native      #? Where it runs: native | web | osmesa | stubs
FORMAT ?= human         #? Report format for bench/show targets: human | csv
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
`make -pnR`). The 267 generated `test-*`/`run-test-*` aliases are invisible to
a source scraper, so every guard below is scoped to **source declarations**
and explicitly exempts `internal-*`, `FORCE`, `.PHONY` helpers, `ifeq`-`else`
error stubs, and generated pattern aliases.

### 4.1 `check-make-target-grammar`

Every target with a recipe must be a bare verb, start with a verb from the
verb set, be in `ROOT_TARGETS` (the nine artifact-named roots), or be in
`DEPRECATED_ALIASES`. The inventory above is the proof that this closes today
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
evaluating the Makefile.

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

Assert the five hand-maintained `*_TARGETS` lists are gone, replaced by one
single-pass scrape (five separate `$(shell awk ...)` calls at parse time would
tax every `make` invocation):

```make
PHONY_TARGETS := $(sort $(shell awk -F: '/^[a-zA-Z0-9_.-]+:.*## / {print $$1}' \
                                 $(firstword $(MAKEFILE_LIST))))
.PHONY: $(PHONY_TARGETS) $(ROOT_TARGETS) $(DEPRECATED_ALIASES)
```

Guard form: fail if a documented target is missing from `.PHONY`.

### 4.5 `check-make-fix-pairs-check`

Every `fix-X` has a `check-X`. Two pairs exist today (`doc-links`, `unicode`)
and both conform; the guard keeps `fix-` from becoming a junk drawer.

### 4.6 `check-make-var-documented`

Every `?=` variable at the top level carries a `#?` comment, so `help-vars`
is complete by construction. This is what makes Tier 3 trustworthy.

All six join `CHECK_TARGETS` so `make check` runs them.

## 5. Blast radius

Repo-wide scan for `make <target>` outside `docs/plans/`, by referencing file
count. This is what makes forwarders non-negotiable for the renames:

| Target | Files | Notes |
|---|---|---|
| `test-stubs` | **95** | CI, pre-push hook, CLAUDE.md, all four skills, docs |
| `test-web` | 12 | |
| `gl-tests` | 10 | |
| `keymap-list`, `web-serve` | 8 each | |
| `freeglut-clean` | 7 | |
| `rebuild-golden` | 6 | |
| `config-list`, `fetch-music` | 4 each | |
| `debug-msan` | 3 | |
| `coverage` | 1 | |
| `distclean`, `lines` | 0 | free to rename |

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
3. `.claude/skills/` -- `gl-repl-config-toggle` (`config-list`, `keymap-list`),
   `gl-repl-new-command` (`test-stubs`), `gl-repl-capture` (`FREEGLUT_OSMESA=1`,
   `fetch-music`).
4. `CLAUDE.md` / `AGENTS.md` build+test tables, `docs/CONTRIBUTING.md`,
   `docs/RELEASE.md`, `docs/ADVANCED_USAGE.md`, `tests/README.md`,
   `packaging/web/README.md`.
5. `scripts/completions/` -- and `check-completions` must be re-run, since it
   asserts the completion set matches the documented options.

The 95-file `test-stubs` count is the reason 2.x keeps it as a first-class
name rather than folding it into `PLATFORM`/`GL_BACKEND`.

## 6. Suggested sequencing

**The help work is independent of the grammar and ships first.** Generated
help, derived `.PHONY` and the `FORMAT` collapse all work on today's names,
need no renames and no closed verb set. The grammar is a *later, optional*
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
4. **`FORMAT` + `SKIP_CHECKS`.** Delete `bench-csv`, `bench-render-csv`,
   `bench-web-csv`; convert `test-no-checks`/`test-only` to `SKIP_CHECKS=1`
   **in the same commit** that removes them, and update the `MAKECMDGOALS`
   filter at Makefile 336 which names both by hand.
5. *(optional)* **`PLATFORM`/`GL_BACKEND` sugar.** Additive only: the raw flags
   keep working, `test-stubs`/`test-web`/`glut` keep their names. Update the
   `MAKECMDGOALS` filter (which omits `test-web` today) and CI in the same
   commit. Do not land this until `test-full` and `bench-web-gl4es` are
   written into the model.
6. *(optional)* **Verb renames + grammar guard**, with `DEPRECATED_ALIASES`
   forwarders.

### Names that keep a forwarder permanently

Not "for one release" -- indefinitely. These are load-bearing in CI, hooks,
skills and muscle memory: `test-stubs` (95 files), `test-web` (12),
`gl-tests` (10), `keymap-list` (8), `web-serve` (8), `freeglut-clean` (7),
`distclean` (0 files, but the name predates this repo).

## 7. What deliberately does not change

- `check-*` stays 92 targets. They are a guard *inventory*, and the fix for
  their bulk is `make help-check` plus keeping them out of the default help --
  not merging them. Merging would cost the per-guard failure message, which is
  the whole value.
- `test-stubs` and `test-web` keep their names permanently (95 and 12
  referencing files). The sugar expresses combinations; it does not rename the
  common lanes.
- The `*-demo` -> `demo-*` rename is the **weakest** item in step 6 and may be
  dropped. `repl-demo` matches `tools/repl_demo/` and the on-disk binary name,
  which is the same artifact-named-target exemption `gl-repl` already gets.
  Consistency with the directory may be worth more than consistency with the
  verb table.
- `gl-repl` keeps its name. It is the product.
- `BUILD` keeps its name and values.
- `internal-*` stays the marker for "implementation, do not type this".
