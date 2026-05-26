# Add a Memory Profile Panel

## Context

The REPL has a CPU profile panel (Ctrl+W, three modes: Off / On / Details) that
shows per-section wall-time costs as a text table. There is no equivalent
panel for **process memory**, so slow leaks (steadily climbing RSS over a long
session, or VSZ growth from mmap churn) are invisible to the user.

Add a sibling panel that:
- Reads current process RSS + VSZ each frame and pushes a sample to a ring
  buffer every 5 seconds → ~85 min of history.
- Renders a **legible** line graph of RSS (bright) and VSZ (dim) over that
  window, with auto-fit Y-axis labels in MB/GB and time labels at the X-axis
  ends and midpoint (`-85m … -42m … now`).
- Shows three text lines: current RSS/VSZ, baseline RSS/VSZ (captured once at
  init), and Δ vs baseline.
- Works on macOS (`task_info`) and Linux (`/proc/self/statm`); leaves a clean
  Windows-future branch (`_WIN32` stub for now).

Toggle is in the Config menu (mirroring CPU profile) **and** bound to
Ctrl+Shift+M. Three modes: **Off / On / Details** — Off and On are the same
as CPU profile; in v1 Details adds nothing extra over On (room reserved for a
future per-allocator breakdown without enum migration).

## Module 1 — `src/support/memprof.{c,h}`

Mirrors `src/support/prof.{c,h}` in shape and conventions (file-scope statics
with `g_` prefix, no GL/UI deps, platform-conditional reader).

### Header (`memprof.h`)

```c
typedef struct {
    unsigned long long rss_bytes;  /* resident set size */
    unsigned long long vsz_bytes;  /* virtual size      */
} MemSample;

/* Capture baseline. Safe to call multiple times — only first call takes. */
void   memprof_init(void);

/* Per-frame entry point. Refreshes the cached "current" reading and
 * pushes a history sample when the wall-clock interval has elapsed.
 * The module owns its own monotonic clock; no parameter. */
void   memprof_frame_tick(void);

/* Test seam: drive the cadence with a virtual clock. Default-time path
 * goes through this with a real-clock argument. */
void   memprof_frame_tick_at(double now_seconds);

/* Cached current reading (always fresh — refreshed in every frame_tick). */
MemSample memprof_current(void);
MemSample memprof_baseline(void);

/* History ring: oldest-first traversal for the renderer.
 * sample_seconds_out is seconds-since-init for the i-th stored sample. */
int    memprof_history_count(void);
int    memprof_history_capacity(void);
void   memprof_history_get(int i, MemSample *out, double *sample_seconds_out);

/* Sweep helpers for graph auto-scale (cheap; capacity ≤ 1024). */
unsigned long long memprof_history_max_rss(void);
unsigned long long memprof_history_min_rss(void);
unsigned long long memprof_history_max_vsz(void);
```

### Implementation (`memprof.c`)

Constants:
```c
#define MEMPROF_HISTORY_CAP        1024
#define MEMPROF_PUSH_INTERVAL_S    5.0
/* Total span = 1024 * 5 = 5120s ≈ 85 min */
```

State:
```c
static int                g_memprof_initialized = 0;
static MemSample          g_memprof_baseline    = {0};
static MemSample          g_memprof_current     = {0};
static double             g_memprof_t0_seconds  = 0.0;
static double             g_memprof_last_push_s = -MEMPROF_PUSH_INTERVAL_S;
static int                g_memprof_count       = 0;
static int                g_memprof_head        = 0;
static MemSample          g_memprof_ring[MEMPROF_HISTORY_CAP];
static double             g_memprof_ring_t[MEMPROF_HISTORY_CAP];
```

Reader (cross-platform, ifdef chain — mirrors `prof.c` style):
```c
#if defined(__APPLE__)
#  include <mach/mach.h>
#  include <mach/task.h>
#elif defined(__linux__)
#  include <stdio.h>
#  include <unistd.h>
#elif defined(_WIN32)
   /* Future: #include <windows.h> + <psapi.h>; GetProcessMemoryInfo. */
#endif

static int memprof_read(MemSample *out) {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                 (task_info_t)&info, &count);
    if (kr != KERN_SUCCESS) { out->rss_bytes = out->vsz_bytes = 0; return 0; }
    out->rss_bytes = info.resident_size;
    out->vsz_bytes = info.virtual_size;
    return 1;
#elif defined(__linux__)
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) { out->rss_bytes = out->vsz_bytes = 0; return 0; }
    unsigned long size_p = 0, rss_p = 0;
    int n = fscanf(f, "%lu %lu", &size_p, &rss_p);
    fclose(f);
    if (n != 2) { out->rss_bytes = out->vsz_bytes = 0; return 0; }
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;
    out->rss_bytes = (unsigned long long)rss_p   * (unsigned long long)pagesz;
    out->vsz_bytes = (unsigned long long)size_p  * (unsigned long long)pagesz;
    return 1;
#else
    out->rss_bytes = out->vsz_bytes = 0;
    return 0;
#endif
}
```

`memprof_frame_tick()` queries monotonic time (same `#ifdef __APPLE__` pattern
as `prof.c` — `mach_absolute_time` vs `clock_gettime(CLOCK_MONOTONIC)`) and
calls `memprof_frame_tick_at(now)`. The test seam version takes the time
directly.

`memprof_frame_tick_at(t)`:
1. `memprof_read(&g_memprof_current)` every call (text rows must be live).
2. If `t - g_memprof_last_push_s >= MEMPROF_PUSH_INTERVAL_S`, push current
   into the ring (wrap on capacity), record `t - g_memprof_t0_seconds` into
   `g_memprof_ring_t[]`, advance head, update `last_push_s`.

Failure mode: when `memprof_read` returns 0, `g_memprof_current` is zeroed —
the panel detects this and renders `"--"` (mirrors the CPU panel's stale-row
convention with `k_prof_dim`).

## Module 2 — `src/ui/app/memory_panel.{c,h}`

Mirrors `src/ui/app/profile_panel.c` shape, but with a graph area below the
text block.

### Header

```c
typedef enum {
    MEMORY_PANEL_OFF = 0,
    MEMORY_PANEL_ON,
    MEMORY_PANEL_DETAILS,
    MEMORY_PANEL_MODE_COUNT
} UiMemoryPanelMode;

void ui_memory_panel_render(const UiRenderSnapshot *snap);
```

### Layout constants

```c
#define MEM_PANEL_W         360
#define MEM_PANEL_MARGIN     12
#define MEM_ROW_H            16
#define MEM_HEADER_H         20
#define MEM_TEXT_BLOCK_H     (3 * MEM_ROW_H + 6)  /* 3 rows + divider */
#define MEM_GRAPH_H         120
#define MEM_Y_GUTTER         44   /* room for "999 MB" Y labels */
#define MEM_BOTTOM_PAD       18   /* X labels + margin */
```

Total panel height = `MEM_HEADER_H + MEM_TEXT_BLOCK_H + MEM_GRAPH_H + MEM_BOTTOM_PAD + MEM_PANEL_MARGIN`.

### Text rows

Three rows, all using `gl2d_draw_string` + `FONT_SMALL`:

```
RSS   142 MB        VSZ   524 MB
init  138 MB        init  520 MB
Δ      +4 MB         Δ      +4 MB
```

Formatter (file-scope static):
```c
static void fmt_bytes(char *buf, int sz, unsigned long long b) {
    if (b == 0)                       snprintf(buf,sz,"--");
    else if (b < 1024ULL*1024)        snprintf(buf,sz,"%llu KB", b/1024);
    else if (b < 1024ULL*1024*1024)   snprintf(buf,sz,"%.1f MB", b/(1024.0*1024.0));
    else                              snprintf(buf,sz,"%.2f GB", b/(1024.0*1024.0*1024.0));
}
```

Signed Δ uses the same helper with a `+`/`-` prefix.

### Graph

Geometry inside the panel:
- Graph plot area: `(panel_x + MEM_Y_GUTTER + 4) .. (panel_x + MEM_PANEL_W - 4)` × `MEM_GRAPH_H`.
- Y-axis labels drawn in the left gutter (right-aligned at `panel_x + MEM_Y_GUTTER - 2`).
- X-axis labels drawn just below the plot area.

Y-axis auto-fit:
```c
static unsigned long long pick_nice_step(unsigned long long range) {
    /* Round range/3 up to {1, 2, 5} × 10^k bytes. Use unsigned long long
       arithmetic throughout; iterate k from 1024 (1 KB) upward. */
}
unsigned long long lo = memprof_history_min_rss();
unsigned long long hi = memprof_history_max_rss();
if (hi == 0) { /* render "no data" placeholder */ }
/* Include VSZ in the range so both lines fit. */
hi = max(hi, memprof_history_max_vsz());
unsigned long long range = hi - lo;
if (range < 1*1024*1024) range = 1*1024*1024;  /* clamp to ≥1 MB span */
unsigned long long pad   = range / 10;
unsigned long long y_lo  = (lo > pad) ? lo - pad/2 : 0;
unsigned long long y_hi  = hi + pad;
unsigned long long step  = pick_nice_step(y_hi - y_lo);
/* Snap y_lo down to a step multiple; y_hi up. */
```

Draw:
1. **Gridlines** (faint horizontal `GL_LINES`, `UI_TOK_DIVIDER` α=0.30): one
   per step value from snapped `y_lo` to `y_hi`. Format value with
   `fmt_bytes`; draw at gutter right edge.
2. **Plot area border**: `gl2d_panel_frame` with subtle background.
3. **VSZ line** (drawn first, dim): `GL_LINE_STRIP` over history; color
   `ui_clr_a(UI_TOK_ACCENT, 0.40f)` or a fixed dim cyan.
4. **RSS line** (bright, drawn second so it overlays): `GL_LINE_STRIP`;
   `ui_clr(UI_TOK_ACCENT)`. Mapping:
   ```
   x = plot_x + (i / (capacity - 1.0)) * plot_w
   y = plot_y + (sample.rss_bytes - y_lo) / (y_hi - y_lo) * MEM_GRAPH_H
   ```
   Skip samples beyond `memprof_history_count()` (graph fills left-to-right
   as samples accumulate).
5. **X-axis labels** at left/mid/right: `-85m`, `-42m`, `now` (derived from
   `MEMPROF_HISTORY_CAP * MEMPROF_PUSH_INTERVAL_S`). Use a tiny
   `fmt_time_offset` helper.

### Positioning

Mirror `profile_panel_rect_for_height` exactly, but with **anti-overlap**:

```c
static void memory_panel_rect_for_height(const UiRenderSnapshot *snap,
                                         int panel_h, int *out_x, int *out_y) {
    /* Same logic as profile_panel_rect_for_height, then:
     * if CPU profile panel is visible, shift this panel left by
     * (PROF_PANEL_W + 8) so they sit side-by-side, clamped to scene rect. */
}
```

Falls back to the standard scene-bottom-right anchor when CPU profile is off.

## Module 3 — UI state plumbing

### `src/ui/app/state_types.h`

Add (next to `UiProfilePanelState`):
```c
typedef struct { int mode; } UiMemoryPanelState;
```

### `src/ui/app/state.{c,h}`

Add `UiMemoryPanelState memory_panel;` field to `UiState`. Default:
`.memory_panel = { .mode = MEMORY_PANEL_OFF }` in the initial state literal.
Public getters: `UiMemoryPanelState ui_state_memory_panel(void);` +
`UiMemoryPanelState *ui_state_memory_panel_mut(void);` (mirror profile
panel's existing API exactly).

### `src/ui/app/snapshot.h`

Add `UiMemoryPanelState memory_panel;` to `UiRenderSnapshot`.

### `src/app/glr_ctrl.c::glr_ctrl_build_ui_snapshot`

Add `snap->memory_panel = ui_state_memory_panel();` right after the
`snap->profile_panel = ...` line.

## Module 4 — Config + actions wiring

### `src/app/glr_config.h`

Add `GLR_CONFIG_MEMORY_PROFILE` immediately after `GLR_CONFIG_CPU_PROFILE`.

### `src/app/glr_config.c`

Add a `case GLR_CONFIG_MEMORY_PROFILE: return &ui_state_memory_panel_mut()->mode;`
arm mirroring the CPU profile case.

### `src/app/glr_actions.c`

Add the mode-names array next to `profile_panel_mode_names`:
```c
static const char *memory_panel_mode_names[] = { "Off", "On", "Details" };
```

Append to `g_cfg_items[]` immediately after the CPU profile row:
```c
{ "Memory profile", 0, 0, 0, GLR_CONFIG_MEMORY_PROFILE,
  MEMORY_PANEL_MODE_COUNT, memory_panel_mode_names, 0 },
```

Note `key_code = 0`: Ctrl+Shift+M cannot use the simple `key_code` field
(since plain Ctrl+M is ASCII 13 = Enter). The hotkey is wired via a router
branch instead — see next section.

## Module 5 — Controller wiring (`src/app/glr_ctrl.c`)

### Includes
Add at top:
```c
#include "support/memprof.h"
#include "ui/app/memory_panel.h"
```

### Init — end of `glr_ctrl_init_gl()`
Add `memprof_init();` *after* `glr_app_reset_all()`, so the baseline reflects
"REPL ready and idle" rather than the bare process at `main()` entry. This is
the more useful baseline for leak attribution.

### Per-frame tick — in `glr_ctrl_display_frame()`
Right after `prof_frame_tick();`, add:
```c
memprof_frame_tick();
```

### Render — in `glr_ctrl_display_frame()`
After the existing `PROF_PROFILE_PANEL` block (~line 1947), add a parallel
profiled block:
```c
prof_begin(PROF_MEMORY_PANEL);
ui_memory_panel_render(&ui_snap);
prof_end(PROF_MEMORY_PANEL);
```

### Ctrl+Shift+M hotkey — in `glr_ctrl_keyboard()`
Add a branch following the existing Ctrl+Shift+X pattern (look at how
Ctrl+Shift+F is wired): when `key == 13 /* CR */` and `glutGetModifiers() &
(GLUT_ACTIVE_CTRL | GLUT_ACTIVE_SHIFT)` are both set, cycle the memory panel
mode via `glr_cfg_cycle_row` with the row index of `GLR_CONFIG_MEMORY_PROFILE`
(or equivalent — match whatever helper the existing Ctrl+Shift+* bindings
call). This must run **before** delegation to `editor_handle_key` so Enter
behavior is unaffected.

### `PROF_MEMORY_PANEL` enum entry
In `src/support/prof.h`, add `PROF_MEMORY_PANEL` right after `PROF_PROFILE_PANEL`.
In `src/ui/app/profile_panel.c::section_label`, add the case
`case PROF_MEMORY_PANEL: return "Memory Panel";`.

## Module 6 — Tests (`tests/test_memprof.c`)

New test target. Wire into Makefile via `TEST_BINS` and a per-test link
recipe (mirror `test_format` or similar minimal test). Test cases:

1. **Baseline non-zero on host platforms.** After `memprof_init()`,
   `memprof_baseline().rss_bytes > 0` on `__APPLE__` || `__linux__`. Skip
   assertion under `_WIN32` (since v1 stub returns 0).
2. **Cadence honored.** Drive `memprof_frame_tick_at` with virtual times
   `0.0, 1.0, 2.0, …, 4.9` → `memprof_history_count()` stays at 0 (or 1 if
   init counts as first push — pick one and assert consistently).
   At `t=5.0`, count becomes 1. At `t=10.0`, count becomes 2.
3. **Ring wraps.** Drive `1024 * 5` pushes; assert `count` clamps to
   `capacity` and `memprof_history_get(0, …)` returns the *new* oldest
   (not the original first sample).
4. **`fmt_bytes` ranges.** (Expose `fmt_bytes` non-statically only for the
   test build, via a `MEMPROF_TEST_HELPERS` macro, or include the .c
   directly from the test the way `tests/test_format.c` does — match
   existing test pattern.) Assert:
   - 0 → "--"
   - 1500 → "1 KB"
   - 1_500_000 → "1.4 MB"
   - 2_500_000_000 → "2.33 GB"

Tests must build under both default and `USE_GL_STUBS=1` since `memprof` has
no GL dependency.

## Makefile changes

In the existing `SRCS = …` block (around line 247), add:
```
  src/support/memprof.c \
  src/ui/app/memory_panel.c \
```
near `src/support/prof.c` and `src/ui/app/profile_panel.c` respectively.

In `CORE_TEST_SRCS`, add `src/support/memprof.c`.

Add a `test_memprof` target following the existing `test_format` recipe
shape, and append `test_memprof` to `TEST_BINS`.

## Critical files

Files to **create**:
- `src/support/memprof.h`
- `src/support/memprof.c`
- `src/ui/app/memory_panel.h`
- `src/ui/app/memory_panel.c`
- `tests/test_memprof.c`

Files to **modify**:
- `Makefile` — `SRCS`, `CORE_TEST_SRCS`, `TEST_BINS`, new per-test recipe
- `src/support/prof.h` — add `PROF_MEMORY_PANEL` enum value
- `src/ui/app/profile_panel.c` — `section_label` case for `PROF_MEMORY_PANEL`
- `src/ui/app/state_types.h` — `UiMemoryPanelState` typedef
- `src/ui/app/state.h` / `state.c` — field, accessors, default
- `src/ui/app/snapshot.h` — snapshot field
- `src/app/glr_config.h` — `GLR_CONFIG_MEMORY_PROFILE`
- `src/app/glr_config.c` — config dispatch case
- `src/app/glr_actions.c` — `memory_panel_mode_names[]` array, `g_cfg_items[]` entry
- `src/app/glr_ctrl.c` — includes, `memprof_init()` call, `memprof_frame_tick()`
  call, render block, Ctrl+Shift+M hotkey branch in `glr_ctrl_keyboard`

## Verification

1. `make check-include-style` — confirms quoted `"support/memprof.h"` and
   `"ui/app/memory_panel.h"` includes.
2. `make check-c99` — the build guard. New module must compile non-pedantic
   under `gcc -std=c99 -fsyntax-only` with the project's `-I` set.
3. `make test_memprof` (new) — unit tests pass.
4. `make test` — full suite still green.
5. **Real-gcc verification (Linux)**:
   ```
   ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
     git pull --ff-only origin main && \
     make check-c99 && make test-stubs && make test_memprof'
   ```
   Confirms the Linux `/proc/self/statm` path and the test under real GCC.
6. **Manual GUI** (macOS):
   - `make gl-repl && ./gl-repl`
   - Open Config → toggle "Memory profile" to On. Confirm panel appears with
     current RSS/VSZ + flat graph (1 sample). Wait 30s; confirm 5–6 samples
     plotted. Confirm Y-axis tick labels formatted as MB/GB.
   - Press Ctrl+W to toggle CPU profile. Confirm both panels render side by
     side without overlap.
   - Press Ctrl+Shift+M three times. Confirm cycle: On → Details → Off → On.
   - Press Enter (in input row) — confirm new-line insert still works (Ctrl+M
     hotkey must not have hijacked plain Enter).
   - Load a large workspace via `./gl-repl workspace/`. Confirm RSS line
     steps up to a new plateau; Δ row updates.
   - Leave running ~10 min in a corner. Confirm Y axis auto-rescales as
     range changes without visual stutter.
7. **Permission-failure smoke (macOS)**: temporarily edit `memprof_read` to
   force the `kr != KERN_SUCCESS` branch; confirm text rows show `--` and
   graph degrades gracefully (no crash, no `nan`). Revert.
