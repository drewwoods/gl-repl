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
    PROF_CODE_PANEL,    /* render_code_panel() */
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
 * g_show_profile_panel == 0. */
void render_profile_panel(void);

#endif /* PROFILE_PANEL_H */
