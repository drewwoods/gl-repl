/*
 * prof_sections.h - CPU-profile section catalog (the app's ProfSection enum).
 *
 * The per-frame timing sections are instrumented (prof_begin/prof_end) from
 * across the tree — src/scene, src/subsystems, src/repl, src/app — none of
 * which include each other. So the section IDs live in this one tiny header,
 * force-included into every translation unit by the Makefile's OBJ_CFLAGS
 * (`-include prof_sections.h`, right beside `-include config.h`). That keeps
 * the generic timers/panel (src/support/cpuprof.{c,h}, src/support/gpuprof.{c,h},
 * src/ui/support/cpuprof.c) free of any app section names: support/cpuprof.h
 * falls back to an opaque
 * `typedef int ProfSection` when this catalog is absent (see its #ifndef
 * PROF_SECTIONS_PROVIDED guard), so those four files compile and link with no
 * dependency on this file at all — the build just injects a richer catalog.
 *
 * Defining PROF_SECTIONS_PROVIDED is the signal cpuprof.h checks to suppress
 * its fallback. Because this header is force-included *before* any TU's own
 * `#include "support/cpuprof.h"`, the guard is always set first.
 *
 * The per-section *display* (label / nesting depth / is-total flag) is NOT
 * here — it is supplied per binary via prof_section_info() (src/app/glr_prof.c
 * for gl-repl; the driver itself for the standalone demos). A different app
 * reusing cpuprof ships its own prof_sections.h with its own section list.
 */
#ifndef PROF_SECTIONS_H
#define PROF_SECTIONS_H

#define PROF_SECTIONS_PROVIDED 1   /* tells support/cpuprof.h to skip its fallback */

/* Sections that are timed each frame (or whenever they run). */
typedef enum {
    PROF_RENDER3D_3D = 0,  /* render_3d_scene() */
    PROF_RENDER3D_3D_SETUP,     /* projection/camera/lights/material setup */
    PROF_RENDER3D_3D_FILL,      /* execute_commands() main fill pass */
    PROF_RENDER3D_3D_FADE,      /* replay fade batches pass */
    PROF_RENDER3D_3D_FADE_BATCH_PREP,  /* per-batch find-open + color */
    PROF_RENDER3D_3D_FADE_BATCH_EXEC,  /* per-batch execute_commands */
    PROF_RENDER3D_3D_FADE_BATCH_POST,  /* per-batch post-execute cleanup */
    PROF_RENDER3D_3D_HELPERS,      /* backdrop/grid/axes/orbit-target aggregate */
    PROF_RENDER3D_3D_BACKDROP,     /* scene_backdrop_render() – stale when off */
    PROF_RENDER3D_3D_GRID,         /* scene_grid_render() */
    PROF_RENDER3D_3D_AXES,         /* scene_axes_render() */
    PROF_RENDER3D_3D_ORBIT_TARGET, /* draw_orbit_target() */
    PROF_RENDER3D_3D_OVERLAYS,  /* vertex dots, vertex/normal/transform guides */
    PROF_RENDER3D_3D_OVERLAY_OUTLINES,  /* polygon outline + current-block highlight */
    PROF_RENDER3D_3D_OVERLAY_TRANSFORM_GUIDES,  /* transform-editing gizmos (translate/rotate/scale) */
    PROF_RENDER3D_3D_OVERLAY_NORMALS,        /* normal vector labels */
    PROF_RENDER3D_3D_OVERLAY_VERTEX_NUMBERS,    /* vertex numbers labels */
    PROF_RENDER3D_3D_POST_PROCESS,   /* scene_postprocess_filter_render() */
    PROF_RENDER3D_3D_LAST = PROF_RENDER3D_3D_POST_PROCESS,
    PROF_CODE_PANEL,    /* ui_repl_code_panel_render_with_chrome() */
    PROF_CODE_PANEL_ROWS,     /* adapter row/segment build from REPL state */
    PROF_CODE_PANEL_TEXT,     /* generic ui_text_panel_render() */
    PROF_CODE_PANEL_TEXT_LAYOUT, /* wrap-count cache + viewport metrics */
    PROF_CODE_PANEL_TEXT_CHROME, /* background/border/divider + scrollbar */
    PROF_CODE_PANEL_TEXT_LINES,  /* row draw loop */
    PROF_CODE_PANEL_OVERLAYS, /* tabs/menubar/search/statusbar/picker/swatch */
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
    PROF_FLATTEN_REPARSE,       /* re-parse + arg-expression eval of GL command lines */
    PROF_FLATTEN_VAR_ASSIGN,    /* scalar `name = expr` RHS eval */
    PROF_FLATTEN_SCRATCH_ASSIGN,/* `A[i] = expr` index + RHS eval */
    PROF_REFORMAT,      /* repl_reformat_program() (on demand) */
    PROF_AUTONORMAL,    /* recompute_autonormals() (only when dirty) */
    PROF_REPLAY_HUD,    /* replay_ui_hud_render() (only when replaying) */
    PROF_PROFILE_PANEL, /* ui_profile_panel_render() (the panel itself) */
    PROF_MEMORY_PANEL,  /* ui_memory_panel_render() (the panel itself) */
    PROF_COMPOSITOR,    /* glr_compositor_postprocess_frame() — whole-frame
                         * (full-screen) post-process; distinct from the
                         * scene-viewport pass PROF_RENDER3D_3D_POST_PROCESS */
    PROF_FRAME_RESTORE, /* post-render flat-count + predef-value restore */
    PROF_FRAME_TOTAL,   /* entire display callback */
    PROF_SECTION_COUNT
} ProfSection;

#endif /* PROF_SECTIONS_H */
