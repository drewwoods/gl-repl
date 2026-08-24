/*
 * prof_sections.h - CPU-profile section catalog (the app's ProfSection enum).
 *
 * The per-frame timing sections are instrumented (prof_begin/prof_end) from
 * across the tree - src/render3d, src/subsystems, src/repl, src/app - none of
 * which include each other. So the section IDs live in this one tiny header,
 * force-included into every translation unit by the Makefile's OBJ_CFLAGS
 * (`-include prof_sections.h`, right beside `-include config.h`). That keeps
 * the generic timers/panel (src/support/cpuprof.{c,h}, src/support/gpuprof.{c,h},
 * src/ui/support/cpuprof.c) free of any app section names: support/cpuprof.h
 * falls back to an opaque
 * `typedef int ProfSection` when this catalog is absent (see its #ifndef
 * PROF_SECTIONS_PROVIDED guard), so those four files compile and link with no
 * dependency on this file at all - the build just injects a richer catalog.
 *
 * Defining PROF_SECTIONS_PROVIDED is the signal cpuprof.h checks to suppress
 * its fallback. Because this header is force-included *before* any TU's own
 * `#include "support/cpuprof.h"`, the guard is always set first.
 *
 * The per-section *display* (label / nesting depth / is-total flag) is NOT
 * here - it is supplied per binary via prof_section_info() (src/app/glr_prof.c
 * for gl-repl; the driver itself for the standalone demos). A different app
 * reusing cpuprof ships its own prof_sections.h with its own section list.
 */
#ifndef PROF_SECTIONS_H
#define PROF_SECTIONS_H

#define PROF_SECTIONS_PROVIDED 1   /* tells support/cpuprof.h to skip its fallback */

/* Sections that are timed each frame (or whenever they run). */
typedef enum {
    PROF_RENDER3D = 0,  /* render3d_draw_scene() + the host chrome-strip clear */
    PROF_RENDER3D_SETUP,     /* projection/camera/lights/material setup */
    PROF_RENDER3D_ACCUM_EFFECT, /* accumulation load/add/resolve + blur subframe prep */
    PROF_RENDER3D_FILL,      /* execute_commands() main fill pass */
    PROF_RENDER3D_FADE,      /* replay fade batches pass */
    PROF_RENDER3D_FADE_BATCH_PREP,  /* per-batch find-open + color */
    PROF_RENDER3D_FADE_BATCH_EXEC,  /* per-batch execute_commands */
    PROF_RENDER3D_FADE_BATCH_POST,  /* per-batch post-execute cleanup */
    PROF_RENDER3D_HELPERS,      /* backdrop/grid/axes/orbit-target/lights aggregate */
    PROF_RENDER3D_BACKDROP,     /* render3d_backdrop_render() - stale when off */
    PROF_RENDER3D_GRID,         /* render3d_grid_render() */
    PROF_RENDER3D_AXES,         /* render3d_axes_render() */
    PROF_RENDER3D_ORBIT_TARGET, /* draw_orbit_target() */
    PROF_RENDER3D_LIGHTS,       /* render3d_lights_render() light-slot gizmos */
    PROF_RENDER3D_OVERLAYS,  /* vertex dots, vertex/normal/transform guides */
    PROF_RENDER3D_OVERLAY_OUTLINES,  /* polygon outline + current-block highlight */
    PROF_RENDER3D_OVERLAY_TRANSFORM_GUIDES,  /* transform-editing gizmos (translate/rotate/scale) */
    PROF_RENDER3D_OVERLAY_NORMALS,        /* normal vector labels */
    PROF_RENDER3D_OVERLAY_VERTEX_NUMBERS,    /* vertex numbers labels */
    PROF_RENDER3D_POST_PROCESS,   /* render3d_postprocess_filter_render() */
    /* Buffer visualization lives in src/subsystems/buffer_viz/, but its
     * sections stay INSIDE the render3d index range: the profile panel's
     * "Render 3D" aggregate is an index sweep (PROF_RENDER3D_SETUP ..
     * PROF_RENDER3D_LAST), and the work still happens inside that frame
     * phase, so moving the constants out would silently change what the
     * render3d row reports. */
    PROF_BUFFER_VIZ_DEPTH,      /* depth-viz readback + convert + quad */
    PROF_BUFFER_VIZ_STENCIL,    /* stencil-viz readback + convert + quad */
    PROF_RENDER3D_LAST = PROF_BUFFER_VIZ_STENCIL,
    PROF_SNAPSHOT,                 /* aggregate snapshot production by controller */
    PROF_SNAPSHOT_TRANSFORMERS,    /* push_color_transformers (per-line scan) */
    PROF_SNAPSHOT_HIGHLIGHTS,      /* push_highlights (feeding cmd + replay PC) */
    PROF_SNAPSHOT_VIRTUAL_LINES,   /* annotations_prepare + virtual-line refresh */
    PROF_SNAPSHOT_PREP,            /* replay_prepare_frame + render-state /
                                    * camera string refresh that sits between
                                    * virtual-line refresh and scene-config */
    /* Two flat-program scans that run inside the prep span above. They are
     * rows because per-frame work inside someone else's bracket shows up only
     * as unattributed remainder, and they are *here* because this is where
     * they run - each belongs to a feature whose panel draws much later, but a
     * section is a place in the frame, not a feature. (They were children of
     * feature-shaped roots that overlapped this very span, which is the double
     * count this placement removes.) */
    PROF_ASSIGN_PLOT_MARKERS,      /* assign_plot_exec_progress() replay PC scan */
    PROF_CONSOLE_CAPTURE,          /* console_capture() flat-program scan */
    PROF_SNAPSHOT_SCENE_CONFIG,    /* build_scene_config */
    PROF_SNAPSHOT_UI,              /* build_ui_snapshot */
    PROF_FLATTEN,       /* flatten_commands() (only when dirty) */
    PROF_FLATTEN_REPARSE,       /* re-parse + arg-expression eval of GL command lines */
    PROF_FLATTEN_VAR_ASSIGN,    /* scalar `name = expr` RHS eval */
    PROF_FLATTEN_SCRATCH_ASSIGN,/* `A[i] = expr` index + RHS eval */
    PROF_REBAKE,        /* repl_flatten_rebake_commands() (value-only dirt) */
    PROF_REBAKE_EVAL,   /* existing flat-stream expression eval + arg writes */
    PROF_REFORMAT,      /* repl_reformat_program() (on demand) */
    PROF_AUTONORMAL,    /* recompute_autonormals() (only when dirty) */
    /* The assignment plot's own flat-program scan, which runs after the flatten
     * refresh and before the snapshot opens - so it is nobody's child. Its two
     * siblings live where they run: the replay-marker scan under the snapshot's
     * prep, the panel under the UI band. Every one of the three stays stale
     * while no assignment row is plotted, which is the honest reading: the
     * feature costs nothing when it is closed. */
    PROF_ASSIGN_PLOT_CAPTURE,   /* assign_plot_capture() flat-program scan */
    /* The 2D UI band - everything drawn over the scene between
     * render3d_draw_scene() and the compositor pass, bracketed as one span in
     * glr_ctrl_display_frame(). It exists for the same reason PROF_RENDER3D
     * does: the individual panels were a flat run of a dozen top-level rows
     * with no row answering "what does the interface cost this frame", and no
     * row absorbing the uninstrumented draws between them (the buffer-viz
     * legend, the status-history popup) - those showed up only as
     * unattributed Frame Work.
     *
     * One span, not an accumulated one: every panel drawn in the stretch is a
     * child of it, including the assignment plot's and the console's, whose
     * scan phases have rows of their own where those scans actually run. */
    PROF_UI_2D,
    PROF_REPLAY_HUD,    /* replay_ui_hud_render() (only when replaying) */
    PROF_TOUR_HUD,      /* tour_ui_hud_render() (only during a guided tour) */
    PROF_CODE_PANEL,    /* ui_repl_code_panel_render_with_chrome() */
    PROF_CODE_PANEL_ROWS,     /* adapter row/segment build from REPL state */
    PROF_CODE_PANEL_TEXT,     /* generic ui_text_panel_render() */
    PROF_CODE_PANEL_TEXT_LAYOUT, /* wrap-count cache + viewport metrics */
    PROF_CODE_PANEL_TEXT_CHROME, /* background/border/divider + scrollbar */
    PROF_CODE_PANEL_TEXT_LINES,  /* row draw loop */
    PROF_CODE_PANEL_OVERLAYS, /* tabs/menubar/search/statusbar/picker/swatch */
    PROF_CODE_PANEL_OVERLAY_TABS,
    PROF_CODE_PANEL_OVERLAY_MENU,
    PROF_CODE_PANEL_OVERLAY_MENU_LABELS,
    PROF_CODE_PANEL_OVERLAY_MENU_PINS,
    PROF_CODE_PANEL_OVERLAY_SEARCH,
    PROF_CODE_PANEL_OVERLAY_STATUS,
    PROF_CODE_PANEL_OVERLAY_STATUS_TEXT,
    PROF_CODE_PANEL_OVERLAY_STATUS_ACTIONS,
    PROF_CODE_PANEL_OVERLAY_PICKER,
    PROF_CODE_PANEL_OVERLAY_SWATCH,
    PROF_ASSIGN_PLOT_PANEL,   /* ui_assign_plot_panel_render() */
    PROF_CONSOLE_PANEL,       /* ui_console_panel_render() */
    PROF_UI_PANELS,     /* the floating popups, in paint order below */
    PROF_UI_PANELS_VIZ_LEGEND,   /* ui_buffer_viz_legend_render() */
    PROF_UI_PANELS_AUTOCOMPLETE, /* ui_autocomplete_panel_render() */
    PROF_UI_PANELS_CMD_DESC,     /* ui_command_description_panel_render() */
    PROF_UI_PANELS_GL_STATE,     /* ui_gl_state_panel_render() */
    PROF_UI_PANELS_EXAMPLE_MENU, /* ui_menu_bar_render_example_dropdown() */
    PROF_UI_PANELS_VARIABLES,    /* ui_variable_panel_render() */
    PROF_UI_PANELS_HELP,         /* ui_tabbed_overlay_render() - the help overlay */
    PROF_PROFILE_PANEL,           /* all compute-profile surfaces */
    PROF_PROFILE_PANEL_FPS,       /* ui_fps_panel_render() */
    PROF_PROFILE_PANEL_SECTIONS,  /* ui_profile_panel_render() */
    PROF_PROFILE_PANEL_HISTOGRAM, /* ui_histogram_panel_render() */
    PROF_MEMORY_PANEL,  /* ui_memory_panel_render() (the panel itself) */
    /* ui_panels_render_scene_status(): the status bell's history popup, drawn
     * after every floating telemetry surface. A sibling of the panels above
     * rather than one of PROF_UI_PANELS' children because it is bracketed
     * outside that span - it paints last of all so its rows own their
     * overlapping pixels. */
    PROF_UI_STATUS_HISTORY,
    PROF_COMPOSITOR,    /* glr_compositor_postprocess_frame() - whole-frame
                         * (full-screen) post-process; distinct from the
                         * scene-viewport pass PROF_RENDER3D_POST_PROCESS */
    PROF_FRAME_RESTORE, /* post-render flat-count + predef-value restore */
    /* Host-band stages. The GLUT display callback in gl_repl.c does real
     * per-frame work on both sides of glr_ctrl_display_frame() - scripted
     * input before it, the splash/tour overlays after it - and
     * PROF_FRAME_WORK covers all of it, so those stages need rows of their
     * own or they show up only as unattributed work. A guided tour's cursor +
     * caption overlay is the reason this exists: it can cost more than the
     * entire 3D scene, and used to be invisible here.
     *
     * These two are bracketed from gl_repl.c itself because each spans the
     * boot/controller seam that only the host file bridges (a boot-band call
     * plus a controller-band one), so no single callee can own the bracket. */
    PROF_SCRIPTED_INPUT,   /* capture-env frame hook + pointer-script events */
    /* glr_extedit_poll(): the `--watch` external-editor gate. Bracketed from
     * the host because it is a host-band stage like the two around it, and it
     * has a row of its own for the same reason they do - per-frame work inside
     * PROF_FRAME_WORK with no section shows up only as unattributed remainder.
     * In steady state it is one `stat()` and reads ~0, which is itself the
     * useful signal: it is what says the watcher costs nothing until the file
     * moves, and it is the row stage 2.5's latency gate is measured in. */
    PROF_EXTERNAL_EDIT,
    PROF_HOST_OVERLAYS,    /* post-composite host draws (aggregate) */
    PROF_HOST_SPLASH,      /* splash_render() - startup banner only. Bracketed
                            * from the host too, so splash.c stays free of the
                            * profiler: test_splash links it against nothing but
                            * the theme table and the GL-stub counters. */
    /* glr_pointer_script_render_overlay(), which owns this bracket itself: the
     * guided-tour narration layer - caption, cursor, click ripple, highlight
     * ring. Named for the tour rather than the cursor because the caption's
     * bitmap text is what costs anything here; the cursor and its decorations
     * are a few dozen vertices. (The same overlay renders for env-driven
     * capture runs, which have no HUD and nobody watching a profile panel.) */
    PROF_TOUR_OVERLAY,
    /* glr_ctrl_render_tour_presence(): the ambient "you are in a tour" layer -
     * four gradient edge bands plus, during the first ~1.4 s, a title card.
     * Separate from the overlay above because it has a different lifetime (it
     * outlives the tour by the length of its exit collapse) and a different
     * cost profile (no bitmap text at all once the card is gone). */
    PROF_TOUR_PRESENCE,
    /* The summary rows, in display order. The application owns the display
     * callback boundary (glr_frame_begin / glr_frame_ended in gl_repl.c), and
     * the span between them is the callback body:
     *
     *   Frame Time     = everything the callback does, present included
     *   Frame Work     = up to the present (closed by glr_frame_work_end)
     *   Depth Snapshot = the post-work capture below, when it ran
     *   Present        = Frame Time - Frame Work - Depth Snapshot, derived
     *   Browser/Frame Wait = frame-start interval - previous Frame Time,
     *                        derived at the next glr_frame_begin
     *
     * Present is a *subtraction* rather than a bracket around the swap on
     * purpose: bracketing glFinish + glutSwapBuffers leaves anything else
     * between the two boundaries unattributed, whereas the difference sweeps
     * every last microsecond of the frame into the row built to absorb it.
     * (prof_section_record_us is how a derived row lands its sample.)
     *
     * The capture is the one measured stage that sits in that stretch, so it is
     * subtracted out rather than left inside Present: Present is the row the
     * panel paints as *slack* (is_slack), and 2.5 ms of pixel transfer is not
     * headroom. It belongs to this group rather than up with the host-overlay
     * stages it runs beside because that is what the arithmetic says it is - a
     * third part of Frame Time, not a part of Frame Work. Subtracted only when
     * it sampled this frame (the capture is gated on labels being in scope);
     * otherwise Present absorbs the whole remainder as before.
     *
     * With vsync on the present is mostly the wait for the next scan-out - idle
     * time the frame is *given*, not time it spends - so Frame Work is the row
     * to watch for cost and the panel colors Present inversely (long = green,
     * see the is_slack flag). The caveat that follows: glFinish absorbs GPU work
     * the driver deferred, so a GPU-bound frame shows up as Present's slack
     * draining away rather than as CPU cost in any row above it; the GPU column
     * is the cross-check where timer queries exist. WebGL commonly lacks those
     * queries and may apply its GPU back-pressure between rAF callbacks instead;
     * the final wait row makes that formerly invisible part of the cadence
     * explicit. It is informational rather than a work budget: a healthy host
     * also waits when a callback finishes before its next delivery slot. */
    PROF_FRAME_TOTAL,   /* whole callback, end to end (the "Frame Time" row) */
    PROF_FRAME_WORK,    /* the same minus the present */
    /* glr_ctrl_capture_depth_snapshot(): the vertex-label occlusion cull's
     * scene-depth read, taken from the host after its glFinish and before the
     * swap - so it is outside Frame Work by construction and cannot be moved
     * inside one (Frame Work carries the GPU timer query, and extending it past
     * the glFinish would report the queue drain as GPU time). Its own row rather
     * than folded into the label pass: it no longer happens during that pass,
     * and it is the one piece of per-frame work here whose cost is a transfer
     * rather than a draw. Expect ~2.5 ms at 4x MSAA; if it ever reads near a
     * refresh interval instead, the frame stopped being drained before it runs.
     * See docs/plans. */
    PROF_DEPTH_SNAPSHOT,
    PROF_PRESENT,       /* derived: TOTAL - WORK - DEPTH_SNAPSHOT */
    PROF_FRAME_WAIT,    /* derived: frame-start interval - prior TOTAL */
    PROF_SECTION_COUNT
} ProfSection;

#endif /* PROF_SECTIONS_H */
