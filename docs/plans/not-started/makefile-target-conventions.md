# Makefile target conventions

**Status:** proposal. Nothing implemented.

`make help-details` is 182 lines and hides 92 of the 177 targets. The problem
is not the count -- it is that the target set has no grammar, so a reader
cannot predict a name and `help-details` has to spell every one of them out by
hand. This plan proposes the grammar, the mechanical cleanups that make the
tree conform, and the guards that keep it conforming.

## 1. The inventory today

177 targets with recipes. 85 carry a `## ` doc comment and reach
`help-details`; 92 are `check-*` and are filtered out of it entirely
(`$$1 !~ /^check-/`). Twelve targets have recipes and no doc at all
(`internal-*`, `require-emcc`, `demos`, `all`, `gl-repl`, three duplicate
`bench-*` stanzas).

Prefix distribution of the documented set:

| Prefix | Count | Nature |
|---|---|---|
| `check-` | 93 | guards |
| `bench-` | 16 | benchmarks |
| `test-` | 10 | test lanes |
| `callgraph-` | 6 | analysis |
| `*-demo` | 8 | **suffix**, not prefix |
| `release-`, `icon-`, `install-`, `lines`, `fix-`, `audit-`, ... | ~25 | one-offs |

### The four axes the names conflate

The user-visible confusion is that four independent axes are all spelled as
name segments, in inconsistent positions:

1. **What it does** -- build / run / test / bench / check / report.
2. **What it does it to** -- the app, a demo, a subsystem.
3. **Where it runs** -- native, `web` (wasm), `osmesa`, real-GL.
4. **How it reports** -- human table vs `--csv`.

Axis 3 appears as a prefix in `web`, `web-serve` and as a suffix in
`test-web`, `bench-web`, `bench-web-gl4es`. Axis 4 appears as a suffix in
`bench-csv`, `bench-render-csv`, `bench-web-csv` -- three targets that exist
only to pass one flag. Axis 2 is a prefix for `bench-*` and `check-*` but a
suffix for `*-demo`.

### Concrete drift found while surveying

- `bench-code-panel-stencil` and `bench-vertex-labels` are each declared
  **twice** with identical `## ` docs (lines 2758/2775 and 2807/2815), so each
  prints twice in `help-details`.
- `test-only` exists solely as an alias for `test-no-checks`; `test-no-checks`
  is a copy-paste of the `test-stubs` recipe minus the `check` prereq.
- `test-stubs` is the real lane; `test` is a one-line forwarder to it. Both are
  documented as if they were peers.
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

Variables consumed by targets, by kind:

| Kind | Variables |
|---|---|
| Build mode | `BUILD`, `SAN`, `NO_SAN` (+ `NOSAN`, `ASAN` aliases), `CC`, `CFLAGS` |
| Link/backend | `FREEGLUT_OSMESA`, `FREEGLUT_VENDOR_LINUX`, `FREEGLUT_LIB_PATH`, `FREEGLUT_INCLUDE_DIR`, `USE_GL_STUBS`, `WEB` |
| Target selection | `SAMPLE`, `ENTRY`, `TEST_CASE`, `EXAMPLES_CATALOG` |
| Run-time passthrough | `ARGS`, `TEST_JOBS`, `TEST_VERBOSE`, `V`/`VERBOSE` |
| Destinations | `MUSIC_DEST`, `ZSHRC`, `CALLGRAPH_FILES_GROUP_CONFIG` |

Three problems: `NO_SAN`/`NOSAN`/`ASAN=0` are three spellings of one switch;
`V`/`VERBOSE` are two spellings of another; and `ARGS` is honoured by 8
targets but documented for exactly one family (`run-test-*`) in a printf at
the bottom of `help-details`.

## 2. Proposed grammar

> **A target name is `<verb>-<subject>[-<subject-qualifier>]`. Axes 3 and 4 --
> where it runs and how it reports -- are never name segments; they are
> variables.**

### Verbs (closed set)

| Verb | Meaning | Notes |
|---|---|---|
| *(none)* | build the named artifact | `gl-repl`, `web`, `app`, `demos` |
| `run-` | build **and execute** | `run-test-eval`, `run-gl-repl` |
| `test-` | a test lane | |
| `bench-` | a benchmark lane | |
| `check-` | a guard; exit non-zero on violation | |
| `fix-` | the mutating twin of a `check-` | must pair 1:1 with a `check-` |
| `show-` | print a report, never mutate, always exit 0 | replaces `*-list`, `lines`, `*-matrix`, `audit-*`, `*-count` |
| `clean-` | remove build output | `clean`, `clean-freeglut`, `clean-all` |
| `install-` | mutate the developer's environment | |
| `release-` | release orchestration | |

That is ten verbs, and the whole tree fits in them. The renames this implies
are mechanical and small:

```
keymap-list          -> show-keymap
config-list          -> show-config
palette-list         -> show-palette
capacity-matrix      -> show-capacity
lines / lines-test   -> show-lines            (SCOPE=src|test)
unicode-count        -> show-unicode          (fix-unicode already correct)
find-trailing-whitespace -> show-trailing-whitespace
audit-editor-ownership   -> show-editor-ownership
freeglut-clean       -> clean-freeglut
distclean            -> clean-all
*-demo               -> demo-*                (repl-demo -> demo-repl, ...)
gl-tests             -> test-gl
test-only            -> deleted (alias)
test-no-checks       -> deleted (see SKIP_CHECKS below)
bench-glut-bitmap-build -> deleted (implied by bench-glut-bitmap)
```

Keep the old spellings as `.PHONY` forwarders for one release cycle if you
want; the guard below can allow-list them by name in a `DEPRECATED_ALIASES`
list so they are visibly temporary rather than silently permanent.

### The two axes that become variables

**Platform (`PLATFORM`).** Replaces the `-web` suffix and the `web` prefix.

```make
PLATFORM ?= native      # native | web | osmesa | stubs
```

`make test PLATFORM=web` replaces `make test-web`; `make bench PLATFORM=web`
replaces `make bench-web`; `make gl-repl PLATFORM=osmesa` replaces
`make gl-repl FREEGLUT_OSMESA=1`. Internally `PLATFORM` sets the existing
`WEB` / `USE_GL_STUBS` / `FREEGLUT_OSMESA` switches, which stay as private
implementation. `web-serve` stays a real target (it is a different verb --
serving, not building) and becomes `run-web`.

`bench-web-gl4es` does not collapse -- it is a genuinely different benchmark
subject (browser draw path), so it becomes `bench-gl4es` and requires
`PLATFORM=web`.

**Report format (`FORMAT`).** Replaces every `-csv` target.

```make
FORMAT ?= human         # human | csv
```

`make bench FORMAT=csv` replaces `bench-csv`; the same one line covers
`bench-render` and `bench PLATFORM=web`. Three targets disappear and the axis
becomes available to every current and future bench.

**Check skipping.** `test-no-checks` becomes `make test SKIP_CHECKS=1`,
which is one switch instead of a duplicated recipe.

### Argument conventions (a rule, not a per-target choice)

- `ARGS` is the **only** passthrough for a target that executes something.
  Every `run-*` and every `bench-*` honours it. A target that runs a binary
  and does not honour `ARGS` is a bug.
- A target that needs a **required** operand takes it in one variable named
  for the operand (`SAMPLE=`, `ENTRY=`, `TEST_CASE=`), and errors with a usage
  line when it is empty. Several today just fail obscurely.
- Collapse the alias spellings: `NO_SAN` is the switch, `NOSAN`/`ASAN` are
  dropped; `V` is the switch, `VERBOSE` is dropped.

## 3. Peeling the onion: three-tier help

`help-details` is unmaintainable because it is one flat sorted dump plus 90
lines of hand-written prose about variables. Replace with:

**Tier 1 -- `make help`** (unchanged in spirit, ~15 lines): the ten things a
newcomer runs.

**Tier 2 -- `make help-<verb>`**: one target per verb, generated from the `## `
comments by filtering on prefix. `make help-check` finally makes the 92 guards
discoverable; `make help-bench`, `make help-test`, `make help-show` likewise.
This is the actual onion-peel: nobody needs all 177 at once, they need one
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

Four new checks, all source-scraping awk/sh in `scripts/check/`, following
`check-completions` (which already enforces "the docs and the thing agree").

### `check-make-target-grammar`

Every target with a recipe must either start with a verb from the closed set,
be in the small `ROOT_TARGETS` allow-list (`all`, `gl-repl`, `app`, `web`,
`demos`, `glut`, `clean`, `help`), be `internal-*`, or be in
`DEPRECATED_ALIASES`. Fails on a new `foo-list` or `distclean`.

### `check-make-target-documented`

Every non-`internal-*`, non-`.PHONY`-helper target carries a `## ` comment.
Twelve targets fail today. This is the guard that keeps the generated help
complete, so the generated help can *replace* the hand-written one.

### `check-make-no-duplicate-targets`

Fails on a target name declared twice with a recipe. Catches the two
`bench-*` duplicates now and the next copy-paste.

### `check-make-phony-derived`

Assert `BUILD_TARGETS` / `PACKAGE_TARGETS` / ... have been replaced by
awk-derived lists (the `CHECK_PHONY_TARGETS` model). Simplest form: fail if a
documented target is missing from `.PHONY`. Once the lists derive themselves
this is free, and it deletes five hand-maintained inventories.

### `check-make-fix-pairs-check`

Every `fix-X` has a `check-X`. Two pairs exist today (`doc-links`, `unicode`)
and both conform; the guard keeps `fix-` from becoming a junk drawer.

All five join `CHECK_TARGETS` so `make check` runs them.

## 5. Suggested sequencing

Each step is independently landable and independently reversible.

1. **Dedupe + document.** Delete the two duplicate `bench-*` stanzas, delete
   `test-only`, add `## ` to the twelve undocumented targets. Land
   `check-make-no-duplicate-targets` and `check-make-target-documented`.
2. **Derive `.PHONY`.** Replace the five hand-maintained `*_TARGETS` lists with
   awk scrapes. Land `check-make-phony-derived`. Pure deletion.
3. **Generated help.** Add `help-<verb>` and `help-vars`; move the runtime-env
   and freeglut prose to `docs/ADVANCED_USAGE.md`. `help-details` becomes
   generated. This is where the 182 lines actually shrink.
4. **`FORMAT` axis.** Delete `bench-csv`, `bench-render-csv`, `bench-web-csv`;
   add `FORMAT`. Three targets gone, one variable added.
5. **`PLATFORM` axis.** Fold `test-web`/`bench-web` and the raw
   `FREEGLUT_OSMESA=1`/`USE_GL_STUBS=1` invocations behind `PLATFORM`. Highest
   value, highest blast radius (CI, scripts/, CLAUDE.md, the pre-push hook all
   name `test-stubs` / `test-web`) -- do it last and keep forwarders.
6. **Verb renames + grammar guard.** The `show-*` / `demo-*` / `clean-*`
   renames, then `check-make-target-grammar` and
   `check-make-fix-pairs-check`.

Steps 1-3 are ~a day and deliver most of the readability win with no
invocation changes for anyone. Steps 4-6 change how people type commands and
should be one announced cut with forwarders.

## 6. What deliberately does not change

- `check-*` stays 92 targets. They are a guard *inventory*, and the fix for
  their bulk is `make help-check` plus keeping them out of the default help --
  not merging them. Merging would cost the per-guard failure message, which is
  the whole value.
- `gl-repl` keeps its name. It is the product.
- `BUILD` keeps its name and values.
- `internal-*` stays the marker for "implementation, do not type this".
