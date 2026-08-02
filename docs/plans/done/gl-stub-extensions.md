## GL Stub Extensions: Printf Trace for Per-Call Argument Diffs

## Status - DONE (2026-05-23 audit)

The trace facility shipped. Spot checks:

- `GL_STUB_TRACE_LINE` macro is invoked 103× across
  `tests/gl-stubs/include/` (every stubbed call has its argument line).
- Storage definition `FILE *gl_stub_trace_fp = NULL;` lives at
  `tests/gl-stubs/gl_stub_counts.c:24`; the env-var open helper is at
  lines 48–54 of the same file.
- `tests/test_export_trace_parity.c` consumes the new trace pathway -
  `--keep-traces` flag at line 234, `/tmp/test_trace_*.repl.tr`
  filename template at line 325, and the child compile cmd at line
  246 wires in the trace stub source.

The two "Out of Scope" carve-outs at the bottom of this plan
(pointer-array args, programmatic ring buffers) remain explicitly
deferred and shouldn't block done-ness.

### Summary
The stub layer at `tests/gl-stubs/include/GL/*` currently records only a
per-symbol call count (`gl_stub_counts[GL_STUB_*]`). That's enough for
shape comparisons - "did the REPL emit the same number of `glVertex3f`
calls as the exported C?" - but it's blind to argument values. The one
real divergence the count-only `test_export_trace_parity` surfaces on
`--full` today (Bezier off-by-one `glVertex2f`) would be much easier to
root-cause if both legs could dump a full per-call trace that `diff(1)`
can chew through.

### Approach
Add a `GL_STUB_TRACE_LINE(...)` macro that each stub function calls
just before `gl_stub_tick()`. The macro expands to either an fprintf
or nothing, gated by a compile-time `#define`. No in-memory storage,
no ring sizing decisions, no per-symbol tables - just lines of text
flushed to a configurable file. Diagnosis becomes a `diff` invocation.

### Macro Design
In `tests/gl-stubs/include/GL/gl_stub_counts.h` (or a sibling header),
alongside the existing `gl_stub_tick`:

```c
#ifdef GL_STUB_TRACE
#  include <stdio.h>
extern FILE *gl_stub_trace_fp;          /* defaults to NULL == off */
#  define GL_STUB_TRACE_LINE(...) do {                            \
        if (gl_stub_trace_fp) fprintf(gl_stub_trace_fp, __VA_ARGS__); \
    } while (0)
#else
#  define GL_STUB_TRACE_LINE(...) ((void)0)
#endif
```

Each stub function adds one line:

```c
static inline void glColor3f(GLfloat r, GLfloat g, GLfloat b) {
    GL_STUB_TRACE_LINE("glColor3f %g %g %g\n", r, g, b);
    gl_stub_tick(GL_STUB_glColor3f);
    /* body: still no-op */
}
```

For zero-arg calls (`glPushMatrix`, `glEnd`) just emit the name:

```c
GL_STUB_TRACE_LINE("glEnd\n");
```

The storage definition (`FILE *gl_stub_trace_fp = NULL;`) lives in
`tests/gl-stubs/gl_stub_counts.c` next to `gl_stub_counts[]`, inside
the same `#ifdef GL_STUB_TRACE` guard.

### Output Format
One call per line. First token is the symbol name; remaining tokens
are arguments separated by spaces. Floats via `%g` (consistent across
locales for the values the REPL emits). Enums via their integer value
(consumers can map back via the stub `g_*_names` tables if needed).

Example:
```
glBegin 4
glColor4f 1 0 0 1
glVertex3f 0 0 0
glVertex3f 1 0 0
glVertex3f 0 1 0
glEnd
```

Newline-terminated and free of synchronization markers - `diff -u`
sees aligned line ranges, the unified-diff hunk points at the
offending range, and you're done.

### Runtime Hookup
The trace file pointer is opt-in at runtime. Two reasonable shapes:

- **Env var**: a small `gl_stub_trace_open_from_env(void)` helper in
  `gl_stub_counts.c` that reads `$GL_STUB_TRACE_FILE`, fopen()s it,
  and stores the result in `gl_stub_trace_fp`. Callers invoke it
  once at startup. Closing on exit via `atexit()` is fine.
- **Explicit**: the test or driver sets `gl_stub_trace_fp` directly
  before the region of interest and clears it after. Simpler for the
  parity test since each leg writes to a different path.

### Integration With test_export_trace_parity
Add a `--trace` flag (or just always-on under `-DGL_STUB_TRACE`):

1. Test sets `gl_stub_trace_fp = fopen("/tmp/test_trace_<pid>_<name>_repl.tr", "w")`,
   runs the REPL leg, fclose. Snapshot counts as today.
2. Compose the child compile command with `-DGL_STUB_TRACE`; the
   driver opens its trace file path from argv[2]. Run child.
3. On count mismatch, run `diff -u <repl.tr> <child.tr>` via
   `system()` and pipe the first ~50 lines to stderr so the failure
   message includes a localized hint.
4. On match, leave the trace files in place (no cleanup) if `--keep-traces`
   was passed; otherwise unlink.

The count check stays as the primary gate. The trace is a diagnosis
aid - only consulted on failure or when explicitly requested.

### Cost
- Compile-time: ~one extra macro invocation per stub function. The
  X-macro list in `gl_stub_counts.h` doesn't change.
- Runtime when `gl_stub_trace_fp == NULL`: a single null-check and
  branch, which the predictor pins to "skip." Effectively free.
- Runtime when tracing: one fprintf per call. Slow, but only paid
  during the parity test's diagnosis runs.
- Code: the macro definition, one storage var, one optional env-var
  helper, and one `GL_STUB_TRACE_LINE(...)` line per stub. About 80
  lines of touched code across `gl_stub_counts.{c,h}` and the stub
  headers (`GL/gl.h`, `GL/glu.h`, `GL/freeglut.h`).

### Wrinkles
- **Float precision.** `%g` is locale-sensitive (decimal separator).
  Force `setlocale(LC_NUMERIC, "C")` once at trace open so the output
  is reproducible.
- **Deterministic ordering.** The REPL executor and the exported C
  walk the flat program in source order, so traces are already
  diff-aligned. If a future feature (parallel rendering, async
  callbacks) breaks that, the trace would need a sequence prefix
  per line and a more careful comparator.
- **String args.** `glutBitmapCharacter` takes a single byte -
  emit it as an integer code rather than as a char so a 0x00 or
  newline byte doesn't corrupt the trace. `label()`-emitted text
  shows up as a sequence of `glutBitmapCharacter N` lines, which is
  exactly the diff granularity we want for the label-divergence
  class of bugs.

### Out of Scope
- Capturing pointer-array arguments (`glLightfv`,
  `glPointParameterfv`, `glRasterPos3fv`). Could be added later by
  emitting `glLightfv GL_LIGHT0 GL_POSITION 2 4 5 0` after a small
  per-call length annotation in each X-macro entry. Defer until a
  parity test actually needs it.
- Programmatic in-memory ring buffers / last-argument tables. The
  text trace plus `diff` covers the same diagnostic need with much
  less code; revisit only if `fprintf` overhead becomes measurable
  in a build that wants tracing always on (it shouldn't for the
  parity-test use case).
- Real GL parity (running the same trace against a real driver).
  The stubs intentionally don't render; comparing CPU-side trace
  shapes is the design.
