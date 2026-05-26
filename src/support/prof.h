/*
 * prof.h - CPU overhead profiling instrumentation.
 *
 * Measures elapsed wall time spent in major per-frame sections.
 * Instrumentation is generic and does not depend on the UI layer.
 */
#ifndef PROF_H
#define PROF_H

/* Sections that are timed each frame (or whenever they run). */
typedef enum {
    PROF_SCENE_3D = 0,  /* render_3d_scene() */
    PROF_SCENE_3D_SETUP,     /* projection/camera/lights/material setup */
    PROF_SCENE_3D_FILL,      /* execute_commands() main fill pass */
    PROF_SCENE_3D_FADE,      /* replay fade batches pass */
    PROF_SCENE_3D_FADE_BATCH_PREP,  /* per-batch find-open + color */
    PROF_SCENE_3D_FADE_BATCH_EXEC,  /* per-batch execute_commands */
    PROF_SCENE_3D_FADE_BATCH_POST,  /* per-batch post-execute cleanup */
    PROF_SCENE_3D_HELPERS,      /* backdrop/grid/axes/orbit-target aggregate */
    PROF_SCENE_3D_BACKDROP,     /* scene_backdrop_render() – stale when off */
    PROF_SCENE_3D_GRID,         /* scene_grid_render() */
    PROF_SCENE_3D_AXES,         /* scene_axes_render() */
    PROF_SCENE_3D_ORBIT_TARGET, /* draw_orbit_target() */
    PROF_SCENE_3D_OVERLAYS,  /* vertex dots, vertex/normal/transform guides */
    PROF_SCENE_3D_OVERLAY_OUTLINES,  /* polygon outline + current-block highlight */
    PROF_SCENE_3D_OVERLAY_BUILD_GUIDES,  /* transform-editing gizmos (translate/rotate/scale) */
    PROF_SCENE_3D_OVERLAY_TRANSFORM_GUIDES,  /* transform-editing gizmos (translate/rotate/scale) */
    PROF_SCENE_3D_OVERLAY_NORMALS,        /* normal vector labels */
    PROF_SCENE_3D_OVERLAY_VERTEX_NUMBERS,    /* vertex numbers labels */
    PROF_SCENE_3D_POST_PROCESS,   /* scene_postprocess_filter_render() */
    PROF_SCENE_3D_LAST = PROF_SCENE_3D_POST_PROCESS,
    PROF_CODE_PANEL,    /* render_code_panel() */
    PROF_CODE_PANEL_LAYOUT,   /* render_code_panel() layout/precompute */
    PROF_CODE_PANEL_CHROME,   /* background, border, header/search chrome */
    PROF_CODE_PANEL_LINES,    /* header/body/footer line rendering */
    PROF_CODE_PANEL_LINES_STATIC,  /* workspace/header static rows */
    PROF_CODE_PANEL_LINES_BODY,    /* command + insert/newline body rows */
    PROF_CODE_PANEL_LINES_BODY_CMDS,    /* command loop body */
    PROF_CODE_PANEL_LINES_BODY_NEWLINE, /* newline slot body */
    PROF_CODE_PANEL_LINES_FOOTER,  /* footer/static trailing rows */
    PROF_CODE_PANEL_OVERLAYS, /* scroll/status/color-picker overlays */
    PROF_UI_PANELS,     /* autocomplete + dropdown + var + config + help */
    PROF_SNAPSHOT,                 /* aggregate snapshot production by controller */
    PROF_SNAPSHOT_TRANSFORMERS,    /* push_color_transformers (per-line scan) */
    PROF_SNAPSHOT_HIGHLIGHTS,      /* push_highlights (feeding cmd + replay PC) */
    PROF_SNAPSHOT_VIRTUAL_LINES,   /* annotations_prepare + virtual-line refresh */
    PROF_SNAPSHOT_PREP,            /* replay_prepare_frame + render-state /
                                    * camera string refresh that sits between
                                    * virtual-line refresh and scene-config */
    PROF_SNAPSHOT_SCENE_CONFIG,    /* build_scene_config */
    PROF_SNAPSHOT_UI,              /* build_ui_snapshot */
    PROF_FLATTEN,       /* flatten_commands() (only when dirty) */
    PROF_REFORMAT,      /* repl_reformat_program() (on demand) */
    PROF_AUTONORMAL,    /* recompute_autonormals() (only when dirty) */
    PROF_REPLAY_HUD,    /* replay_ui_hud_render() (only when replaying) */
    PROF_PROFILE_PANEL, /* ui_profile_panel_render() (the panel itself) */
    PROF_MEMORY_PANEL,  /* ui_memory_panel_render() (the panel itself) */
    PROF_FRAME_RESTORE, /* post-render flat-count + predef-value restore */
    PROF_FRAME_TOTAL,   /* entire display callback */
    PROF_SECTION_COUNT
} ProfSection;

/* Begin/end a named CPU-time measurement.
 * prof_begin stores the current process CPU clock; prof_end records the
 * elapsed time and updates the running average for section s. */
void prof_begin(ProfSection s);
void prof_end(ProfSection s);

/* Accumulation-aware timing helpers for sections called inside a multi-pass
 * loop (e.g. accumulation-buffer AA).
 *
 * Usage:
 *   prof_accum_reset(s);          // once, before the loop
 *   for (...) {
 *       prof_begin(s);
 *       ...work...
 *       prof_accum_end(s);        // adds elapsed to a running total
 *   }
 *   prof_accum_commit(s);         // stores total as last_us, updates EMA
 */
void prof_accum_reset(ProfSection s);
void prof_accum_end(ProfSection s);
void prof_accum_commit(ProfSection s);

/* Mark the start of a new frame so per-frame sections can detect staleness. */
void prof_frame_tick(void);

/* Read-only API for HUD rendering. */
double prof_section_last_us(ProfSection s);
double prof_section_avg_us(ProfSection s);
int    prof_section_is_stale(ProfSection s);

#endif /* PROF_H */
