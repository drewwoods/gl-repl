/*
 * profile_panel.h — CPU overhead profiling overlay panel.
 *
 * Measures CPU time spent in the major per-frame sections and renders a
 * compact overlay showing the last measured time and a smoothed average.
 * Toggle with Ctrl+W.
 */
#ifndef PROFILE_PANEL_H
#define PROFILE_PANEL_H

/* Sections that are timed each frame (or whenever they run). */
typedef enum {
    PROF_SCENE_3D = 0,  /* render_3d_scene() */
    PROF_SCENE_3D_SETUP,     /* projection/camera/lights/material setup */
    PROF_SCENE_3D_FILL,      /* execute_commands() main fill pass */
    PROF_SCENE_3D_FADE,      /* replay fade batches pass */
    PROF_SCENE_3D_HELPERS,   /* backdrop/grid/axes/orbit-target */
    PROF_SCENE_3D_OUTLINES,  /* polygon outline + current-block highlight */
    PROF_SCENE_3D_OVERLAYS,  /* vertex dots, vertex/normal/transform guides */
    PROF_SCENE_3D_HUD,       /* lights, vertex nums, normals, replay HUD */
    PROF_CODE_PANEL,    /* render_code_panel() */
    PROF_CODE_PANEL_LAYOUT,   /* render_code_panel() layout/precompute */
    PROF_CODE_PANEL_LAYOUT_GEOM,   /* panel geom + row precompute + line totals */
    PROF_CODE_PANEL_LAYOUT_GEOM_SETUP,      /* workspace refresh + panel geometry */
    PROF_CODE_PANEL_LAYOUT_GEOM_PRECOMPUTE, /* wrap/precompute row counts */
    PROF_CODE_PANEL_LAYOUT_GEOM_TOTALS,     /* total line accumulation pass */
    PROF_CODE_PANEL_LAYOUT_CURSOR, /* cursor/replay doc-line mapping */
    PROF_CODE_PANEL_LAYOUT_SCROLL, /* scroll clamping + follow-cursor updates */
    PROF_CODE_PANEL_CHROME,   /* background, border, header/search chrome */
    PROF_CODE_PANEL_LINES,    /* header/body/footer line rendering */
    PROF_CODE_PANEL_LINES_STATIC,  /* workspace/header static rows */
    PROF_CODE_PANEL_LINES_BODY,    /* command + insert/newline body rows */
    PROF_CODE_PANEL_LINES_BODY_CMDS,    /* command loop body */
    PROF_CODE_PANEL_LINES_BODY_NEWLINE, /* newline slot body */
    PROF_CODE_PANEL_LINES_FOOTER,  /* footer/static trailing rows */
    PROF_CODE_PANEL_OVERLAYS, /* scroll/status/color-picker overlays */
    PROF_UI_PANELS,     /* autocomplete + dropdown + var + config + help */
    PROF_FLATTEN,       /* flatten_commands() (only when dirty) */
    PROF_REFORMAT,      /* repl_reformat_commands() (on demand) */
    PROF_FRAME_TOTAL,   /* entire display callback */
    PROF_SECTION_COUNT
} ProfSection;

/* Begin/end a named CPU-time measurement.
 * prof_begin stores the current process CPU clock; prof_end records the
 * elapsed time and updates the running average for section s. */
void prof_begin(ProfSection s);
void prof_end(ProfSection s);

/* Mark the start of a new frame so per-frame sections can detect staleness. */
void prof_frame_tick(void);

/* Render the profile panel overlay.  Draws nothing when
 * g_show_profile_panel == PROFILE_PANEL_OFF. */
void render_profile_panel(void);

/* Whether detail rows should be rendered/timed for PROF_CODE_PANEL. */
int prof_code_panel_details_enabled(void);

#endif /* PROFILE_PANEL_H */
