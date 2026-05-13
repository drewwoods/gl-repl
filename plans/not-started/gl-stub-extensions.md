## GL Stub Extensions: Last-Argument Snapshots and Per-Symbol Ring Buffers

### Summary
The stub layer at `tests/gl-stubs/include/GL/*` currently records only a
per-symbol call count (`gl_stub_counts[GL_STUB_*]`). That's enough for
shape comparisons — "did the REPL emit the same number of `glVertex3f`
calls as the exported C?" — but it's blind to argument values. Two
divergences the count-only `test_export_trace_parity` already surfaces
on `--full` (Bezier off-by-one, label glutBitmapCharacter delta) would
be much easier to root-cause if the test could also assert the *values*
the two legs pass through the stubs.

### Motivation
`test_export_trace_parity` (added alongside this plan) cross-checks REPL
execution and the exported C by counting per-symbol GL calls. Real
divergences fall into a few buckets:

- **Counts match, arguments don't.** Two passes of `glColor3f(1, 0, 0)`
  vs `glColor3f(0.5, 0.5, 0.5)` look identical to the count comparator
  but render very differently. Today the test can't see this.
- **Counts are off by one in a long stream.** "REPL emitted 111
  glVertex2f, exported emitted 112" doesn't say *which* vertex was
  the extra one. With a per-symbol ring buffer of recent arguments the
  test can diff the two streams and point at the divergence.
- **Argument folding the REPL does on purpose.** `CMD_COLOR3F` routes
  through `glColor4f(..., g_execute_alpha_scale)` in the executor.
  Last-argument snapshots would let the test confirm the alpha was the
  expected default, distinguishing "intentional fold" from "accidental
  divergence."

### V1: Last-Argument Snapshots
Drop-in compatible with the existing X-macro list in
`tests/gl-stubs/include/GL/gl_stub_counts.h`.

Add a parallel storage table indexed the same way:

```
extern union gl_stub_last_arg gl_stub_last_args[GL_STUB_COUNT_MAX];
```

`gl_stub_last_arg` is a `union` over the largest argument tuple in the
covered API. For Phase 1 cover the common shapes:

```
union gl_stub_last_arg {
    struct { float a, b, c, d; } f4;
    struct { int   a, b, c, d; } i4;
    struct { GLenum a, b; }       e2;
    GLenum                        e1;
    float                         f1;
    int                           i1;
    GLboolean                     b1;
};
```

Each stub function writes its arguments into its slot before
`gl_stub_tick()`. The X-macro list grows a second column that names the
appropriate union member:

```
X(glColor3f, f4)
X(glColor4f, f4)
X(glVertex3f, f4)
X(glBegin, e1)
...
```

Trace consumers compare `gl_stub_last_args[GL_STUB_glColor3f].f4.a/b/c`
between REPL and child traces in the same place they compare counts.

### V2: Per-Symbol Ring Buffers
Gated behind `-DGL_STUBS_TRACE_BUFFER` so the build cost stays opt-in.

```
extern struct gl_stub_ring gl_stub_rings[GL_STUB_COUNT_MAX];
```

Each ring is a small (configurable, default 64) circular buffer of the
same union type as V1. On call: push at head, advance, wrap. Counter
stays accurate even after wrap because it's still incremented
independently.

Trace consumers iterate `gl_stub_rings[i].entries[0..ring_count(i))` and
diff in order. The two known `--full` divergences become tractable:
- **Bezier off-by-one**: walk both rings for `glVertex2f` until they
  diverge, print the first mismatched index and arguments.
- **Label glutBitmapCharacter delta**: same approach over the character
  ring shows which formatted strings differ.

### V3: Argument Predicates / Tolerances
Float comparisons need tolerance. Add a `gl_stub_arg_eq()` helper that
defaults to bit-exact for ints/enums and `fabsf(a - b) < 1e-5f` for
floats (caller-overridable). The parity test calls this instead of
`memcmp` on the union slots.

### Migration Notes
- The X-macro list is the single source of truth. Any new stub added
  to `GL_STUB_COUNTER_LIST` automatically picks up the new storage.
- Cost in real-GL builds is still zero: the entire storage and every
  helper is wrapped in `#ifdef GL_STUBS`, exactly like
  `gl_stub_counts[]` today.
- The hot path stays a single increment plus (in V1) a tiny field
  assignment. V2's ring push is a couple extra writes; gate behind
  `-DGL_STUBS_TRACE_BUFFER` for builds that care.

### Test Harness Integration
- `tests/export_trace_driver.c` already dumps `gl_stub_counts`; extend
  it to emit `gl_stub_last_args` (V1) and `gl_stub_rings` (V2) on the
  same out-file path, one section per channel.
- `tests/test_export_trace_parity.c` grows a `compare_last_args()` /
  `compare_rings()` step parallel to its existing `compare_counts()`.
- Backwards-compatible: a stub-counts-only build still works; the new
  channels just stay zeroed and the diff is a no-op.

### Out of Scope
- Capturing pointer arguments (`glLightfv`, `glPointParameterfv`,
  `glRasterPos3fv`) — content is array-by-reference; would require a
  side-buffer copy and length annotation per X-entry. Defer until a
  parity test actually needs it.
- Real GL parity (running the same trace against a real driver). The
  stubs intentionally don't render; comparing CPU-side trace shapes is
  the design.
