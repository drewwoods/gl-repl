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
Ctrl+Shift+M. Three modes: **Off / On / Details** - Off and On are the same
as CPU profile; in v1 Details adds nothing extra over On (room reserved for a
future per-allocator breakdown without enum migration).

## Module 1 - `src/support/memprof.{c,h}`

Mirrors `src/support/prof.{c,h}` in shape and conventions (file-scope statics
with `g_` prefix, no GL/UI deps, platform-conditional reader).

### Header (`memprof.h`)

```c
/* Public compile-time tuning. Exposed in the header (not buried in the
 * .c) so tests can drive the ring deterministically without duplicating
 * literals. */
#define MEMPROF_HISTORY_CAP        1024
#define MEMPROF_PUSH_INTERVAL_S    5.0
/* Total span = MEMPROF_HISTORY_CAP * MEMPROF_PUSH_INTERVAL_S
 *            = 5120 s ≈ 85 min */

typedef struct {
    unsigned long long rss_bytes;  /* resident set size */
    unsigned long long vsz_bytes;  /* virtual size      */
} MemSample;

/* Capture baseline using the module's monotonic clock as t0.
 * Safe to call multiple times - only first call takes. */
void   memprof_init(void);

/* Per-frame entry point. Refreshes the cached "current" reading and
 * pushes a history sample when the wall-clock interval has elapsed.
 * The module owns its own monotonic clock; no parameter. */
void   memprof_frame_tick(void);

/* --- Test seams (also safe to use in production, but exist to make the
 *     module deterministic under test). --- */

/* Init with an explicit t0, instead of querying the monotonic clock.
 * memprof_init() is implemented as `memprof_init_at(memprof_now_s())`.
 * Tests use this to control sample timestamps from a known origin. */
void   memprof_init_at(double t0_seconds);

/* Drive the cadence with a virtual clock. The production frame-tick
 * goes through this with the real clock argument. Tests can call it
 * directly with monotonically-increasing virtual times. */
void   memprof_frame_tick_at(double now_seconds);

/* Clear ALL module state: initialized flag, baseline, current, t0,
 * last-push timestamp, ring contents, count, head, injected reader.
 * Must be called in every test's setup (or teardown) to avoid state
 * leaking across cases when tests share the same process. */
void   memprof_reset(void);

/* Inject a synthetic reader for tests so cadence/ring-wrap assertions
 * can compare exact MemSample values. NULL restores the platform reader.
 * The injected reader is cleared by memprof_reset(). */
typedef int (*MemprofReaderFn)(MemSample *out);
void   memprof_set_reader(MemprofReaderFn reader);

/* --- Live state accessors. --- */

/* Cached current reading (always fresh - refreshed in every frame_tick). */
MemSample memprof_current(void);
MemSample memprof_baseline(void);

/* History ring: oldest-first traversal for the renderer.
 * sample_seconds_out is seconds-since-init for the i-th stored sample. */
int    memprof_history_count(void);
int    memprof_history_capacity(void);
void   memprof_history_get(int i, MemSample *out, double *sample_seconds_out);

/* t_rel (seconds-since-t0) of the newest pushed sample. Used by the
 * renderer to right-align the newest sample to "now" without jiggle.
 * Returns 0.0 when history is empty. */
double memprof_history_latest_t(void);

/* Sweep helpers for graph auto-scale (cheap; capacity ≤ 1024). */
unsigned long long memprof_history_max_rss(void);
unsigned long long memprof_history_min_rss(void);
unsigned long long memprof_history_max_vsz(void);

/* --- Pure formatter (no platform deps; tested in test_memprof.c
 *     without pulling in any UI/GL code). --- */

/* Format byte count as "999 KB" / "1.4 MB" / "2.33 GB"; 0 → "--".
 * Writes into buf and returns buf for chained use. */
const char *memprof_format_bytes(char *buf, int buf_sz,
                                 unsigned long long bytes);
```

### Implementation (`memprof.c`)

The capacity and push interval are defined in `memprof.h` (see above) so
both the implementation and the tests share the same numbers.

State:
```c
static int                g_memprof_initialized = 0;
static MemSample          g_memprof_baseline    = {0};
static MemSample          g_memprof_current     = {0};
static double             g_memprof_t0_seconds  = 0.0;
static double             g_memprof_last_push_s = 0.0; /* relative to t0 */
static int                g_memprof_count       = 0;
static int                g_memprof_head        = 0;
static MemSample          g_memprof_ring[MEMPROF_HISTORY_CAP];
static double             g_memprof_ring_t[MEMPROF_HISTORY_CAP];
static MemprofReaderFn    g_memprof_reader      = NULL; /* NULL → memprof_read */
```

Lifecycle plumbing:
```c
static double memprof_now_s(void); /* monotonic clock, real-platform */

void memprof_init(void)                    { memprof_init_at(memprof_now_s()); }
void memprof_frame_tick(void)              { memprof_frame_tick_at(memprof_now_s()); }
void memprof_set_reader(MemprofReaderFn r) { g_memprof_reader = r; }

void memprof_init_at(double t0_seconds) {
    if (g_memprof_initialized) return;
    g_memprof_t0_seconds  = t0_seconds;
    g_memprof_last_push_s = 0.0;   /* First history push happens at t=5s
                                    * (one full interval after init), not
                                    * at t=0. This is the committed
                                    * sampling semantics - text rows are
                                    * live immediately, graph fills in. */
    /* Capture baseline now (text rows): */
    MemprofReaderFn reader = g_memprof_reader ? g_memprof_reader : memprof_read;
    reader(&g_memprof_baseline);
    g_memprof_current = g_memprof_baseline;
    g_memprof_initialized = 1;
}

void memprof_reset(void) {
    g_memprof_initialized = 0;
    g_memprof_t0_seconds  = 0.0;
    g_memprof_last_push_s = 0.0;
    g_memprof_count       = 0;
    g_memprof_head        = 0;
    g_memprof_baseline    = (MemSample){0};
    g_memprof_current     = (MemSample){0};
    g_memprof_reader      = NULL;
    /* Ring contents left as-is; count=0 makes them unreachable. */
}
```

Reader (cross-platform, ifdef chain - mirrors `prof.c` style):
```c
/* Must appear BEFORE any system header. Mirrors src/support/prof.c so
 * Linux gets clock_gettime() / CLOCK_MONOTONIC prototypes under -std=c99
 * (which otherwise hides POSIX 2008 names). */
#define _POSIX_C_SOURCE 200809L

/* Unconditional standard headers - used regardless of platform.
 *   stdio.h: snprintf in memprof_format_bytes (every build) and
 *            fopen/fscanf inside the __linux__ reader branch.
 *   string.h: memset (zeroing samples on reset / failure). */
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#  include <mach/mach.h>
#  include <mach/task.h>
#elif defined(__linux__)
#  include <unistd.h>           /* sysconf(_SC_PAGESIZE) */
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
    /* Open/read/close every call (once per frame, ~60 Hz). This is
     * intentional: procfs reads are ~1 µs, and holding the FD open
     * would prevent the kernel from refreshing the snapshot on each
     * read on some kernels. Don't "optimize" by caching the FD. */
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

`memprof_now_s()` is implemented with the same `#ifdef __APPLE__` pattern as
`prof.c` (`mach_absolute_time` vs `clock_gettime(CLOCK_MONOTONIC)`), returning
absolute seconds. `memprof_frame_tick()` calls `memprof_frame_tick_at(memprof_now_s())`.

`memprof_frame_tick_at(t_abs)`:
1. If not initialized, return early (callers should `memprof_init` first; in
   production this is guaranteed by the controller-init call site).
2. Compute `t_rel = t_abs - g_memprof_t0_seconds`. Sample timestamps stored
   in the ring are always relative to `t0`, so the renderer can compute "age"
   as `t_rel_now - sample_t_rel` without knowing the absolute clock.
3. Refresh cache: `MemprofReaderFn r = g_memprof_reader ? g_memprof_reader : memprof_read; r(&g_memprof_current);`
   (text rows must be live).
4. If `t_rel - g_memprof_last_push_s >= MEMPROF_PUSH_INTERVAL_S`, push
   `g_memprof_current` into the ring (wrap on capacity), record `t_rel` into
   `g_memprof_ring_t[]`, advance `head`, set `g_memprof_last_push_s = t_rel`.

Failure mode: when the reader returns 0, `g_memprof_current` is zeroed - the
panel detects this and renders `"--"` (mirrors the CPU panel's stale-row
convention with `k_prof_dim`).

Most-recent-timestamp helper for the renderer (so the X axis can right-align
the newest sample to "now" - see Graph section):
```c
double memprof_history_latest_t(void); /* returns t_rel of the newest
                                          sample, or 0 if count == 0 */
```
Add this to `memprof.h` next to `memprof_history_get`.

## Module 2 - `src/ui/app/memory_panel.{c,h}`

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

Formatter lives in `src/support/memprof.c` as the public
`memprof_format_bytes(buf, sz, bytes)` helper (declared in `memprof.h`).
This keeps the formatter on the support side of the dependency boundary, so
`tests/test_memprof.c` can exercise it without pulling in any GL/UI code.

```c
const char *memprof_format_bytes(char *buf, int sz, unsigned long long b) {
    if (b == 0)                       snprintf(buf,sz,"--");
    else if (b < 1024ULL*1024)        snprintf(buf,sz,"%llu KB", b/1024);
    else if (b < 1024ULL*1024*1024)   snprintf(buf,sz,"%.1f MB", b/(1024.0*1024.0));
    else                              snprintf(buf,sz,"%.2f GB", b/(1024.0*1024.0*1024.0));
    return buf;
}
```

Edge-case note: values `0 < b < 1024` format as `"0 KB"` due to integer
division. Irrelevant for real RSS/VSZ (always many pages), but the test
case for `b=512` therefore expects `"0 KB"` not `"<1 KB"`. If sub-KB
display ever matters, switch to `%.2f KB` with `b/1024.0` and adjust the
test expectations.

The panel calls `memprof_format_bytes` for each value. Signed Δ is formatted
by a tiny file-scope wrapper in `memory_panel.c` that picks the `+`/`-`
prefix, then delegates to `memprof_format_bytes` for the absolute magnitude.

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

X-axis (**time-anchored, right-aligned to "now"**):

The X-axis is *always* anchored such that the right edge is "now" and the
left edge is `total_span = MEMPROF_HISTORY_CAP * MEMPROF_PUSH_INTERVAL_S`
seconds ago, regardless of how full the ring is. A partially-filled ring
shows samples only in the right portion of the graph; the left portion is
empty until enough wall-clock time has elapsed. This matches the X-axis
labels (`-85m … -42m … now`).

For each stored sample i ∈ [0, count):
```
double t_rel        = sample_t_seconds_for(i);          /* from memprof_history_get */
double t_latest     = memprof_history_latest_t();       /* newest sample's t_rel */
double age_seconds  = t_latest - t_rel;                 /* age relative to newest */
float  x = (float)(plot_x + plot_w
                   - (age_seconds / total_span) * plot_w);
float  y = (float)(plot_y
                   + (sample.rss_bytes - y_lo) / (double)(y_hi - y_lo) * MEM_GRAPH_H);
```

Anchoring to `memprof_history_latest_t()` (rather than the live current
`memprof_now_s()`) avoids a per-frame horizontal jiggle: between two pushes
the newest sample stays pinned to the right edge.

Draw:
1. **Gridlines** (faint horizontal `GL_LINES`, `UI_TOK_DIVIDER` α=0.30): one
   per step value from snapped `y_lo` to `y_hi`. Format value with
   `memprof_format_bytes`; draw at gutter right edge.
2. **Plot area border**: `gl2d_panel_frame` with subtle background.
3. **VSZ line** (drawn first, dim): `GL_LINE_STRIP` over history; color
   `ui_clr_a(UI_TOK_ACCENT, 0.40f)` or a fixed dim cyan. Uses the same x/y
   mapping as RSS, with `sample.vsz_bytes` in place of `sample.rss_bytes`.
4. **RSS line** (bright, drawn second so it overlays): `GL_LINE_STRIP`;
   `ui_clr(UI_TOK_ACCENT)`, using the mapping shown above.
5. **X-axis labels** at left/mid/right: `-85m`, `-42m`, `now` (derived from
   `total_span`). Use a tiny `fmt_time_offset` helper. These labels are
   geometry-fixed - they describe the panel's time scale, not the history's
   extent.

### Positioning

Mirror `profile_panel_rect_for_height` exactly, but with **anti-overlap**: if
the CPU profile panel is visible (`snap->profile_panel.mode != PROFILE_PANEL_OFF`),
shift this panel left by `PROFILE_PANEL_W + 8` so the two sit side-by-side,
then clamp to the scene rect. Falls back to the standard scene-bottom-right
anchor when CPU profile is off.

`PROFILE_PANEL_W` is currently a `#define PROF_PANEL_W 320` private to
`src/ui/app/profile_panel.c`. To allow `memory_panel.c` to reference it for
side-by-side layout, **move the `#define` into `src/ui/app/profile_panel.h`**
(rename to `PROFILE_PANEL_W` for clarity; update the in-file references in
`profile_panel.c`). This is a tiny, mechanical change and keeps panel widths
co-located with the public panel API.

The memory panel must mirror `profile_panel_rect_for_height`'s **full
branching on `snap->variable_panel.visible`** (`profile_panel.c:51-73`),
not just one branch - otherwise the two panels desync when the user hides
the variable panel and the side-by-side offset lands them somewhere weird.

```c
static void memory_panel_rect_for_height(const UiRenderSnapshot *snap,
                                         int panel_h, int *out_x, int *out_y) {
    int scene_x, scene_y, scene_w, scene_h;
    int panel_x, panel_y;
    ui_layout_scene_rect(&scene_x, &scene_y, &scene_w, &scene_h);

    /* Mirror profile_panel_rect_for_height EXACTLY, with MEM_* swapped in
     * for PROF_*: anchor depends on whether the variable panel is shown. */
    if (snap->variable_panel.visible) {
        panel_x = scene_x + scene_w - MEM_PANEL_W - MEM_PANEL_MARGIN;
        panel_y = scene_y + scene_h - panel_h    - MEM_PANEL_MARGIN;
    } else {
        int var_x, var_y, var_w, var_h;
        ui_variable_panel_rect_for_count(snap, snap->variable_panel_vars.count,
                                         &var_x, &var_y, &var_w, &var_h);
        panel_x = var_x + var_w - MEM_PANEL_W;
        panel_y = var_y;
    }

    /* Side-by-side when CPU profile is also up: shift left of its slot. */
    if (snap->profile_panel.mode != PROFILE_PANEL_OFF) {
        panel_x -= (PROFILE_PANEL_W + 8);
    }

    /* Clamp into scene rect (matches profile_panel's clamps). */
    panel_x = clamp_int(panel_x, scene_x + 4, scene_x + scene_w - MEM_PANEL_W - 4);
    panel_y = clamp_int(panel_y,
                        scene_y + STATUSBAR_H + 4,
                        scene_y + scene_h     - panel_h - 4);
    if (out_x) *out_x = panel_x;
    if (out_y) *out_y = panel_y;
}
```

Required local helpers (not in any shared header today):
- **`clamp_int(v, lo, hi)`** is file-static in `src/ui/app/profile_panel.c:35`.
  Duplicate it as a 5-line file-static helper at the top of `memory_panel.c`
  rather than extracting to a shared header - it's trivial, and extracting
  would touch profile_panel.c unnecessarily. (If a third caller emerges later,
  hoist it then.)
- **`STATUSBAR_H`** comes from `src/ui/app/layout.h` - include that header at
  the top of `memory_panel.c` (profile_panel.c also includes it; mirror the
  same `#include "ui/app/layout.h"` line).

## Module 3 - UI state plumbing

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

## Module 4 - Config + actions wiring

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

Append to `g_cfg_items[]` immediately after the CPU profile row (which sits
under the `### INTERFACE` section header at `glr_actions.c:175` - Memory
profile must share that section so it appears in the same Config flyout):
```c
/* In ### INTERFACE section, right after the CPU profile row: */
{ "Memory profile", 0, 0, 0, GLR_CONFIG_MEMORY_PROFILE,
  MEMORY_PANEL_MODE_COUNT, memory_panel_mode_names, 0 },
```

Note `key_code = 0`: Ctrl+Shift+M cannot use the simple `key_code` field
(since plain Ctrl+M is ASCII 13 = Enter). The hotkey is wired via a router
branch instead - see next section.

## Module 5 - Controller wiring (`src/app/glr_ctrl.c`)

### Includes
Add at top:
```c
#include "support/memprof.h"
#include "ui/app/memory_panel.h"
```

### Init - end of `glr_ctrl_init_gl()`
Add `memprof_init();` *after* `glr_app_reset_all()`, so the baseline reflects
"REPL ready and idle" rather than the bare process at `main()` entry. This is
the more useful baseline for leak attribution.

### Per-frame tick - in `glr_ctrl_display_frame()`
Right after `prof_frame_tick();`, add:
```c
memprof_frame_tick();
```

### Render - in `glr_ctrl_display_frame()`
After the existing `PROF_PROFILE_PANEL` block (~line 1947), add a parallel
profiled block:
```c
prof_begin(PROF_MEMORY_PANEL);
ui_memory_panel_render(&ui_snap);
prof_end(PROF_MEMORY_PANEL);
```

### Ctrl+Shift+M hotkey - new router helper in `src/app/glr_ctrl.c`

Add a named router helper alongside the existing chain (`glr_ctrl.c:2604`
onward - see `glr_ctrl_router_handle_save_key`, `_handle_code_focus_key`, etc).
The existing routers go through `editor_input_active_modifiers()`, **not**
`glutGetModifiers()` - match that pattern, and check each modifier bit
individually (`mods & (A | B)` is true if *either* bit is set, which would
steal Shift+Enter from the editor):

```c
int glr_ctrl_router_handle_memory_panel_key(unsigned char key) {
    if (key != 13) return 0;                       /* CR / Ctrl+M */
    int mods = editor_input_active_modifiers();
    if (!(mods & GLUT_ACTIVE_CTRL))  return 0;     /* both bits required: */
    if (!(mods & GLUT_ACTIVE_SHIFT)) return 0;     /*  no Shift+Enter steal */
    /* Cycle the config value directly via the keyed API. */
    glr_config_cycle(GLR_CONFIG_MEMORY_PROFILE, +1);
    return 1;
}
```

There is **no** `glr_cfg_cycle_by_key()` helper; the existing APIs are:
- `glr_config_cycle(ReplConfigKey key, int dir)` - cycle by config key
  (this is the right tool here).
- `glr_cfg_cycle_row(int row_idx, int dir)` - cycle by `g_cfg_items[]`
  index (useful when a row index is already known, e.g. menu clicks).
- `glr_ctrl_router_handle_code_focus_key` does *not* go through config - it
  toggles a state flag directly, which is why its shape differs from this
  router.

If `glr_config_cycle` proves not to exist with that exact signature at
implementation time (no harm in double-checking), fall back to:
```c
for (int i = 0; i < CFG_ITEM_COUNT; i++) {
    if (g_cfg_items[i].key == GLR_CONFIG_MEMORY_PROFILE) {
        glr_cfg_cycle_row(i, +1);
        return 1;
    }
}
return 0;
```

**Insertion in the chain**: in `glr_ctrl_keyboard` around `glr_ctrl.c:3857-3867`
(where each router is called in sequence with short-circuit `return`),
insert the new call **right after `glr_ctrl_router_handle_code_focus_key`**
(~line 3865). This must run *before* delegation to `editor_handle_key` so
the unmodified Enter behavior is preserved.

Verification: with the keyboard running, press Shift+Enter inside the input
buffer - must still insert a newline (not cycle the memory panel). Press
plain Enter - must still commit. Press Ctrl+Shift+M - must cycle the panel.

### `PROF_MEMORY_PANEL` enum entry
In `src/support/prof.h`, add `PROF_MEMORY_PANEL` right after `PROF_PROFILE_PANEL`.
In `src/ui/app/profile_panel.c::section_label`, add the case
`case PROF_MEMORY_PANEL: return "Memory Panel";`.

## Module 6 - Tests

### `tests/test_memprof.c` (new)

New test target. Wire into Makefile via `TEST_BINS` and a per-test link
recipe (mirror `test_format` or similar minimal test). Each test case begins
with `memprof_reset()` followed by either a fake-reader install + `memprof_init_at(0.0)`
*or* a real `memprof_init()` (case 1 only), so cases don't leak state.

1. **Baseline non-zero on host platforms.** `memprof_reset(); memprof_init();`,
   then `memprof_baseline().rss_bytes > 0` on `__APPLE__` || `__linux__`. Skip
   assertion under `_WIN32` (v1 stub returns 0).
2. **Cadence honored.** `memprof_reset();` install a fake reader returning
   `{rss=100, vsz=200}`; `memprof_init_at(0.0);`. Drive `memprof_frame_tick_at`
   with virtual times `0.5, 1.0, 1.5, …, 4.9` → assert `memprof_history_count()`
   stays at 0 (init does not push). Call `memprof_frame_tick_at(5.0)` →
   assert count == 1 and the stored sample's rss == 100. Call
   `memprof_frame_tick_at(10.0)` → assert count == 2.
3. **Ring wraps.** `memprof_reset();` install a reader that returns whatever
   the test wrote into a global `g_test_rss`. `memprof_init_at(0.0)` (init
   reader call captures baseline - irrelevant here, ring is empty). Then
   **tick only at push boundaries** (one tick per push interval, no sub-interval
   noise) for `N = MEMPROF_HISTORY_CAP + 5` iterations:
   ```c
   for (int i = 1; i <= N; i++) {
       g_test_rss = (unsigned long long)i;     /* what the reader returns */
       memprof_frame_tick_at(i * MEMPROF_PUSH_INTERVAL_S);
   }
   ```
   After the loop, `memprof_history_count() == capacity`. The ring should
   hold pushes [6, 7, …, N], so:
   - `memprof_history_get(0, &s, NULL)` → `s.rss == 6` (oldest survivor).
   - `memprof_history_get(capacity - 1, &s, NULL)` → `s.rss == N`.

   This relies on ticking exactly at push boundaries so the counter math
   stays simple; sub-interval ticks would still pass the count assertion
   but make the slot-0 value harder to predict.
4. **Right-anchored timestamps.** Assert
   `memprof_history_latest_t() ≈ last_pushed_t_rel` (within an epsilon),
   so the renderer can right-align the newest sample to "now" deterministically.
5. **`memprof_format_bytes` ranges** (pure formatter, no platform/GL deps -
   safe to test here since it lives in `memprof.c`):
   - 0 → "--"
   - 1500 → "1 KB"
   - 1_500_000 → "1.4 MB"
   - 2_500_000_000 → "2.33 GB"
6. **`memprof_set_reader(NULL)` restores the platform reader.** Set a fake,
   then NULL, then `memprof_frame_tick_at(...)`; on `__APPLE__` || `__linux__`
   the next current reading should match what the platform reports (non-zero
   RSS).

Tests must build under both default and `USE_GL_STUBS=1` since `memprof` has
no GL dependency.

### `tests/test_glr_ctrl.c` (modify)

Two updates required so the existing controller test keeps working after
`ui_memory_panel_render` and `PROF_MEMORY_PANEL` land:

1. **Stub the new render function** so the controller test doesn't pull in
   real GL (it currently stubs every UI render via `#define X test_X`).
   - At the `#define` block (~line 31, with the existing
     `#define ui_profile_panel_render test_ui_profile_panel_render`), add:
     ```c
     #define ui_memory_panel_render             test_ui_memory_panel_render
     ```
   - At the matching `#undef` block (~line 54), add:
     ```c
     #undef ui_memory_panel_render
     ```
   - Provide an empty `static void test_ui_memory_panel_render(const UiRenderSnapshot *snap) { (void)snap; }`
     near the other test render stubs.

2. **Extend the profile-coverage assertion** in
   `test_display_frame_profile_coverage` (~line 331) so the new section is
   guarded:
   - Add `PROF_MEMORY_PANEL,` to the `major[]` array (~line 343) right after
     `PROF_PROFILE_PANEL`.
   - Add `+ prof_section_last_us(PROF_MEMORY_PANEL)` to the `sum_us` sum
     (~line 382) so the half-of-frame lower-bound continues to balance.

Without these the test will either hard-fail (PROF_MEMORY_PANEL stale because
the controller wraps the new render in `prof_begin/end` but the test missed
it) or, worse, accidentally run real UI code if the stub isn't in place.

## Makefile changes

In the existing `SRCS = …` block (around line 247), add:
```
  src/support/memprof.c \
  src/ui/app/memory_panel.c \
```
near `src/support/prof.c` and `src/ui/app/profile_panel.c` respectively.

In `HDRS = …`, add the two new headers next to their existing siblings:
```
  src/support/memprof.h \
  src/ui/app/memory_panel.h \
```

In `CORE_TEST_SRCS`, add **both** new translation units:
```
  src/support/memprof.c \
  src/ui/app/memory_panel.c \
```
`memory_panel.c` is required here because `tests/test_glr_ctrl.c` includes
`src/app/glr_ctrl.c`, which now calls `ui_memory_panel_render()` - the same
reason `src/ui/app/profile_panel.c` is already in `CORE_TEST_SRCS` (Makefile
line ~473). Without it the controller test will fail to link.

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
- `Makefile` - `SRCS`, `CORE_TEST_SRCS`, `TEST_BINS`, new per-test recipe
- `src/support/prof.h` - add `PROF_MEMORY_PANEL` enum value
- `src/ui/app/profile_panel.h` - move `PROF_PANEL_W` here as `PROFILE_PANEL_W`
  so `memory_panel.c` can reference it for side-by-side layout
- `src/ui/app/profile_panel.c` - `section_label` case for `PROF_MEMORY_PANEL`;
  switch local `PROF_PANEL_W` references to the header-defined `PROFILE_PANEL_W`
- `src/ui/app/state_types.h` - `UiMemoryPanelState` typedef
- `src/ui/app/state.h` / `state.c` - field, accessors, default
- `src/ui/app/snapshot.h` - snapshot field
- `src/app/glr_config.h` - `GLR_CONFIG_MEMORY_PROFILE`
- `src/app/glr_config.c` - config dispatch case
- `src/app/glr_actions.c` - `memory_panel_mode_names[]` array, `g_cfg_items[]` entry
- `src/app/glr_ctrl.c` - includes, `memprof_init()` call, `memprof_frame_tick()`
  call, render block, Ctrl+Shift+M hotkey branch in `glr_ctrl_keyboard`
- `tests/test_glr_ctrl.c` - `ui_memory_panel_render` stub `#define`/`#undef`
  pair + empty stub body; add `PROF_MEMORY_PANEL` to the profile-coverage
  `major[]` array and to the `sum_us` sum

## Verification

1. `make check-include-style` - confirms quoted `"support/memprof.h"` and
   `"ui/app/memory_panel.h"` includes.
2. `make check-c99` - the build guard. New module must compile non-pedantic
   under `gcc -std=c99 -fsyntax-only` with the project's `-I` set.
3. `make test_memprof` (new) - unit tests pass.
4. `make test` - full suite still green.
5. **Real-gcc verification (Linux)**:
   ```
   ssh gracemont 'cd ~/code/openGL/samples/gen-ai/gl-repl && \
     git pull --ff-only origin main && \
     make check-c99 && make test-stubs && make test_memprof'
   ```
   Confirms the Linux `/proc/self/statm` path and the test under real GCC.
6. **Manual GUI** (macOS):
   - `make gl-repl && ./gl-repl`
   - Open Config → toggle "Memory profile" to On. Confirm panel appears
     immediately with **live text rows** (current RSS/VSZ, baseline RSS/VSZ,
     Δ) and an **empty plot area** (no history pushed yet - first sample
     lands at the 5 s mark; that gap is intentional, see Module 1).
   - Wait ~30 s. Confirm 5–6 samples plotted, newest pinned to the right
     edge, X-axis labels reading `-85m … -42m … now`. Confirm Y-axis tick
     labels formatted as MB/GB and the line is visible (not clipped).
   - Press Ctrl+W to toggle CPU profile. Confirm both panels render side by
     side without overlap.
   - Press Ctrl+Shift+M three times. Confirm cycle: On → Details → Off → On.
   - Press Enter (in input row) - confirm new-line insert still works (Ctrl+M
     hotkey must not have hijacked plain Enter).
   - Load a large workspace via `./gl-repl workspace/`. Confirm RSS line
     steps up to a new plateau; Δ row updates.
   - Leave running ~10 min in a corner. Confirm Y axis auto-rescales as
     range changes without visual stutter.
7. **Permission-failure smoke (macOS)**: temporarily edit `memprof_read` to
   force the `kr != KERN_SUCCESS` branch; confirm text rows show `--` and
   graph degrades gracefully (no crash, no `nan`). Revert.
