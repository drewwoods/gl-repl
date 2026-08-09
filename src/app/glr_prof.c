/*
 * glr_prof.c - gl-repl's CPU-profile section display table.
 *
 * The generic timer (src/support/cpuprof.c) and the HUD panel
 * (src/ui/support/cpuprof.c) are section-name agnostic: they key on the
 * opaque ProfSection int and ask the app for each section's display via
 * prof_section_info(). This file is that app-side table for the gl-repl
 * binary - each row is
 * { label, depth, is_total, is_slack, is_frame_total }:
 *   - label    is the bare section name (NO indentation baked in);
 *   - depth    is the nesting level (0 = top-level, 1/2/3 = children),
 *              the single source of truth - the panel derives the visual
 *              indentation from it, so restyling the indent never changes
 *              the nesting classification;
 *   - is_total marks a total row;
 *   - is_slack marks an inversely-colored headroom row;
 *   - is_frame_total marks the one total whose 60 Hz threshold gets refresh
 *     jitter tolerance.
 * Depth reflects true prof_begin/prof_end enclosure (PROF_SNAPSHOT_* nest in
 * PROF_SNAPSHOT, PROF_RENDER3D_OVERLAY_* in _OVERLAYS, etc.). A standalone
 * demo that reuses cpuprof supplies its own prof_section_info.
 */
#include "app/glr_prof.h"
#include "support/cpuprof.h"
#include "support/gpuprof.h"

#include <stddef.h>   /* NULL */

/* Indexed by ProfSection (designated initializers keep label/depth co-located
 * with each enum constant). PROF_RENDER3D_LAST aliases PROF_BUFFER_VIZ_STENCIL,
 * so it shares that row. Any section left out reads back as {NULL,...} and is
 * rendered as "?" by the accessor below. */
static const ProfSectionInfo k_sections[PROF_SECTION_COUNT] = {
    [PROF_RENDER3D]                          = { "Render 3D",       0, 0 },
    [PROF_RENDER3D_SETUP]                    = { "setup",           1, 0 },
    [PROF_RENDER3D_ACCUM_EFFECT]             = { "accum effect",    1, 0 },
    [PROF_RENDER3D_FILL]                     = { "fill",            1, 0 },
    [PROF_RENDER3D_FADE]                     = { "fade batches",    1, 0 },
    [PROF_RENDER3D_FADE_BATCH_PREP]          = { "batch prep",      2, 0 },
    [PROF_RENDER3D_FADE_BATCH_EXEC]          = { "batch exec",      2, 0 },
    [PROF_RENDER3D_FADE_BATCH_POST]          = { "batch post",      2, 0 },
    [PROF_RENDER3D_HELPERS]                  = { "helpers",         1, 0 },
    [PROF_RENDER3D_BACKDROP]                 = { "backdrop",        2, 0 },
    [PROF_RENDER3D_GRID]                     = { "grid",            2, 0 },
    [PROF_RENDER3D_AXES]                     = { "axes",            2, 0 },
    [PROF_RENDER3D_ORBIT_TARGET]             = { "orbit target",    2, 0 },
    [PROF_RENDER3D_LIGHTS]                   = { "lights",          2, 0 },
    [PROF_RENDER3D_OVERLAYS]                 = { "overlays",        1, 0 },
    [PROF_RENDER3D_OVERLAY_OUTLINES]         = { "outlines",        2, 0 },
    [PROF_RENDER3D_OVERLAY_TRANSFORM_GUIDES] = { "xform guides",    2, 0 },
    [PROF_RENDER3D_OVERLAY_NORMALS]          = { "normals",         2, 0 },
    [PROF_RENDER3D_OVERLAY_VERTEX_NUMBERS]   = { "vertex nums",     2, 0 },
    [PROF_RENDER3D_POST_PROCESS]             = { "post FX (scene)", 1, 0 },
    /* Buffer-viz cost is dominated by the synchronous glReadPixels stall
     * plus the CPU convert - CPU-side numbers by nature, so these rows are
     * deliberately NOT in k_gpu_sections below. Stencil reads once per
     * accumulation pass (it composites per pass), depth once per frame, so
     * the two rows are not comparable at accum > 1. */
    [PROF_BUFFER_VIZ_DEPTH]                  = { "depth viz",       1, 0 },
    [PROF_BUFFER_VIZ_STENCIL]                = { "stencil viz",     1, 0 },
    [PROF_CODE_PANEL]                        = { "Code Panel",      0, 0 },
    [PROF_CODE_PANEL_ROWS]                   = { "build rows",      1, 0 },
    [PROF_CODE_PANEL_TEXT]                   = { "draw text",       1, 0 },
    [PROF_CODE_PANEL_TEXT_LAYOUT]            = { "layout",          2, 0 },
    [PROF_CODE_PANEL_TEXT_CHROME]            = { "chrome",          2, 0 },
    [PROF_CODE_PANEL_TEXT_LINES]             = { "lines",           2, 0 },
    [PROF_CODE_PANEL_OVERLAYS]               = { "overlays",        1, 0 },
    [PROF_CODE_PANEL_OVERLAY_TABS]            = { "tabs",            2, 0 },
    [PROF_CODE_PANEL_OVERLAY_MENU]            = { "menu",            2, 0 },
    [PROF_CODE_PANEL_OVERLAY_MENU_LABELS]     = { "labels",          3, 0 },
    [PROF_CODE_PANEL_OVERLAY_MENU_PINS]       = { "pins",            3, 0 },
    [PROF_CODE_PANEL_OVERLAY_SEARCH]          = { "search",          2, 0 },
    [PROF_CODE_PANEL_OVERLAY_STATUS]          = { "status",          2, 0 },
    [PROF_CODE_PANEL_OVERLAY_STATUS_TEXT]     = { "text",            3, 0 },
    [PROF_CODE_PANEL_OVERLAY_STATUS_ACTIONS]  = { "actions",         3, 0 },
    [PROF_CODE_PANEL_OVERLAY_PICKER]          = { "picker",          2, 0 },
    [PROF_CODE_PANEL_OVERLAY_SWATCH]          = { "num swatch",      2, 0 },
    [PROF_UI_PANELS]                         = { "UI Panels",       0, 0 },
    [PROF_SNAPSHOT]                          = { "Snapshot",        0, 0 },
    [PROF_SNAPSHOT_TRANSFORMERS]             = { "transformers",    1, 0 },
    [PROF_SNAPSHOT_HIGHLIGHTS]               = { "highlights",      1, 0 },
    [PROF_SNAPSHOT_VIRTUAL_LINES]            = { "virtual lines",   1, 0 },
    [PROF_SNAPSHOT_PREP]                     = { "prep",            1, 0 },
    [PROF_SNAPSHOT_SCENE_CONFIG]             = { "scene config",    1, 0 },
    [PROF_SNAPSHOT_UI]                       = { "ui snapshot",     1, 0 },
    [PROF_FLATTEN]                           = { "Flatten",         0, 0 },
    [PROF_FLATTEN_REPARSE]                   = { "reparse",         1, 0 },
    [PROF_FLATTEN_VAR_ASSIGN]                = { "var assign",      1, 0 },
    [PROF_FLATTEN_SCRATCH_ASSIGN]            = { "scratch assign",  1, 0 },
    [PROF_REBAKE]                            = { "Rebake",          0, 0 },
    [PROF_REBAKE_EVAL]                       = { "eval walk",        1, 0 },
    [PROF_REFORMAT]                          = { "Reformat",        0, 0 },
    [PROF_AUTONORMAL]                        = { "Autonormal",      0, 0 },
    [PROF_REPLAY_HUD]                        = { "Replay HUD",      0, 0 },
    [PROF_TOUR_HUD]                          = { "Tour HUD",        0, 0 },
    [PROF_PROFILE_PANEL]                     = { "Profile Panel",   0, 0 },
    [PROF_PROFILE_PANEL_FPS]                 = { "fps plot",        1, 0 },
    [PROF_PROFILE_PANEL_SECTIONS]            = { "section list",    1, 0 },
    [PROF_PROFILE_PANEL_HISTOGRAM]           = { "histograms",      1, 0 },
    [PROF_MEMORY_PANEL]                      = { "Memory Panel",    0, 0 },
    [PROF_ASSIGN_PLOT]                       = { "Assign Plot",     0, 0 },
    [PROF_ASSIGN_PLOT_CAPTURE]               = { "capture",         1, 0 },
    [PROF_ASSIGN_PLOT_PANEL]                 = { "panel",           1, 0 },
    [PROF_COMPOSITOR]                        = { "Compositor FX",   0, 0 },
    [PROF_FRAME_RESTORE]                     = { "Frame Restore",   0, 0 },
    [PROF_SCRIPTED_INPUT]                    = { "Scripted Input",  0, 0 },
    [PROF_HOST_OVERLAYS]                     = { "Host Overlays",   0, 0 },
    [PROF_HOST_SPLASH]                       = { "splash",          1, 0 },
    [PROF_TOUR_OVERLAY]                      = { "tour overlay",    1, 0 },
    [PROF_TOUR_PRESENCE]                     = { "tour presence",   1, 0 },
    /* The summary rows under the divider: the whole frame, then the parts it
     * splits into. "Frame Time" carries is_total (the divider above it and the
     * full-budget thresholds) plus is_frame_total (refresh-boundary tolerance);
     * "Present" is the vsync wait, colored inversely because a long one is
     * headroom rather than cost (is_slack).
     *
     * All four are depth 0. "Depth Snapshot" is a *sibling* here, not a child of
     * anything: its capture runs after both Host Overlays and Frame Work have
     * closed, so indenting it under either would read as a 2 ms child of a 9 us
     * row, and Present is derived by subtracting it out. */
    [PROF_FRAME_TOTAL]                       = { "Frame Time",      0, 1, 0, 1 },
    [PROF_FRAME_WORK]                        = { "Frame Work",      0, 0, 0 },
    [PROF_DEPTH_SNAPSHOT]                    = { "Depth Snapshot",  0, 0 },
    [PROF_PRESENT]                           = { "Present",         0, 0, 1 },
};

ProfSectionInfo prof_section_info(ProfSection s) {
    if (s >= 0 && s < PROF_SECTION_COUNT && k_sections[s].label != NULL)
        return k_sections[s];
    ProfSectionInfo unknown = { "?", 0, 0, 0, 0 };
    return unknown;
}

/* The GPU-bracketed subset: sections whose body emits GL calls, marked 1
 * so the cpuprof hooks below wrap them in GL_TIME_ELAPSED queries. Left
 * out (0): the pure-CPU sections (snapshot/flatten/reformat/autonormal/
 * frame-restore, code-panel layout precompute) and the per-fade-batch
 * sub-sections - one segment per batch per accumulation pass would swamp
 * gpuprof's per-frame query budget while PROF_RENDER3D_FADE already
 * covers the whole pass. */
static const unsigned char k_gpu_sections[PROF_SECTION_COUNT] = {
    [PROF_RENDER3D]                          = 1,
    [PROF_RENDER3D_SETUP]                    = 1,
    [PROF_RENDER3D_ACCUM_EFFECT]             = 1,
    [PROF_RENDER3D_FILL]                     = 1,
    [PROF_RENDER3D_FADE]                     = 1,
    [PROF_RENDER3D_HELPERS]                  = 1,
    [PROF_RENDER3D_BACKDROP]                 = 1,
    [PROF_RENDER3D_GRID]                     = 1,
    [PROF_RENDER3D_AXES]                     = 1,
    [PROF_RENDER3D_ORBIT_TARGET]             = 1,
    [PROF_RENDER3D_LIGHTS]                   = 1,
    [PROF_RENDER3D_OVERLAYS]                 = 1,
    [PROF_RENDER3D_OVERLAY_OUTLINES]         = 1,
    [PROF_RENDER3D_OVERLAY_TRANSFORM_GUIDES] = 1,
    [PROF_RENDER3D_OVERLAY_NORMALS]          = 1,
    [PROF_RENDER3D_OVERLAY_VERTEX_NUMBERS]   = 1,
    [PROF_RENDER3D_POST_PROCESS]             = 1,
    /* build rows / layout are pure-CPU phases (no GL emitted) - excluded. */
    [PROF_CODE_PANEL]                        = 1,
    [PROF_CODE_PANEL_TEXT]                   = 1,
    [PROF_CODE_PANEL_TEXT_CHROME]            = 1,
    [PROF_CODE_PANEL_TEXT_LINES]             = 1,
    [PROF_CODE_PANEL_OVERLAYS]               = 1,
    /* Overlay children are diagnostic CPU subdivisions. The parent already
     * owns the GPU query; nesting ten more queries would perturb the work
     * these rows are intended to explain, especially in browser builds. */
    [PROF_UI_PANELS]                         = 1,
    [PROF_REPLAY_HUD]                        = 1,
    [PROF_TOUR_HUD]                          = 1,
    [PROF_PROFILE_PANEL]                     = 1,
    /* Profile-panel children are diagnostic CPU subdivisions. Their parent
     * owns the GPU query so the overlay does not nest timer queries while it
     * is rendering the profiler's own measurements. */
    [PROF_MEMORY_PANEL]                      = 1,
    /* Only the assignment plot's panel leaf draws. Its parent spans the
     * flat-program scan as well - pure CPU, and accumulated across two
     * separate points in the frame - so a query there would bracket work
     * that issues no GL at all. */
    [PROF_ASSIGN_PLOT_PANEL]                 = 1,
    [PROF_COMPOSITOR]                        = 1,
    /* Host-band stages: the overlay aggregate draws (splash banner, tour
     * cursor/caption), so it owns a query; its two children stay CPU-only
     * diagnostic subdivisions like every other leaf under a drawing parent.
     * Scripted input issues no GL of its own.
     *
     * Of the three summary rows only Frame Work carries a query. Frame Time
     * runs to the end of the callback, past a glFinish that has already
     * drained the queue, so its query would report the vsync wait as GPU time;
     * Present is derived arithmetic with no span to bracket at all. Frame Work
     * ends exactly where the GPU work does, which is the number worth having. */
    [PROF_HOST_OVERLAYS]                     = 1,
    [PROF_FRAME_WORK]                        = 1,
};

int glr_prof_section_is_gpu(ProfSection s) {
    if (s < 0 || s >= PROF_SECTION_COUNT) return 0;
    return k_gpu_sections[s];
}

static GlrProfGpuCaptureMode g_gpu_capture_mode = GLR_PROF_GPU_CAPTURE_ALL;

void glr_prof_set_gpu_capture_mode(GlrProfGpuCaptureMode mode) {
    g_gpu_capture_mode = mode;
}

/* Capture-mode gate for the hooks. The mode only changes at frame top
 * (before any begin), so begin/end always agree within a frame. */
static int glr_prof_gpu_capture_allows(ProfSection s) {
    if (!k_gpu_sections[s]) return 0;
    if (g_gpu_capture_mode == GLR_PROF_GPU_CAPTURE_OFF) return 0;
    return 1;
}

static void glr_prof_gpu_begin_hook(ProfSection s) {
    if (glr_prof_gpu_capture_allows(s)) gpu_prof_begin(s);
}

static void glr_prof_gpu_end_hook(ProfSection s) {
    if (glr_prof_gpu_capture_allows(s)) gpu_prof_end(s);
}

/* The depth column, as the lookup cpuprof's nesting guard wants. Same table the
 * panel indents from, so the guard checks exactly the tree the panel draws. */
static int glr_prof_section_depth(ProfSection s) {
    return prof_section_info(s).depth;
}

void glr_prof_install_nesting_guard(void) {
    prof_install_section_depth_fn(glr_prof_section_depth);
}

void glr_prof_install_gpu_section_hooks(void) {
    prof_install_section_hooks(glr_prof_gpu_begin_hook, glr_prof_gpu_end_hook);
}
