## Machine-Agnostic Metrics for the REPL Benchmarks

### Summary

`bench_repl` (introduced in this branch) currently reports **wall-clock
time only**. That is enough to spot regressions on the same machine, but
it makes cross-machine comparisons noisy: a number that comes back faster
on a Threadripper than on an N100 mini-PC tells us almost nothing about
whether the parser got more efficient. The fix is to add metrics that are
either physically invariant (CPU instructions retired, branch
mispredictions) or normalized to a synthetic baseline the same binary
measures on the host.

This document describes how to extend `bench_repl` with those metrics
and what the per-platform implementation effort looks like. Linux x86_64
is the primary target; macOS feasibility is covered at the end.

### What we want to be able to say

After this lands, a single benchmark line should be able to read like:

```
parse_lines  iters=10  ops=7920  total=4.20 ms  per-op=0.53 us
             instr/op=4124  cycles/op=2890  IPC=1.43  branch-misses/op=12
             vs-baseline=1.04x  (sha256-1MB normalization)
```

with the existing wall-clock columns still present so single-machine
diffs keep working.

### Metric tiers

The metrics naturally split into three tiers by portability and cost:

**Tier 1 - wall time (already shipped).** `clock_gettime(CLOCK_MONOTONIC)`,
no privileges, no kernel features. Always on.

**Tier 2 - hardware perf counters (Linux first, macOS later).** Counts
that capture *what the CPU actually did* and are largely insulated from
turbo/thermal effects:

- instructions retired (best single proxy for "amount of work")
- CPU cycles (combine with instructions for IPC)
- branch instructions / branch misses (parser quality signal)
- L1d / LLC cache misses (optional, useful for the long-replay scene)

These are stable across CPU SKUs in a much more useful sense than wall
time: a parser that goes from 5,000 → 4,000 instructions per line is
unambiguously faster, even if one machine clocks twice as fast as the
other.

**Tier 3 - normalized score.** Same binary runs a tiny CPU-only
microbenchmark (e.g. SHA-256 of 1 MB or a fixed-iteration xorshift
loop) at startup and reports REPL benchmark times *as a ratio* to that
baseline. This collapses CPU-frequency differences out of the wall-time
column and gives a single dimensionless number that can be compared
across an N100, a Ryzen 9, and a CI runner.

### Linux x86_64 implementation plan (primary target)

The existing benchmark is a plain user-space binary; perf counters can
be added without root or special build flags by going through
`perf_event_open(2)`.

#### Wiring

1. Add a new `bench_perf.c` / `bench_perf.h` pair next to `bench_repl.c`
   that owns a small `PerfCounters` struct. On Linux it wraps
   `perf_event_open` with `PERF_TYPE_HARDWARE` and event ids
   `PERF_COUNT_HW_INSTRUCTIONS`, `PERF_COUNT_HW_CPU_CYCLES`,
   `PERF_COUNT_HW_BRANCH_INSTRUCTIONS`, `PERF_COUNT_HW_BRANCH_MISSES`.
   Group them under a leader file descriptor with `read_format =
   PERF_FORMAT_GROUP | PERF_FORMAT_ID` so a single `read()` returns all
   counters atomically.

2. Wrap each sub-benchmark loop in `bench_repl.c` with
   `perf_start()` / `perf_stop()` (which `ioctl` `PERF_EVENT_IOC_RESET`
   + `PERF_EVENT_IOC_ENABLE` / `_DISABLE` on the leader fd). Cache the
   counters in `BenchResult` next to `total_sec`. Update `report()` to
   print the new columns; keep CSV column order stable by appending,
   not inserting, new fields.

3. Detect availability at runtime. `perf_event_open(2)` returns `-1`
   on failure and sets `errno`; if `errno == EACCES` (paranoid mode)
   or `errno == ENOSYS` (older kernel without HW counters), fall back
   to wall-time-only reporting and print a one-line warning to stderr.
   Do not fail the run. Note that `perf_event_open` is not wrapped by
   glibc and must be invoked via `syscall(SYS_perf_event_open, ...)`,
   which still sets `errno` the usual way.

4. Document the kernel knob in `README.md`: unprivileged perf counter
   access requires `kernel.perf_event_paranoid <= 1` (or `<= 2` for
   per-process counters). Most workstations and CI runners are already
   at `2`, which is enough for our process-only group.

#### Effort estimate

About **half a day** of work on Linux x86_64:
- ~80 lines for `bench_perf.c` (the perf_event_open boilerplate is
  small and stable; the existing kernel docs in `tools/perf/` are the
  reference),
- ~30 lines of plumbing in `bench_repl.c` (struct field, before/after
  calls, two new printf columns),
- ~10 lines of Makefile work (no new dependencies - perf_event_open is
  in libc).

No external libraries. No root. No build-mode change.

#### Caveats specific to Linux x86_64

- `instructions retired` on x86 has a small "skid" - a counter overflow
  can be attributed a few instructions past the actual one. We don't
  use sampling, just count totals, so skid does not matter for us.
- Hyper-threading: if the benchmark gets context-switched onto a
  sibling SMT thread mid-iteration, the `cycles` count is noisy. We
  already pin to one CPU implicitly by being short-lived; for the
  long-replay sub-benchmark it is worth pinning explicitly via
  `sched_setaffinity` to a single CPU id and disabling turbo
  (`cpupower frequency-set -g performance`) if the user wants a
  reproducible cycles/op number. Document this in README, do not
  enforce it.
- AMD vs. Intel: `PERF_COUNT_HW_*` are abstracted by the kernel and
  map to the right vendor-specific PMU events. Nothing in `bench_perf.c`
  is x86-specific; the same code path also works on ARM Linux if a
  developer wants to test on a Pi or a Graviton instance.

### Tier 3: normalized score

Add `bench_baseline.c` exposing `bench_baseline_run()` that returns
seconds for one well-defined workload (suggestion: 1 million iterations
of an xorshift32 inner loop - small, deterministic, no allocations,
exercises ALU+branch like the REPL parser does).

`main()` runs it once at startup, stores the seconds in a global, and
`report()` prints `vs-baseline = repl_per_op / baseline_per_iter` as
the last column. The exact baseline is arbitrary - what matters is
that *the same* binary measures it on every machine.

This piece is **machine-portable**: pure C, no kernel calls, ~30 lines.
Worth adding even before the perf-counter tier because it solves the
"this number doesn't compare across boxes" problem with the smallest
possible code surface.

### macOS feasibility

The macOS story is meaningfully harder than Linux. Options ranked by
effort:

**Option A - wall time + normalized score only.** Ships on macOS today
with zero extra code (Tier 1 + Tier 3 already are portable C). The
`vs-baseline` column gives a usable cross-machine comparison even
without HW counters. **Effort: zero, already covered by the Tier 3
plan above.**

**Option B - `mach_absolute_time` for a higher-resolution wall clock.**
On Apple Silicon, `clock_gettime(CLOCK_MONOTONIC)` is already backed by
`mach_absolute_time` so there is no precision win; this is only worth
doing if we ever target old x86 macOS where `clock_gettime` was
emulated. **Effort: trivial (~10 lines, conditional on `__APPLE__`),
but probably not worth it.**

**Option C - kperf / kpc HW counters via the private framework.**
macOS does expose performance counters (`kperf` / `kpc_*`) but the
public surface is severely limited:
- The `kpc_*` symbols live inside `/System/Library/PrivateFrameworks/kperf.framework`
  and are dlopen'd at runtime; they are not in any public SDK header.
- Reading counters requires either the `com.apple.private.kernel.kperf`
  entitlement (Apple-internal only) or running as root.
- Apple Silicon counters are organized differently from x86 - the
  event ids `INST_RETIRED.ANY` etc. are documented only via Instruments
  XML, and they change between M1 / M2 / M3 generations.

  Realistically this means we either:
  - require `sudo bench_repl` on macOS, or
  - shell out to `xcrun xctrace` to record an Instruments trace and
    parse the resulting `.trace` bundle, or
  - skip Tier 2 on macOS entirely.

  **Effort: 2–3 days minimum, fragile across OS upgrades, requires
  privilege escalation. Not recommended unless macOS becomes a primary
  perf target.**

**Recommendation for macOS:** ship Tier 1 + Tier 3 (wall time +
normalized baseline). Skip Tier 2 unless a concrete need appears.

### Compile-time / runtime gating

To keep the macOS and stub-mode builds clean, add a Makefile-level
flag:

```make
ifeq ($(UNAME_S),Linux)
BENCH_CFLAGS += -DBENCH_HAVE_PERF_EVENTS=1
endif
```

Inside `bench_perf.c`:

```c
#if BENCH_HAVE_PERF_EVENTS
/* perf_event_open path */
#else
/* no-op stubs that leave the counter fields zeroed */
#endif
```

The reporter prints the perf columns only when `instr_retired > 0` so
non-Linux output stays uncluttered.

### Phasing

Suggested order, each independently mergeable:

1. **Tier 3 - normalized baseline.** ~30 lines, portable, gives the
   single biggest cross-machine usability win for the smallest cost.
2. **Tier 2 - Linux perf counters.** ~120 lines behind a build flag,
   no behavior change on macOS or stubs builds.
3. **Optional: scheduler pinning + turbo-off documentation.** README
   only, no code; just so contributors can publish reproducible
   numbers.
4. **Optional: macOS Tier 2 via Instruments trace shell-out.** Only
   if/when a maintainer actively wants macOS perf data.

### What we explicitly do NOT plan

- **Statistical reporting (mean ± stddev, percentiles).** The current
  output already includes a `min` column, which for short benchmarks
  is the most informative single number (least disturbed by noise).
  Adding a full distribution is easy later but not in the initial
  metrics extension.
- **Memory / allocator stats.** The REPL is statically allocated
  globals - there's no `malloc` traffic worth measuring inside the
  hot loops. Skip.
- **Per-sub-benchmark overrides for iteration counts.** A single
  `--iters` flag works fine because the slowest sub-benchmark
  (`flatten_examples`, ~1 s/iter) already dominates so the others
  amortize well at the same iter count.
