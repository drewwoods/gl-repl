/*
 * glr_prof.c - gl-repl's CPU-profile section display table.
 *
 * The generic timer (src/support/cpuprof.c) and the HUD panel
 * (src/ui/support/cpuprof.c) are section-name agnostic: they key on the
 * opaque ProfSection int and ask the app for each section's display via
 * prof_section_info(). This file is that app-side table for the gl-repl
 * binary — each row is { label, depth, is_total }:
 *   - label    is the bare section name (NO indentation baked in);
 *   - depth    is the nesting level (0 = top-level, 1/2/3 = children),
 *              the single source of truth — the panel derives the visual
 *              indentation from it, so restyling the indent never changes
 *              the nesting classification;
 *   - is_total marks the one whole-frame total row.
 * Depth reflects true prof_begin/prof_end enclosure (PROF_SNAPSHOT_* nest in
 * PROF_SNAPSHOT, PROF_SCENE_3D_OVERLAY_* in _OVERLAYS, etc.). A standalone
 * demo that reuses cpuprof supplies its own prof_section_info.
 */
#include "support/cpuprof.h"

#include <stddef.h>   /* NULL */

/* Indexed by ProfSection (designated initializers keep label/depth co-located
 * with each enum constant). PROF_SCENE_3D_LAST aliases _POST_PROCESS, so it
 * shares that row. Any section left out reads back as {NULL,...} and is
 * rendered as "?" by the accessor below. */
static const ProfSectionInfo k_sections[PROF_SECTION_COUNT] = {
    [PROF_SCENE_3D]                          = { "Scene 3D",        0, 0 },
    [PROF_SCENE_3D_SETUP]                    = { "setup",           1, 0 },
    [PROF_SCENE_3D_FILL]                     = { "fill",            1, 0 },
    [PROF_SCENE_3D_FADE]                     = { "fade batches",    1, 0 },
    [PROF_SCENE_3D_FADE_BATCH_PREP]          = { "batch prep",      2, 0 },
    [PROF_SCENE_3D_FADE_BATCH_EXEC]          = { "batch exec",      2, 0 },
    [PROF_SCENE_3D_FADE_BATCH_POST]          = { "batch post",      2, 0 },
    [PROF_SCENE_3D_HELPERS]                  = { "helpers",         1, 0 },
    [PROF_SCENE_3D_BACKDROP]                 = { "backdrop",        2, 0 },
    [PROF_SCENE_3D_GRID]                     = { "grid",            2, 0 },
    [PROF_SCENE_3D_AXES]                     = { "axes",            2, 0 },
    [PROF_SCENE_3D_ORBIT_TARGET]             = { "orbit target",    2, 0 },
    [PROF_SCENE_3D_OVERLAYS]                 = { "overlays",        1, 0 },
    [PROF_SCENE_3D_OVERLAY_OUTLINES]         = { "outlines",        2, 0 },
    [PROF_SCENE_3D_OVERLAY_BUILD_GUIDES]     = { "build guides",    2, 0 },
    [PROF_SCENE_3D_OVERLAY_TRANSFORM_GUIDES] = { "xform guides",    2, 0 },
    [PROF_SCENE_3D_OVERLAY_NORMALS]          = { "normals",         2, 0 },
    [PROF_SCENE_3D_OVERLAY_VERTEX_NUMBERS]   = { "vertex nums",     2, 0 },
    [PROF_SCENE_3D_POST_PROCESS]             = { "post FX (scene)", 1, 0 },
    [PROF_CODE_PANEL]                        = { "Code Panel",      0, 0 },
    [PROF_CODE_PANEL_LAYOUT]                 = { "layout",          1, 0 },
    [PROF_CODE_PANEL_CHROME]                 = { "chrome",          1, 0 },
    [PROF_CODE_PANEL_LINES]                  = { "lines",           1, 0 },
    [PROF_CODE_PANEL_LINES_STATIC]           = { "static",          2, 0 },
    [PROF_CODE_PANEL_LINES_BODY]             = { "body",            2, 0 },
    [PROF_CODE_PANEL_LINES_BODY_CMDS]        = { "commands",        3, 0 },
    [PROF_CODE_PANEL_LINES_BODY_NEWLINE]     = { "newline",         3, 0 },
    [PROF_CODE_PANEL_LINES_FOOTER]           = { "footer",          2, 0 },
    [PROF_CODE_PANEL_OVERLAYS]               = { "overlays",        1, 0 },
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
    [PROF_REFORMAT]                          = { "Reformat",        0, 0 },
    [PROF_AUTONORMAL]                        = { "Autonormal",      0, 0 },
    [PROF_REPLAY_HUD]                        = { "Replay HUD",      0, 0 },
    [PROF_PROFILE_PANEL]                     = { "Profile Panel",   0, 0 },
    [PROF_MEMORY_PANEL]                      = { "Memory Panel",    0, 0 },
    [PROF_COMPOSITOR]                        = { "Compositor FX",   0, 0 },
    [PROF_FRAME_RESTORE]                     = { "Frame Restore",   0, 0 },
    [PROF_FRAME_TOTAL]                       = { "Frame Total",     0, 1 },
};

ProfSectionInfo prof_section_info(ProfSection s) {
    if (s >= 0 && s < PROF_SECTION_COUNT && k_sections[s].label != NULL)
        return k_sections[s];
    ProfSectionInfo unknown = { "?", 0, 0 };
    return unknown;
}
