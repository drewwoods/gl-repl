#include "app/glr_ctrl.h"
#include "app/glr_ctrl_export.h"
#include "app/glr_ctrl_replay_annotations.h"

#include "subsystems/replay/replay_render.h"
#include "subsystems/edit_overlays/edit_overlays.h"

#include "c_compat.h"  /* STATIC_ASSERT (C99/C11 portable) */
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include "gl_includes.h"
#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "app/glr_audio.h"
#include "subsystems/color_picker/color_picker_state.h"
#include "subsystems/hidden_lines/hidden_lines.h"
#include "editor/clipboard.h"
#include "app/glr_completion.h"
#include "app/glr_compositor.h"      /* whole-frame post-process hook */
#include "app/glr_defaults.h"        /* CFG_DEFAULT_* */
#include "app/glr_log_prefix.h"      /* shared init-trace stamp */
#include "app/glr_state.h"
#include "app/glr_tour_presence.h"   /* ambient tour-presence phase machine */
#include "editor/commit.h"
#include "editor/completion.h"
#include "editor/help_session.h"
#include "editor/inline_file_prompt.h"
#include "editor/inline_rename.h"
#include "editor/input.h"
#include "editor/state.h"
#include "editor/undo.h"
#include "render3d/grid.h"                   /* render3d_grid_reveal (transition curve) */
#include "render3d/axes.h"                   /* render3d_axes_reveal (transition curve) */
#include "render3d/lights.h"                 /* render3d_lights_apply_theme */
#include "subsystems/buffer_viz/buffer_viz.h"      /* buffer-hook subscriber + frame modes */
#include "subsystems/buffer_viz/depth_viz.h"        /* BufferVizDepthMode */
#include "subsystems/buffer_viz/stencil_viz.h"      /* BufferVizStencilMode */
#include "app/glr_actions.h"
#include "app/glr_config.h"
#include "app/glr_camera.h"
#include "app/glr_camera_export.h"
#include "app/glr_workspaces.h"      /* glr_workspaces_active_name (window title) */
#include "repl/workspace_io.h"       /* WORKSPACE_IO_NAME_MAX (snapshot cap assert) */
#include "keys.h"
#include "support/memprof.h"
#include "support/cpuprof.h"
#include "support/gpuprof.h"
#include "app/glr_prof.h"
#include "repl/bootstrap.h"
#include <string.h>
#include "repl/host_effects.h"
#include "repl/program_query.h"
#include "repl/scenes.h"
#include "source_document.h"
#include "repl/examples.h"          /* REPL_EXAMPLE_TAG_* */
#include "repl/eval.h"
#include "repl/parser.h"
#include "repl/executor.h"
#include "repl/export.h"
#include "repl/flatten.h"            /* repl_flat_clears_stencil */
#include "repl/flatten_query.h"
#include "repl/geometry_query.h"
#include "repl/attrib_bits.h"        /* glPushAttrib per-bit highlight collectors */
#include "repl/command_spec.h"       /* repl_attrib_bit_entries (token names) */
#include "repl/command_descriptions.h"
#include "repl/gl_state_inspector.h"
#include "repl/help_text.h"
#include "repl/pipeline.h"
#include "subsystems/replay/replay_annotations.h"
#include "repl/source_scope.h"
#include "repl/state_owners.h"
#include "repl/time.h"
#include "repl/tutorials.h"
#include "repl/visible_vars.h"
#include "subsystems/replay/replay.h"
#include "subsystems/replay/replay_state.h"
#include "subsystems/tutorial/tutorial.h"
#include "subsystems/tutorial/tutorial_state.h"
#include "ui/subsystems/replay_hud.h"
#include "ui/subsystems/tour_hud.h"
#include "ui/subsystems/tour_presence.h"
#include "app/glr_pointer_script.h"   /* glr_pointer_script_tour_view */
#include "app/glr_tours.h"            /* glr_tours_start */
#include "render3d/postprocess_filter.h" /* Render3dPostFilterMode, mode_name */
#include "render3d/render.h"
#include "ui/app/autocomplete_panel.h"
#include "ui/app/command_description_panel.h"
#include "ui/app/gl_state_panel.h"
#include "ui/app/editor.h"
#include "ui/app/layout.h"
#include "ui/app/menu_bar.h"
#include "ui/app/overlay_layout.h"
#include "ui/support/assign_plot.h"
#include "ui/support/memprof.h"
#include "ui/app/numeric_swatch.h"
#include "ui/app/panels.h"
#include "ui/app/repl_code_panel.h"
#include "ui/support/cpuprof.h"
#include "ui/app/snapshot.h"
#include "ui/app/state.h"
#include "ui/app/state_types.h" /* UI-chrome typedefs (CodePanel/Camera/Help/etc.) */
#include "ui/core/tabbed_overlay.h"
#include "ui/subsystems/variable_panel.h"
#include "ui/app/variable_panel_view.h"
#include "app/glr_clipboard.h"        /* glr_clipboard_install (OS clipboard) */
#include "app/glr_assign_plot_bridge.h"
#include "app/glr_color_picker_bridge.h"
#include "app/glr_ctrl_internal.h"
#include "app/glr_modal.h"
#include "subsystems/assign_plot/assign_plot.h"
#include "subsystems/variable_panel/variable_panel_drag.h"
#include "subsystems/variable_panel/variable_panel_state.h"

/* The REPL pipeline tracks light-enable state for REPL_LIGHT_SLOT_COUNT
 * slots (state_views.h, scene-include-free); the scene render contract sizes
 * its light table by MAX_LIGHTS. The controller is the one TU that sees both,
 * so it pins the two counts together - the per-frame merge in
 * glr_ctrl_build_scene_config indexes both by the same i. */
STATIC_ASSERT(REPL_LIGHT_SLOT_COUNT == MAX_LIGHTS,
              "REPL light-slot count must match scene MAX_LIGHTS");

static int glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(
    const UiRenderSnapshot *snap,
    int *out_follow_doc_line,
    int *out_visible_lines);
static UiCommandDescriptionPanelView
glr_ctrl_build_command_description_panel_view(void);

/* Frame-lived storage for the right-click OpenGL-state popup report; the
 * view handed to ui_gl_state_panel_render points here. */
static ReplGlStateReport g_gl_state_report;
/* Gutter label per g_gl_state_report row, parallel by index (-1 when the row
 * has no user source line, or its line has no code-panel row). */
static int g_gl_state_gutter_labels[REPL_GL_STATE_REPORT_MAX_ROWS];
static char g_command_description_title[96];
/* Title text the stencil legend view points at. Built from the config
 * row's own state names so the panel and the menu never disagree about
 * what the active mode is called. */
static char g_buffer_viz_legend_title[48];

/* Gap kept between the replay HUD's top edge and the overlay panel stack.
 * The easing itself lives in the overlay layout engine (overlay_layout.c). */
#define GLR_CTRL_REPLAY_PANEL_CLEARANCE_PX 10

static UiRenderSnapshot g_last_ui_snapshot;
static int g_last_ui_snapshot_valid = 0;
static int g_last_replay_follow_src_line = -1;
/* Offline/capture mode: advance the complete fixed-dt simulation once per
 * rendered frame instead of once per wall-clock-paced GLUT timer callback. */
static int g_tick_per_frame = 0;

/* --- Accumulation motion-blur sub-frame driver ---
 * When accum_effect == BLUR the scene's accum loop calls back per sample to
 * vary the camera (interpolated prev<->cur pose) or the animation time (a
 * sub-step re-bake), accumulating the samples into one blurred frame. The
 * mode is resolved once per frame in glr_ctrl_resolve_blur_subframe(); the
 * baseline snapshot lets each sample start from identical REPL state so
 * accumulating programs don't compound across samples. */
typedef enum { GLR_BLUR_NONE = 0, GLR_BLUR_CAMERA, GLR_BLUR_TIME } GlrBlurMode;
typedef struct {
    GlrBlurMode     mode;
    GlrCameraPose   prev, cur;   /* GLR_BLUR_CAMERA endpoints */
    float           t_end, dt;   /* GLR_BLUR_TIME window [t_end - dt, t_end] */
    int             edit_line;   /* reflatten anchor for time blur */
    float           base_predef[MAX_PREDEF_VARS];
    float           base_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    ReplRenderState base_render;
} GlrSubframeCtx;
static GlrSubframeCtx g_subframe_ctx;
static GlrCameraPose  g_prev_frame_pose;
static int            g_prev_frame_pose_valid = 0;
static GlrCameraPose  g_cur_frame_pose;

/* Elapsed-seconds source for the controller's init-trace lines; installed
 * by main() (init_elapsed_seconds), NULL in tests/demos. */
static GlrCtrlElapsedSecondsFn g_init_log_elapsed_fn = NULL;

void glr_ctrl_set_init_log_elapsed_fn(GlrCtrlElapsedSecondsFn fn) {
    g_init_log_elapsed_fn = fn;
}

/* Emit one init-trace line for the one-shot GL-capability probes in
 * glr_ctrl_init_gl. Carries the shared stamp (matching gl_repl.c's boot
 * trace) when an elapsed source is installed, else the bare tag - both
 * spelled by glr_log_prefix.h, which is also what makes these lines read
 * as trace rather than error in the web build's console. Runs on the main
 * thread, so the fprintf(stderr) reaches the browser console there. `fmt`
 * must not carry a trailing newline. */
static void glr_ctrl_init_log(const char *fmt, ...) {
    va_list ap;
    char prefix[GLR_LOG_PREFIX_MAX];
    if (g_init_log_elapsed_fn) {
        double elapsed = g_init_log_elapsed_fn();
        fputs(glr_log_prefix(prefix, sizeof prefix, &elapsed), stderr);
    } else {
        fputs(glr_log_prefix(prefix, sizeof prefix, NULL), stderr);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

typedef ReplExecutorPointParameterProc
(*GlrCtrlPointParameterProcLoaderFn)(const char *proc_name);

static ReplExecutorPointParameterProc
glr_ctrl_default_point_parameter_loader(const char *proc_name) {
#if (defined(__APPLE__) && defined(USE_GLUT)) || defined(__EMSCRIPTEN__)
    /* Two runtimes advertise glPointParameterfv but can't resolve it by
     * name, so take the statically-linked symbol directly:
     *   - Apple system GLUT has no glutGetProcAddress at all.
     *   - Emscripten: gl4es exports glPointParameterfv and its FPE shader
     *     implements GL_POINT_DISTANCE_ATTENUATION, but glutGetProcAddress
     *     falls through to eglGetProcAddress, whose table only knows
     *     WebGL/GLES2 entry points and returns NULL for this desktop-GL
     *     symbol. Binding it directly lights up the gl4es attenuation path
     *     so point size tracks camera distance (authored points and the
     *     glPolygonMode(GL_POINT) GLUT-solid overlays alike). */
    (void)proc_name;
    return &glPointParameterfv;
#else
    return (ReplExecutorPointParameterProc)glutGetProcAddress(proc_name);
#endif
}

static GlrCtrlPointParameterProcLoaderFn g_glr_ctrl_point_parameter_loader =
    glr_ctrl_default_point_parameter_loader;

static ReplExecutorPointParameterProc
glr_ctrl_load_point_parameter_proc(int has_core, int has_arb, int has_ext) {
    if (!g_glr_ctrl_point_parameter_loader)
        return NULL;
    if (has_core) {
        ReplExecutorPointParameterProc proc = g_glr_ctrl_point_parameter_loader("glPointParameterfv");
        if (proc)
            return proc;
    }
    if (has_arb) {
        ReplExecutorPointParameterProc proc = g_glr_ctrl_point_parameter_loader("glPointParameterfvARB");
        if (proc)
            return proc;
    }
    if (has_ext) {
        ReplExecutorPointParameterProc proc = g_glr_ctrl_point_parameter_loader("glPointParameterfvEXT");
        if (proc)
            return proc;
    }
    /* Some extension-only stacks still expose the unsuffixed symbol
     * through the loader; take it as a final compatibility fallback
     * after the suffixes the extension contract actually names. */
    if (!has_core && (has_arb || has_ext))
        return g_glr_ctrl_point_parameter_loader("glPointParameterfv");
    return NULL;
}

/* Resolve the GL timer-query entry points for gpuprof. Same split as the
 * point-parameter loader above: the Apple system-GLUT fallback build takes
 * the framework symbols directly (Apple GLUT has no glutGetProcAddress);
 * everywhere else (vendored freeglut Cocoa = dlsym on the OpenGL framework,
 * Linux = glXGetProcAddress, OSMesa = OSMesaGetProcAddress) resolves at
 * runtime, trying the core GL 1.5 name then the ARB/EXT suffix. Missing
 * entries come back NULL and gpu_prof_init rejects the table.
 *
 * has_timestamp gates the optional glQueryCounter slot (GL_ARB_timer_query
 * / GL 3.3 only - never advertised by EXT_timer_query, so e.g. Apple's GL
 * 2.1 stays in elapsed mode). It must be capability-gated rather than
 * load-and-see: GLX's GetProcAddress can return a callable stub for any
 * name, supported or not. */
#if defined(__APPLE__) && defined(USE_GLUT)
static GpuProfGlFns glr_ctrl_load_gpu_prof_fns(int has_timestamp) {
    GpuProfGlFns fns;
    (void)has_timestamp;  /* Apple GL 2.1: no glQueryCounter symbol exists */
    fns.gen_queries     = (void (*)(int, unsigned *))&glGenQueries;
    fns.delete_queries  = (void (*)(int, const unsigned *))&glDeleteQueries;
    fns.begin_query     = (void (*)(unsigned, unsigned))&glBeginQuery;
    fns.end_query       = (void (*)(unsigned))&glEndQuery;
    fns.get_query_iv    = (void (*)(unsigned, unsigned, int *))&glGetQueryObjectiv;
    fns.get_query_ui64v =
        (void (*)(unsigned, unsigned, GpuProfU64 *))&glGetQueryObjectui64vEXT;
    fns.query_counter   = NULL;
    return fns;
}
#else
static GLUTproc glr_ctrl_gpu_prof_proc(const char *name, const char *alt_name) {
    GLUTproc proc = glutGetProcAddress(name);
    if (!proc && alt_name)
        proc = glutGetProcAddress(alt_name);
    return proc;
}

static GpuProfGlFns glr_ctrl_load_gpu_prof_fns(int has_timestamp) {
    GpuProfGlFns fns;
    fns.gen_queries     = (void (*)(int, unsigned *))
        glr_ctrl_gpu_prof_proc("glGenQueries", "glGenQueriesARB");
    fns.delete_queries  = (void (*)(int, const unsigned *))
        glr_ctrl_gpu_prof_proc("glDeleteQueries", "glDeleteQueriesARB");
    fns.begin_query     = (void (*)(unsigned, unsigned))
        glr_ctrl_gpu_prof_proc("glBeginQuery", "glBeginQueryARB");
    fns.end_query       = (void (*)(unsigned))
        glr_ctrl_gpu_prof_proc("glEndQuery", "glEndQueryARB");
    fns.get_query_iv    = (void (*)(unsigned, unsigned, int *))
        glr_ctrl_gpu_prof_proc("glGetQueryObjectiv", "glGetQueryObjectivARB");
    fns.get_query_ui64v = (void (*)(unsigned, unsigned, GpuProfU64 *))
        glr_ctrl_gpu_prof_proc("glGetQueryObjectui64vEXT", "glGetQueryObjectui64v");
    fns.query_counter   = has_timestamp
        ? (void (*)(unsigned, unsigned))
              glr_ctrl_gpu_prof_proc("glQueryCounter", NULL)
        : NULL;
    return fns;
}
#endif

/* Non-static: the input router (src/app/glr_ctrl_router.c) reads this cached
 * snapshot for drag hit-testing. Declared in glr_ctrl_internal.h. */
const UiRenderSnapshot *glr_ctrl_drag_hit_test_snapshot(void) {
    if (!g_last_ui_snapshot_valid) {
        glr_ctrl_build_ui_snapshot(&g_last_ui_snapshot);
        g_last_ui_snapshot_valid = 1;
    }
    return &g_last_ui_snapshot;
}

static int glr_ctrl_cmd_is_focus_vertex(const GLCmd *cmd) {
    /* glVertex2f counts too: args[2] is zero on a well-formed 2D vertex
     * (parser leaves the slot zero-initialised), so building focus.pos
     * from args[0..2] gives (x, y, 0) - the right point to focus on. */
    return cmd->valid && repl_cmd_emits_vertex(cmd->type);
}

/* Last unrolled draw emitted by `line_idx` - the copy the last-instance
 * scopes highlight, and the one whose loop-body variable values the
 * variable panel still reports after the frame. */
static int glr_ctrl_last_flat_color_consumer_for_source_line(int line_idx) {
    FlatProgramView flat = repl_state_flat_program_view();
    int found = -1;

    for (int i = 0; i < flat.cmd_count; i++) {
        const GLCmd *cmd = &flat.cmds[i];
        if (!cmd->valid)
            continue;
        if (cmd->src_cmd_idx == line_idx &&
            repl_cmd_consumes_current_color(cmd->type))
            found = i;
    }
    return found;
}

static int glr_ctrl_find_affecting_transform_highlights(int line_idx,
                                                        int *out,
                                                        int out_cap) {
    GlrPresentationState presentation = glr_state_presentation();

    if (presentation.overlay_scope == OVERLAY_SCOPE_LAST_INSTANCE ||
        presentation.overlay_scope == OVERLAY_SCOPE_SINGLE_POLYGON) {
        int flat_idx = glr_ctrl_last_flat_color_consumer_for_source_line(line_idx);
        if (flat_idx >= 0)
            return repl_find_affecting_transforms_for_flat_vertex(
                flat_idx, out, out_cap);
        return 0;
    }

    return repl_find_affecting_transforms_flat(line_idx, out, out_cap);
}

static Render3dFocusVertex glr_ctrl_build_focus_vertex(void) {
    Render3dFocusVertex focus = { .valid = 0 };
    int edit_line = editor_state_edit_line();
    const GLCmd *cmds = repl_state_document_cmds();

    if (edit_line >= 0 && edit_line < repl_state_document_count() &&
        glr_ctrl_cmd_is_focus_vertex(&cmds[edit_line])) {
        focus.pos[0] = cmds[edit_line].args[0];
        focus.pos[1] = cmds[edit_line].args[1];
        focus.pos[2] = cmds[edit_line].args[2];
        focus.valid = 1;
    } else {
        for (int i = edit_line - 1; i >= 0; i--) {
            if (glr_ctrl_cmd_is_focus_vertex(&cmds[i])) {
                focus.pos[0] = cmds[i].args[0];
                focus.pos[1] = cmds[i].args[1];
                focus.pos[2] = cmds[i].args[2];
                focus.valid = 1;
                break;
            }
        }
    }

    return focus;
}

/* Parse up to `max_slots` comma-separated arg slots out of `src`, recording
 * which positions are actually present. repl_eval_parse_exprs() skips leading
 * commas so ",1," and "1," look identical to it; this version tracks
 * slot indices. Empty slots leave filled[i]=0, out[i] unchanged. Callers
 * pass 3 (glVertex / glTranslatef / glScalef) or 4 (glRotatef angle+axis). */
static int parse_arg_slots(const char *src,
                           const ExprVar *predef_vars, int predef_var_count,
                           float *out, int *filled, int max_slots) {
    const char *s = src;
    int n_filled = 0;
    for (int i = 0; i < max_slots; i++) filled[i] = 0;

    for (int slot = 0; slot < max_slots; slot++) {
        while (*s == ' ' || *s == '\t') s++;
        const char *start = s;
        s = repl_scan_next_arg_delim(s);
        const char *end = s;
        const char *c = start;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        if (c < end) {
            ExprCtx ctx = { .p = start, .vars = predef_vars,
                            .num_vars = predef_var_count,
                            .err = NULL, .err_sz = 0 };
            float val = repl_eval_expr(&ctx);
            if (ctx.p > start) {
                out[slot] = val;
                filled[slot] = 1;
                n_filled++;
            }
        }
        if (*s == ',') s++;
        else break;
    }

    return n_filled;
}

/* Light slot 0..MAX_LIGHTS-1 for a "glEnable(GL_LIGHTn" / "glDisable(GL_LIGHTn"
 * text (committed line or live, still-being-typed input), else -1. The digit
 * guard after "GL_LIGHT" rejects GL_LIGHTING / GL_LIGHT_MODEL*, and the missing
 * close paren of a partial mid-typed line is irrelevant, so live feedback lands
 * as soon as the digit is typed. */
static int light_slot_from_text(const char *s) {
    if (!s) return -1;
    if (strncmp(s, "glEnable(", 9) != 0 && strncmp(s, "glDisable(", 10) != 0)
        return -1;
    const char *g = strstr(s, "GL_LIGHT");
    if (!g) return -1;
    char d = g[8]; /* char after "GL_LIGHT" */
    return (d >= '0' && d < '0' + MAX_LIGHTS) ? d - '0' : -1;
}

/* Resolve the cursor-line geometry args into floats so the scene
 * module can render its guides without depending on repl_eval. Sets the
 * pre-parsed fields on `snapshot` based on `input`. */
static void fill_guide_arg_slots(Render3dGuideSnapshot *snapshot,
                                 const char *input, int input_len) {
    snapshot->vertex_n_filled = 0;
    snapshot->raster_pos_n_filled = 0;
    snapshot->normal_n_filled = 0;
    snapshot->vertex_filled[0] = snapshot->vertex_filled[1] =
        snapshot->vertex_filled[2] = 0;
    snapshot->clip_plane_idx = -1;
    snapshot->clip_plane_n_filled = 0;
    snapshot->clip_plane_cap_enabled = 0;

    if (!input) return;

    ReplPredefView predef = repl_eval_predef_view();

    const char *vertex_args = NULL;
    if (strncmp(input, "glVertex3f(", 11) == 0 && input_len > 11) {
        vertex_args = input + 11;
    } else if (strncmp(input, "glVertex4f(", 11) == 0 && input_len > 11) {
        vertex_args = input + 11;
    } else if (strncmp(input, "glVertex2f(", 11) == 0 && input_len > 11) {
        vertex_args = input + 11;
    } else if (strncmp(input, "gluVertex(", 10) == 0 && input_len > 10) {
        vertex_args = input + 10;
    }
    if (vertex_args) {
        snapshot->vertex_n_filled = parse_arg_slots(
            vertex_args, predef.vars, predef.count,
            snapshot->vertex_args, snapshot->vertex_filled, 3);
    }

    if (strncmp(input, "glRasterPos3f(", 14) == 0 && input_len > 14) {
        int raster_filled[3];
        snapshot->raster_pos_n_filled = parse_arg_slots(
            input + 14, predef.vars, predef.count,
            snapshot->raster_pos_args, raster_filled, 3);
    }

    /* Live transform args (glTranslatef / glScalef / glRotatef), parsed the
     * same way so transform guides can track the cursor line before commit.
     * Rotate carries a 4th slot (angle + axis); translate/scale use 3. */
    snapshot->xform_n_filled = 0;
    snapshot->xform_filled[0] = snapshot->xform_filled[1] =
        snapshot->xform_filled[2] = snapshot->xform_filled[3] = 0;
    const char *xform_args = NULL;
    int xform_max = 0;
    if (strncmp(input, "glTranslatef(", 13) == 0) {
        xform_args = input + 13; xform_max = 3;
    } else if (strncmp(input, "glScalef(", 9) == 0) {
        xform_args = input + 9; xform_max = 3;
    } else if (strncmp(input, "glRotatef(", 10) == 0) {
        xform_args = input + 10; xform_max = 4;
    }
    if (xform_args) {
        snapshot->xform_n_filled = parse_arg_slots(
            xform_args, predef.vars, predef.count,
            snapshot->xform_args, snapshot->xform_filled, xform_max);
    }

    const char *normal_args = NULL;
    if (strncmp(input, "glNormal3f(", 11) == 0 && input_len > 11) {
        normal_args = input + 11;
    } else if (strncmp(input, "gluNormal(", 10) == 0 && input_len > 10) {
        normal_args = input + 10;
    }
    if (normal_args) {
        snapshot->normal_n_filled = repl_eval_parse_exprs(
            normal_args, snapshot->normal_args, 3, (ExprVar *)predef.vars, predef.count);
    }

    /* Live clip-plane args. The plane digit lands the guide as soon as
     * it's typed; coefficients follow after the first comma, with an
     * optional (GLdouble[]){ prefix (the canonical committed form). */
    if (strncmp(input, "glClipPlane(GL_CLIP_PLANE", 25) == 0 &&
        input_len > 25 && input[25] >= '0' && input[25] <= '5') {
        snapshot->clip_plane_idx = input[25] - '0';
        const char *cp_args = strchr(input + 25, ',');
        if (cp_args) {
            int cp_filled[4];
            cp_args++;
            while (*cp_args == ' ' || *cp_args == '\t') cp_args++;
            if (strncmp(cp_args, "(GLdouble[]){", 13) == 0)
                cp_args += 13;
            snapshot->clip_plane_n_filled = parse_arg_slots(
                cp_args, predef.vars, predef.count,
                snapshot->clip_plane_args, cp_filled, 4);
        }
        /* Net enable state for this plane's cap: last glEnable/glDisable
         * in document order wins. Loop/if nesting is ignored - this is a
         * hint for the guide's dimmed "cap off" styling, not execution. */
        {
            GLenum cap = (GLenum)(GL_CLIP_PLANE0 + snapshot->clip_plane_idx);
            for (int i = 0; i < snapshot->source_cmd_count; i++) {
                const GLCmd *cmd = &snapshot->source_cmds[i];
                if (!cmd->valid) continue;
                if (cmd->type == CMD_ENABLE && (GLenum)cmd->args[0] == cap)
                    snapshot->clip_plane_cap_enabled = 1;
                else if (cmd->type == CMD_DISABLE && (GLenum)cmd->args[0] == cap)
                    snapshot->clip_plane_cap_enabled = 0;
            }
        }
    }
}

/* Build a guide-render snapshot from live REPL + editor state. The
 * Render3dRenderConfig argument is consulted only for fields the
 * controller already supplies (alpha_scale);
 * everything else comes from REPL/editor accessors so this can run
 * without a per-frame config pointer if needed. */
static Render3dGuideSnapshot glr_ctrl_build_guide_snapshot(const Render3dRenderConfig *config) {
    GlrPresentationState presentation = glr_state_presentation();
    ReplVariableView vars = repl_state_variables();
    EditorInputView input = editor_state_input();
    int edit_line = editor_state_edit_line();

    Render3dGuideSnapshot snapshot = {
        .show_guides = (presentation.xform_guide_mode != RENDER3D_XFORM_GUIDE_OFF),
        .replaying = replay_active(),
        .replay_focus_anchor_flat_idx = replay_focus_anchor_flat_idx(),
        .xform_guide_mode = presentation.xform_guide_mode,
        .anim_time = vars.anim_time,
        .input = input.input,
        .input_len = input.input_len,
        .cursor_pos = input.cursor_pos,
        .edit_line_idx = edit_line,
        .prefer_last_instance =
            (presentation.overlay_scope == OVERLAY_SCOPE_LAST_INSTANCE ||
             presentation.overlay_scope == OVERLAY_SCOPE_SINGLE_POLYGON),
        .inserting = editor_insert_mode(),
        .edit_line_committed_text = editor_buffer_line(edit_line),
        .source_cmds = repl_state_document_cmds(),
        .source_cmd_count = repl_state_document_count(),
        .flat_program = repl_state_flat_program_view(),
        .alpha_scale = config ? config->alpha_scale : 1.0f,
    };
    fill_guide_arg_slots(&snapshot, input.input, input.input_len);
    return snapshot;
}

/* Per-frame replay-fade state, owned by the controller. Used by the main
 * fill (to clamp the flat-cmd count to the pre-fade base limit) and by
 * the post_fill_fn hook (to render the fading-batch overlay). */
static ReplayFadePlan g_replay_fade_plan;
/* Frame-local replay execution clamp. This must not be written into
 * ReplFlatProgramState.cmd_count; UI/snapshot code should still see the
 * full flattened program. */
static int g_frame_replay_exec_limit = -1;
/* Frame-local assignment-plot replay markers: where the clamp above falls
 * within each plotted series' executions, and the value each series produced
 * there. Computed once the clamp is resolved and read by the panel's view
 * builder later in the same frame. */
static float g_assign_plot_replay_frac[MAX_ASSIGN_PLOT_SERIES];
static float g_assign_plot_replay_value[MAX_ASSIGN_PLOT_SERIES];
static int   g_assign_plot_replay_marker = 0;

static void glr_ctrl_build_replay_fade_plan(FlatProgramView flat_program, int replaying) {
    ReplayFadeBatchView fade_batches;
    int batch_count;

    memset(&g_replay_fade_plan, 0, sizeof(g_replay_fade_plan));
    g_replay_fade_plan.active = 0;
    g_replay_fade_plan.base_limit = 0;

    if (!replaying)
        return;

    replay_copy_baseline_predef_snapshot(&g_replay_fade_plan.baseline_predef);
    replay_copy_baseline_scratch_arrays(
        g_replay_fade_plan.baseline_scratch_arrays);

    if (!replay_has_active_fades())
        return;

    g_replay_fade_plan.base_limit = replay_fill_base_limit(flat_program);
    fade_batches = replay_fade_batches_view();
    batch_count = replay_compute_fade_skip_limits(flat_program, g_replay_fade_plan.skip_limits,
                                                       REPLAY_FADE_BATCH_MAX);
    if (batch_count > REPLAY_FADE_BATCH_MAX)
        batch_count = REPLAY_FADE_BATCH_MAX;

    g_replay_fade_plan.batch_count = batch_count;
    for (int batch_idx = 0; batch_idx < batch_count; batch_idx++) {
        const ReplayFadeBatch *batch = &fade_batches.batches[batch_idx];
        g_replay_fade_plan.batches[batch_idx] = *batch;
        g_replay_fade_plan.batch_alpha[batch_idx] = replay_batch_alpha(batch);
    }
    g_replay_fade_plan.active = 1;
}

static int glr_ctrl_current_begin_block_source_extent(int edit_line,
                                                      int *out_start,
                                                      int *out_end);

static OverlaySnapshotPack g_overlay_pack;

static void glr_ctrl_build_overlay_pack(OverlaySnapshotPack *pack, const Render3dRenderConfig *cfg) {
    GlrPresentationState presentation = glr_state_presentation();
    int replaying = replay_active();
    int replay_mode_vertex = replaying && (replay_mode() == REPLAY_MODE_VERTEX);

    pack->walk.program = repl_state_flat_program_view();
    pack->walk.cursor.edit_line_idx = editor_state_edit_line();
    pack->walk.cursor.cursor_block_begin = repl_state_flat_program_current_block_begin();
    pack->walk.cursor.cursor_block_end = repl_state_flat_program_current_block_end();
    pack->walk.cursor.cursor_source_block_valid =
        glr_ctrl_current_begin_block_source_extent(
            pack->walk.cursor.edit_line_idx,
            &pack->walk.cursor.cursor_source_block_begin,
            &pack->walk.cursor.cursor_source_block_end);
    pack->walk.cursor.cursor_func_scope_mask = 0;
    pack->walk.cursor_overlay_scope = presentation.overlay_scope;

    pack->walk.cursor_poly_valid = 0;
    pack->walk.cursor_poly_first = 0;
    pack->walk.cursor_poly_last = 0;
    pack->walk.cursor_poly_fan_anchor = 0;
    if (presentation.overlay_scope == OVERLAY_SCOPE_SINGLE_POLYGON) {
        ReplCursorPolygon poly =
            repl_flatten_cursor_polygon(pack->walk.cursor.edit_line_idx);
        pack->walk.cursor_poly_valid = poly.valid;
        pack->walk.cursor_poly_first = poly.first;
        pack->walk.cursor_poly_last = poly.last;
        pack->walk.cursor_poly_fan_anchor = poly.fan_anchor;
    }

    /* Silhouette outlines only make sense over filled geometry; in any
     * wireframe mode the fill is replaced by edges, so suppress them. */
    pack->walk.show_vertex_outlines = presentation.show_vertex_outlines &&
                                      presentation.wireframe == WIREFRAME_OFF;
    pack->walk.vertex_outline_style = presentation.vertex_outline_style;
    pack->walk.highlight_current_poly =
        presentation.highlight_current_poly != POLY_HIGHLIGHT_OFF && !replaying;
    pack->walk.highlight_clipped_culled =
        presentation.highlight_current_poly == POLY_HIGHLIGHT_CLIPPED_CULLED &&
        !replaying;
    pack->walk.replay_tess_preview = replay_mode_vertex;
    pack->walk.show_vertex_points = presentation.show_vertex_points;
    pack->walk.replay_vertex_points = replay_mode_vertex;
    pack->walk.replay_vertex_label = replay_mode_vertex && replay_vertex_label();
    pack->walk.show_normal_vectors = presentation.show_normal_vectors;
    pack->walk.replay_normal_display = replay_mode_vertex
                                       ? replay_normal_display()
                                       : REPLAY_NORMAL_DISPLAY_OFF;
    pack->walk.replay_anchor_flat_idx = replay_mode_vertex
                                        ? replay_focus_anchor_flat_idx() : -1;
    pack->walk.xform_guide_mode = presentation.xform_guide_mode;

    pack->snapshot = glr_ctrl_build_guide_snapshot(cfg);
    pack->vertex_label_mode = (OverlayVertexLabelMode)presentation.show_vertex_labels;
    pack->overlay_scope = presentation.overlay_scope;
    pack->vertex_label_placement = presentation.vertex_label_placement;
    pack->depth_snapshot = glr_ctrl_depth_snapshot_view();
    pack->ortho_mode = presentation.ortho_mode;
    pack->show_normal_vectors = presentation.show_normal_vectors;
    pack->multisample_enabled = cfg ? cfg->multisample_enabled : 0;
    pack->line_smooth_enabled = cfg ? cfg->line_smooth_enabled : 0;
}

/* Scan a glPushAttrib line's canonical text for each GL_*_BIT token in its
 * mask and push one HIGHLIGHT_ATTRIB_BIT_TOKEN char-range highlight per token
 * (aux = canonical bit index). The scan is confined to the (...) argument
 * range and each token is gated on the parsed mask, so a token mentioned in a
 * trailing comment is never coloured. Within the argument range canonical
 * bitfield text guarantees exact spelling and a single appearance per token
 * (dupe-drop), so a substring scan suffices. */
static void glr_ctrl_push_attrib_bit_tokens(int push_line) {
    const GLCmd *cmd = repl_state_document_cmd_at(push_line);
    const char *text = editor_buffer_view_line(editor_buffer_view(), push_line);
    ReplAttribTokenSpan spans[REPL_ATTRIB_BIT_COUNT];
    unsigned mask;
    int n;
    if (!cmd || cmd->type != CMD_PUSH_ATTRIB || !text)
        return;
    mask = (unsigned)cmd->args[0];
    n = repl_attrib_bit_token_spans(text, spans, REPL_ATTRIB_BIT_COUNT);
    for (int i = 0; i < n; i++) {
        if (!(mask & (unsigned)repl_attrib_bit_entries()[spans[i].bit_idx].value))
            continue;
        editor_state_highlights_append_aux(push_line, spans[i].char_start,
                                           spans[i].char_end,
                                           HIGHLIGHT_ATTRIB_BIT_TOKEN,
                                           spans[i].bit_idx);
    }
}

static void glr_ctrl_push_highlights(void) {
    editor_state_highlights_clear();

    int doc_count = repl_state_document_count();
    int edit_line = editor_state_edit_line();
    int insert_mode = editor_insert_mode();

    if (!insert_mode && edit_line >= 0 && edit_line < doc_count) {
        const GLCmd *cmd = repl_state_document_cmd_at(edit_line);
        if (cmd && cmd->valid) {
            int norm_idx = repl_find_feeding_normal_cmd(edit_line);
            int color_idx = repl_find_feeding_color_cmd(edit_line);
            if (norm_idx >= 0)
                editor_state_highlights_append(norm_idx, -1, -1,
                                                    HIGHLIGHT_FEEDING_NORMAL);
            if (color_idx >= 0)
                editor_state_highlights_append(color_idx, -1, -1,
                                                    HIGHLIGHT_FEEDING_COLOR);

            if (cmd->type == CMD_POP_MATRIX) {
                int push_idx = repl_find_matching_push_matrix(edit_line);
                if (push_idx >= 0)
                    editor_state_highlights_append(push_idx, -1, -1,
                                                        HIGHLIGHT_MATCHING_PUSH_MATRIX);
            } else if (cmd->type == CMD_PUSH_MATRIX) {
                int pop_idx = repl_find_matching_pop_matrix(edit_line);
                if (pop_idx >= 0)
                    editor_state_highlights_append(pop_idx, -1, -1,
                                                        HIGHLIGHT_MATCHING_PUSH_MATRIX);
            } else if (cmd->type == CMD_END) {
                int begin_idx = repl_find_matching_begin(edit_line);
                if (begin_idx >= 0)
                    editor_state_highlights_append(begin_idx, -1, -1,
                                                        HIGHLIGHT_MATCHING_PUSH_MATRIX);
            } else if (cmd->type == CMD_BEGIN) {
                int end_idx = repl_find_matching_end(edit_line);
                if (end_idx >= 0)
                    editor_state_highlights_append(end_idx, -1, -1,
                                                        HIGHLIGHT_MATCHING_PUSH_MATRIX);
            } else if (cmd->type == CMD_PUSH_ATTRIB) {
                /* Cursor on the push: bracket the matching pop, mark the prior
                 * setter lines this push saves, and colour each mask token on
                 * the push line itself. */
                ReplAttribHighlightLine saved[REPL_ATTRIB_HL_MAX];
                int pop_idx = repl_find_matching_pop_attrib(edit_line);
                int ns;
                if (pop_idx >= 0)
                    editor_state_highlights_append(pop_idx, -1, -1,
                                                        HIGHLIGHT_MATCHING_PUSH_MATRIX);
                ns = repl_attrib_collect_push_saved(edit_line, saved,
                                                    REPL_ATTRIB_HL_MAX);
                for (int i = 0; i < ns; i++)
                    editor_state_highlights_append_aux(saved[i].line_idx, -1, -1,
                                                        HIGHLIGHT_ATTRIB_STATE,
                                                        (int)saved[i].bit_idx_mask);
                glr_ctrl_push_attrib_bit_tokens(edit_line);
            } else if (cmd->type == CMD_POP_ATTRIB) {
                /* Cursor on the pop: bracket the matching push (colouring its
                 * mask tokens) and mark the scoped setter lines this pop
                 * reverts. */
                ReplAttribHighlightLine rev[REPL_ATTRIB_HL_MAX];
                int push_idx = repl_find_matching_push_attrib(edit_line);
                int nr;
                if (push_idx >= 0) {
                    editor_state_highlights_append(push_idx, -1, -1,
                                                        HIGHLIGHT_MATCHING_PUSH_MATRIX);
                    glr_ctrl_push_attrib_bit_tokens(push_idx);
                }
                nr = repl_attrib_collect_pop_reverted(edit_line, rev,
                                                      REPL_ATTRIB_HL_MAX);
                for (int i = 0; i < nr; i++)
                    editor_state_highlights_append_aux(rev[i].line_idx, -1, -1,
                                                        HIGHLIGHT_ATTRIB_STATE,
                                                        (int)rev[i].bit_idx_mask);
            }

            /* Affecting-transform highlight for the edit cursor. During
             * replay the replay-focus vertex owns this marker instead (the
             * cursor may be parked elsewhere), so skip the cursor set and let
             * the replay block below push it. Prefer the flat-accurate
             * resolver so a vertex inside a funcN body picks up the
             * calling-scope transforms too; fall back to the source walk only
             * when the flat program is empty/unbuilt (e.g. a flatten error) -
             * by this point in the frame flatten has already run and cleared
             * its dirty flag. */
            if (!replay_active()) {
                int xform_lines[MAX_AFFECTING_TRANSFORMS];
                int xform_count = glr_ctrl_find_affecting_transform_highlights(
                    edit_line, xform_lines, MAX_AFFECTING_TRANSFORMS);
                if (xform_count == 0 && repl_state_flat_program_view().cmd_count == 0)
                    xform_count = repl_find_affecting_transforms(
                        edit_line, xform_lines, MAX_AFFECTING_TRANSFORMS);
                for (int i = 0; i < xform_count; i++)
                    editor_state_highlights_append(xform_lines[i], -1, -1,
                                                        HIGHLIGHT_AFFECTING_TRANSFORM);
            }
        }
    }

    /* Always-on: flag structurally unbalanced bracket commands
     * (unmatched glPushMatrix/glBegin, orphan glPopMatrix/glEnd). Not
     * cursor-gated - the relaxed REPL tolerates these but export
     * auto-balances them, so make them visible in the gutter. */
    {
        int unbalanced[MAX_HIGHLIGHTS];
        int nb = repl_source_scope_collect_unbalanced(unbalanced, MAX_HIGHLIGHTS);
        for (int i = 0; i < nb; i++)
            editor_state_highlights_append(unbalanced[i], -1, -1,
                                                HIGHLIGHT_UNBALANCED);
    }

    int src_line = replay_src_line();
    if (replay_active() && src_line >= 0)
        editor_state_highlights_append(src_line, -1, -1,
                                            HIGHLIGHT_REPLAY_PC);

    /* When the focused replay command was expanded from a funcN(...) call,
     * also light up the call site(s) so a reused or recursive function shows
     * which invocation is live (the PC line above is the body line inside the
     * function). call_src_cmd_idx is the immediate caller; root_call_src_cmd_idx
     * the outermost caller of a nested chain - push it only when distinct. */
    if (replay_active()) {
        int focus = replay_focus_flat_idx();
        FlatProgramView flat = repl_state_flat_program_view();
        if (focus >= 0 && focus < flat.cmd_count) {
            int call_site = flat.cmds[focus].call_src_cmd_idx;
            int root_site = flat.cmds[focus].root_call_src_cmd_idx;
            if (call_site >= 0)
                editor_state_highlights_append(call_site, -1, -1,
                                                    HIGHLIGHT_REPLAY_CALL_SITE);
            if (root_site >= 0 && root_site != call_site)
                editor_state_highlights_append(root_site, -1, -1,
                                                    HIGHLIGHT_REPLAY_ROOT_CALL_SITE);
        }
    }

    /* Affecting-transform highlight anchored on the replay-focused draw
     * (req 5). replay_focus_anchor_flat_idx() is a flat index (vertex mode
     * only; -1 otherwise) of the draw the step emitted - a glVertex/gluVertex
     * or a glutSolid* - so feed it straight to the req-4 exact-flat resolver,
     * which already accepts any repl_cmd_consumes_current_color target. This
     * shows the transforms shaping the draw currently being replayed,
     * independent of where the edit cursor sits, and composes with the replay
     * PC / call-site markers. */
    if (replay_active()) {
        int focus_anchor = replay_focus_anchor_flat_idx();
        if (focus_anchor >= 0) {
            int xform_lines[MAX_AFFECTING_TRANSFORMS];
            int xform_count = repl_find_affecting_transforms_for_flat_vertex(
                focus_anchor, xform_lines, MAX_AFFECTING_TRANSFORMS);
            for (int i = 0; i < xform_count; i++)
                editor_state_highlights_append(xform_lines[i], -1, -1,
                                                    HIGHLIGHT_AFFECTING_TRANSFORM);
        }
    }

    if (tutorial_active()) {
        int insertion_line = tutorial_expected_commit_line();
        if (insertion_line >= 0)
            editor_state_highlights_append(insertion_line, -1, -1,
                                                HIGHLIGHT_TUTORIAL_INSERTION);
    }
}

/* Push per-line text overrides for source lines whose displayed text
 * should differ from the buffer text. Today only replay's expansion-mode
 * annotations produce overrides (e.g. `x = 0.4` -> `x = 0.4 // = 0.4`
 * with the evaluated form appended). The list stays empty outside
 * active replay, and is sparse during it (only has_vars cmds of the
 * applicable kinds get entries). */
static void glr_ctrl_push_line_overrides(void) {
    editor_state_line_overrides_clear();
    ReplayRuntimeState replay = replay_state_view();
    if (!replay.active || !replay.expand_args)
        return;

    SourceTextView view = source_document_view();
    int n = repl_state_document_count();
    for (int i = 0; i < n; i++) {
        char display[MAX_LINE_OVERRIDE_TEXT];
        if (!replay_code_panel_get_command_display_text(view, i, display,
                                                             sizeof(display)))
            continue;
        const char *base = source_text_line(view, i);
        if (base && strcmp(display, base) == 0)
            continue;
        editor_state_line_overrides_append(i, display);
    }
}

static void glr_ctrl_push_color_transformers(void) {
    editor_state_transformers_clear();
    int doc_count = repl_state_document_count();
    for (int i = 0; i < doc_count; i++) {
        float r, g, b, a;
        int has_alpha;
        if (!color_picker_read_cmd_color(i, &r, &g, &b, &a, &has_alpha))
            continue;
        UiTransformer t = {
            .line_idx = i,
            .char_start = -1,
            .char_end = -1,
            .kind = TRANSFORMER_COLOR_PICKER,
            .state.color = {
                .r = r,
                .g = g,
                .b = b,
                .a = a,
                .has_alpha = has_alpha,
            },
        };
        if (!editor_state_transformers_append(&t))
            break;
    }
}

/* Browser autoplay policy: the Web Audio context stays suspended until
 * a user gesture. The very first key / mouse / special event after
 * startup fires glr_audio_on_user_gesture; native builds make this a
 * no-op. */

/* Layout provider installed on the editor at glr_ctrl_init_gl. The
 * editor reads through this so src/editor/ does not need to include
 * src/app/glr_state.h. */
int glr_ctrl_code_panel_layout_provider(void) {
    return glr_state_presentation().code_panel_layout;
}

/* The action
 * writes glr_state_presentation_mut(), runs glr_ctrl_sync_ui_chrome,
 * and closes the menu / picker - all controller / UI concerns the
 * editor module should not be reaching into. The editor signals
 * the intent via the restore_hidden_code_panel effect flag, which
 * apply_input_effects below actualizes by calling this. */
int glr_ctrl_restore_hidden_code_panel(void) {
    if (glr_state_presentation().code_panel_layout != CODE_PANEL_LAYOUT_HIDDEN)
        return 0;
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_menu_bar_close();
    color_picker_stop();
    return 1;
}

/* The body reaches
 * the camera, menu bar, color picker, and the controller's own
 * code-panel-drag reset - none of which are editor-text concerns.
 * The editor still owns the commit-side state reset via
 * editor_commit_reset_transients(); this wraps that with the
 * cross-subsystem cleanup the app-frame paths actually want. */
void glr_ctrl_reset_transients(void) {
    editor_commit_reset_transients();
    /* Settle before the reset, not after: an outgoing scene's camera ease is
     * still in flight when scenes are switched in quick succession, and
     * glr_camera_controls_reset() would abandon it at whatever interpolation
     * frame the last tick produced. That partway pose is what the incoming
     * scene inherits (a scene with no `@camera` rows keeps it verbatim, a
     * partial header merges against it) and what repl_load_* writes back into
     * the outgoing user-scene slot, since a scene save reads
     * glr_camera_destination(). Settling makes the handoff the outgoing
     * scene's intended pose regardless of switch timing. */
    glr_camera_settle_target();
    glr_camera_controls_reset();
    glr_camera_clear_scene_default();
    ui_menu_bar_close();
    color_picker_stop();
    ui_state_command_description_close();
    glr_ctrl_router_reset_code_panel_drag();
}

/* Non-static: the input router (src/app/glr_ctrl_router.c) applies dispatch
 * effects after each routing helper. Declared in glr_ctrl_internal.h. */
void glr_ctrl_apply_input_effects(EditorInputDispatchEffects effects) {
    if (effects.set_cursor)
        glutSetCursor(effects.cursor);
    /* The GLUT host continuously requests frames at 60 Hz, so the semantic
     * redraw request is already satisfied. Issuing an extra host redraw here
     * would make input events perturb that cadence on high-refresh displays. */
    (void)effects.request_redraw;
    if (effects.restore_hidden_code_panel)
        glr_ctrl_restore_hidden_code_panel();
    if (effects.close_help_overlay)
        glr_ctrl_close_help();
}

/* ========================================================================= */
/* Scene config builder (push model)                                          */
/* ========================================================================= */

/* The GL clear-color state every program walk starts in: what a glClear with
 * no preceding glClearColor uses. Fixed configuration, never observation -
 * render3d establishes it from Render3dRenderConfig.baseline_clear_color and
 * the executor starts its observation from the same array, so the walk's
 * starting point and GL's cannot disagree. */
static const float k_baseline_clear_rgba[4] = {
    CFG_DEFAULT_CLEAR_R, CFG_DEFAULT_CLEAR_G,
    CFG_DEFAULT_CLEAR_B, CFG_DEFAULT_CLEAR_A
};

/* The last background a program actually established, as observed by the walk
 * that cleared it. Seeded to the configured default and written only by a
 * fully-known observation (glr_ctrl_retain_background), so a walk that
 * established none - no color clear, a clear under a glColorMask that left a
 * channel disabled, a replay prefix whose PC has not reached the clear, a
 * rejected config that ran no callback at all - keeps showing the last honest
 * answer instead of snapping to the default.
 *
 * Deliberately never reset or invalidated (not on scene load, not on
 * reset_all): invalidating would snap to the default for exactly the frame the
 * retained value is least trustworthy.
 *
 * **This one variable serves both presentation vintages, and which one a
 * consumer gets is decided by WHEN it reads - not by a second copy.**
 * glr_ctrl_build_scene_config() runs before the geometry walk, so
 * presentation_rgba and the alpha_scale derived from it carry the previous
 * frame's background; the chrome strips clear after the walk, so they read
 * what this frame just established. That read ordering is load-bearing (see
 * glr_ctrl_display_frame) and is pinned by test_glr_ctrl. */
static float g_presentation_rgba[4] = {
    CFG_DEFAULT_CLEAR_R, CFG_DEFAULT_CLEAR_G,
    CFG_DEFAULT_CLEAR_B, CFG_DEFAULT_CLEAR_A
};

/* Retain what an authoritative pass observed itself clearing. An unknown
 * observation is dropped rather than stored - retention IS the fallback, so
 * there is nothing else to decide.
 *
 * Under accumulation every sample calls this and the last known one wins.
 * That is an **endpoint approximation, not the resolved background**: the
 * passes are summed at 1/N and glAccum(GL_RETURN) presents their weighted
 * average, so with an animated glClearColor the pixels show the mean of the
 * samples while this retains the final one. They coincide for AA and camera
 * blur, where every sample re-bakes the same clear colour, and diverge only
 * for time blur over a clear colour that moves within the frame - bounded by
 * the parser's cap on clear-colour components, so the error is small. Taking
 * the endpoint is deliberate: it is the sample baked at the true frame time,
 * and weighted background averaging was scoped out of v1 (see
 * docs/plans/done/clear-background-execution-observation.md). Add it only if a
 * real scene demonstrates a visible need. */
static void glr_ctrl_retain_background(const ReplBackgroundObservation *obs) {
    if (obs && obs->known)
        memcpy(g_presentation_rgba, obs->rgba, sizeof(g_presentation_rgba));
}

/* Clear one chrome strip, skipping degenerate rects. */
static void glr_ctrl_clear_strip(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
        return;
    glScissor(x, y, w, h);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* The frame's clear, minus the scene rect.
 *
 * Ownership split: the scene rect is cleared by the *program* - a user
 * glClear, which render3d scissors to that rect. Nothing clears it on the
 * program's behalf, so deleting the glClear line smears, exactly as it
 * would in the exported C. That is the point: a renderer-side clear would
 * run whether or not the source said so, leaving the source's glClear
 * decorative and the real clear invisible.
 *
 * What the program cannot reach is the chrome around the scene - menu bar,
 * status bar, code-panel backdrop - which the 2D overlays paint over
 * afterwards but still need cleared first. That is this function, and it
 * lives here rather than in render3d because chrome is a controller
 * concept: render3d knows the scene rect, not the window layout (same
 * reason the camera transform is loaded here, not there). The four strips
 * tile the window minus the scene rect.
 *
 * Runs AFTER the scene, on the background that scene's own execution was
 * observed to establish - the strips exclude the scene rectangle, so painting
 * them late can neither repair nor disturb a single user pixel, and it is what
 * lets chrome consume the frame's own observation instead of a prediction of
 * it. The load-bearing ordering rule is only that this precedes the 2D
 * overlays that paint over the chrome. */
static void glr_ctrl_clear_chrome(const Render3dRenderConfig *config,
                                  const float rgba[4]) {
    UiViewportState vp = ui_state_viewport();
    int sx = config->render3d_x;
    int sy = config->render3d_y;
    int sw = config->render3d_w;
    int sh = config->render3d_h;

    if (vp.window_w <= 0 || vp.window_h <= 0)
        return;

    /* The program's own glClear left its color live in GL, and render3d
     * established the baseline before that. Own the value here rather than
     * inheriting either: the strips clear to the background this frame's
     * scene actually took, so the two land on the same color. */
    glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    /* The chrome strips clear depth too, and a program glClearDepth leaves
     * its value live in GL after the scene walk. Own the value here rather
     * than inheriting last frame's leftover - same reason as the clear
     * color above. (The view-mode swatch does the same for its cell.) */
    glClearDepth(1.0);
    glPushAttrib(GL_SCISSOR_BIT);
    glEnable(GL_SCISSOR_TEST);
    glr_ctrl_clear_strip(0, 0, sx, vp.window_h);                       /* left  */
    glr_ctrl_clear_strip(sx + sw, 0, vp.window_w - (sx + sw),
                         vp.window_h);                                 /* right */
    glr_ctrl_clear_strip(sx, 0, sw, sy);                               /* below */
    glr_ctrl_clear_strip(sx, sy + sh, sw, vp.window_h - (sy + sh));    /* above */
    glPopAttrib();
}

/* State filter for the winding view (RENDER3D_EXEC_WINDING). The pass in
 * render.c owns a two-sided-lighting setup (front green / back red); these
 * program commands would clobber it, so suppress their GL emission while the
 * geometry, transforms and normals still execute normally. glColor passes
 * through - with lighting on and color-material off, GL ignores it anyway.
 *
 * glPushAttrib/glPopAttrib are intentionally NOT listed: the executor
 * dispatches them in their own case block before the repl_apply_state_cmd
 * cluster, so this filter never sees them. A balanced user push/pop only saves
 * and restores whatever GL state is live (the pass's own setup) and nets out to
 * a no-op; any leftover push is unwound at cursor end and is bounded by this
 * pass's outer glPushAttrib bracket regardless. */
static int winding_state_filter(CmdType type, const GLCmd *cmd, void *ud) {
    (void)ud;
    switch (type) {
    case CMD_MATERIALFV:
    case CMD_MATERIALF:
    case CMD_COLOR_MATERIAL:
    case CMD_LIGHT_MODEL_I:
        return 0;
    case CMD_ENABLE:
    case CMD_DISABLE:
        switch ((GLenum)cmd->args[0]) {
        case GL_LIGHTING:
        case GL_CULL_FACE:
        case GL_COLOR_MATERIAL:
        case GL_LIGHT0:
        case GL_LIGHT1:
        case GL_LIGHT2:
        case GL_LIGHT3:
            return 0;
        default:
            return 1;
        }
    default:
        return 1;
    }
}

/* Scene's geometry callback for the main-fill and depth-probe passes.
 * The signature is intentionally opaque to scene - the controller
 * pulls live program / count / text from REPL state here, and clamps
 * the count to the pre-fade base limit when replay-fade overlays are
 * active so the fade pass can layer on top of an unmodified prefix.
 *
 * For auxiliary purposes (depth probes and the renderer-owned hidden /
 * depth-only wireframe passes), snapshot REPL mutable state before the
 * geometry walk and restore it after. The visible wireframe line pass
 * is the one side-effecting pass when wireframe mode replaces the
 * normal main fill, so assignment-driven animation still advances
 * exactly once per frame. Both the generic executor and the app-owned
 * hidden-line renderer apply CMD_VAR_ASSIGN / CMD_SCRATCH_ASSIGN from
 * precomputed args[]; they also update the GL-light enable
 * bookkeeping. Without this bracket auxiliary passes would advance the
 * user's `t = t + 1` style state alongside the main fill and leak
 * render bookkeeping across frames (the frame-level snapshot in
 * glr_ctrl_display_frame restores predef + scratch only, not the
 * persistent render-state struct).
 *
 * Render3dExecutePurpose is a forward-compatible enum; additional
 * probe-like purposes (fade-overlay, picking pass, etc.) take the same
 * snapshot/restore path automatically. */
static void scene_execute_adapter(const Render3dExecuteContext *ctx,
                                  void *user_data) {
    (void)user_data;

    Render3dExecutePurpose purpose =
        ctx ? ctx->purpose : RENDER3D_EXEC_MAIN_FILL;
    int suppress_side_effects =
        purpose != RENDER3D_EXEC_MAIN_FILL &&
        purpose != RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES &&
        purpose != RENDER3D_EXEC_WINDING;
    int wireframe_effect_pass =
        purpose == RENDER3D_EXEC_WIREFRAME_HIDDEN_LINES ||
        purpose == RENDER3D_EXEC_WIREFRAME_DEPTH_FILL ||
        purpose == RENDER3D_EXEC_WIREFRAME_VISIBLE_LINES;
    /* Which passes may speak for the frame's background. render3d runs
     * exactly one of the three fill paths per accumulation pass, and each
     * paints the scene rect for real:
     *   - MAIN_FILL          the ordinary (and plain-wireframe) geometry pass;
     *   - WINDING            the winding view's two-sided fill;
     *   - WIREFRAME_DEPTH_FILL  the hidden-line effect's depth seed, whose one
     *                        synthetic full-mask clear IS that view's clear.
     * Every other purpose publishes nothing and says so explicitly below
     * rather than by leaving a field zeroed: the depth probe is a feedback
     * walk, the hidden/visible line redraws replay the program over the seed
     * the depth fill laid down, and replay fade overlays suppress CMD_CLEAR
     * outright. */
    int publishes_background =
        purpose == RENDER3D_EXEC_MAIN_FILL ||
        purpose == RENDER3D_EXEC_WINDING ||
        purpose == RENDER3D_EXEC_WIREFRAME_DEPTH_FILL;
    ReplBackgroundObservation observation = {{0.0f, 0.0f, 0.0f, 0.0f}, 0};

    int count = repl_state_flat_program_count();
    if (g_frame_replay_exec_limit >= 0 && g_frame_replay_exec_limit < count)
        count = g_frame_replay_exec_limit;
    if (g_replay_fade_plan.active)
        count = g_replay_fade_plan.base_limit;

    float saved_predef[MAX_PREDEF_VARS];
    float saved_scratch[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    ReplRenderState saved_render = {0};
    if (suppress_side_effects) {
        repl_copy_predef_values(saved_predef, MAX_PREDEF_VARS);
        repl_eval_copy_scratch_arrays(saved_scratch);
        saved_render = repl_state_render();
    }

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    if (wireframe_effect_pass) {
        HiddenLinesRenderContext hl = {
            .flat_cmd_count = count,
            .program        = repl_state_flat_program_view(),
            .observation_out = publishes_background ? &observation : NULL,
        };
        memcpy(hl.baseline_clear_rgba, k_baseline_clear_rgba,
               sizeof(hl.baseline_clear_rgba));
        hidden_lines_execute(&hl, purpose);
    } else {
        ReplExecutionOptions opts = {
            .flat_cmd_count = count,
            .program        = repl_state_flat_program_view(),
            .observation_out = publishes_background ? &observation : NULL,
        };
        memcpy(opts.baseline_clear_rgba, k_baseline_clear_rgba,
               sizeof(opts.baseline_clear_rgba));
        if (purpose == RENDER3D_EXEC_WINDING)
            opts.state_filter = winding_state_filter;
        repl_execute_program(&opts);
    }
    glPopAttrib();

    if (suppress_side_effects) {
        repl_restore_predef_values(saved_predef, MAX_PREDEF_VARS);
        repl_eval_restore_scratch_arrays(saved_scratch);
        repl_state_render_set(&saved_render);
    }

    /* Outside the snapshot/restore bracket above on purpose: the observation
     * is not REPL state to be rewound but a report of pixels this pass wrote,
     * and the hidden-line depth fill - the one publishing pass that IS
     * side-effect-suppressed - must still be able to speak for the frame.
     *
     * Retained here, during the walk, rather than folded and resolved after
     * it: that is what lets one variable serve both vintages (see
     * g_presentation_rgba). Everything built before the walk already read the
     * previous value; the chrome clear runs after and reads this one. */
    if (publishes_background)
        glr_ctrl_retain_background(&observation);
}

/* Grid/axes in-out fade machines.
 * The config path is untouched: toggling still just flips
 * presentation.grid_theme/axes_theme. Each frame the diff feeds these
 * machines and the renderer draws the effective {theme, opacity}. */
static Render3dXnState g_grid_xn;
static Render3dXnState g_axes_xn;

/* Per-renderer scene state (formerly file-static in src/render3d/render.c).
 * The single-renderer assumption is now an instance, not a global -
 * glr_ctrl is one renderer, render3d_demo is another, and tests own
 * theirs. Initialized in glr_ctrl_init_gl. */
static Render3dState g_scene_renderer;

/* Runtime GL_NV_fog_distance capability, probed once in glr_ctrl_init_gl
 * (the GL context is current there) and mirrored into each frame's
 * Render3dRenderConfig. Lets the city backdrop and ocean/radar grid themes
 * opt into radial fog. 0 until detection runs - the safe default. */
static int g_nv_fog_distance_supported = 0;

/* Runtime depth-readback capability (glReadPixels GL_DEPTH_COMPONENT from
 * the default framebuffer), probed once in glr_ctrl_init_gl. WebGL forbids
 * it, so the web build hard-disables. When unsupported the interactive
 * Depth view cycle (Ctrl+N / Config row) refuses with a status message
 * (glr_cfg_cycle_row), while @cfg header loads still write the stored
 * value (round-trip, point-parameter philosophy), build_scene_config forces
 * the render-config copy to Off, and vertex labels remain visible without
 * attempting an occlusion readback. Defaults to 1: tests that never run
 * init-GL keep the feature exercisable. */
static int g_depth_readback_supported = 1;
static int g_stencil_readback_supported = 1;
static int g_stencil_clear_warning_active = 0;

/* ---------------------------------------------------------------------------
 * Vertex-label occlusion depth snapshot.
 *
 * The label pass culls labels whose vertex the scene drew over, which needs
 * the frame's depth buffer. It used to read that itself, mid-pass. On a driver
 * that renders ahead (NVIDIA by default) a synchronous glReadPixels blocks
 * until the whole queued, vsync-throttled pipeline retires, so that read cost
 * ~14 ms and consumed the entire frame budget - not in transfer (0.7-2.5 ms)
 * but in drain. See docs/plans/in-review/vertex-label-depth-readback-stall.md
 * and `make bench-vertex-labels`, which reproduces every variant that does not
 * work (a different buffer, a different position, a PBO, a PBO plus a fence).
 *
 * So the read moved here: the host takes it after the glFinish it already
 * performs at end of frame, where the queue is drained and the wait is already
 * paid, and the *next* frame's label pass consumes it. The cull is therefore
 * one frame stale, which at 60 fps is not perceptible.
 *
 * `valid` means "these pixels describe the scene as of the end of last frame".
 * Only a completed capture sets it. Everything that can make the retained
 * pixels describe a *different* scene clears it - see
 * glr_ctrl_invalidate_depth_snapshot() and its callers. Note that the question
 * "should we capture this frame" and the question "is the buffer we already
 * hold still usable" are separate; answering only the first is what would let
 * a label off/on cycle publish the pre-off buffer to the first pass back.
 * ------------------------------------------------------------------------- */
typedef struct {
    float *pixels;      /* vw*vh window depths, viewport-local, bottom-up */
    int    vx, vy;      /* scene viewport origin the read was taken from  */
    int    vw, vh;
    size_t capacity;    /* floats allocated, so a shrink need not realloc  */
    int    valid;
    /* Editor undo generation the pixels were captured under. Ordinary
     * scene/example/workspace replacement paths bump it through
     * editor_undo_note_wholesale_replacement(), so the read-time comparison
     * below covers those paths without enumerating them. The two exceptional
     * replacement families that preserve or restore that generation
     * (tutorial starts and controlled-tour baseline restore) invalidate the
     * snapshot explicitly at their app-layer boundaries. */
    unsigned int generation;
} GlrDepthSnapshot;

static GlrDepthSnapshot g_depth_snapshot;

/* Did last frame's label pass actually reach the occlusion cull? The capture
 * gate reads this so that labels-on-but-nothing-in-scope keeps costing
 * nothing, which is the property the old lazy read had and the one thing an
 * end-of-frame capture cannot work out for itself - it runs before the pass
 * that would need the pixels. The cost is one frame of latency when labels
 * first come into scope, during which the pass sees no snapshot and draws
 * every label, which is the same fallback an unsupported context gets. */
static int g_depth_snapshot_wanted = 0;

void glr_ctrl_invalidate_depth_snapshot(void) {
    g_depth_snapshot.valid = 0;
}

/* Cleared here rather than only in the capture gate: on a wholesale document
 * or view change the *next* frame's pass must not cull against the previous
 * scene, and it would otherwise be handed a snapshot that is stale rather than
 * absent. */
static void glr_ctrl_depth_snapshot_reset(void) {
    glr_ctrl_invalidate_depth_snapshot();
    g_depth_snapshot_wanted = 0;
}

void glr_ctrl_depth_snapshot_free(void) {
    free(g_depth_snapshot.pixels);
    g_depth_snapshot.pixels = NULL;
    g_depth_snapshot.capacity = 0;
    glr_ctrl_depth_snapshot_reset();
}

void glr_ctrl_set_depth_snapshot_wanted(int wanted) {
    g_depth_snapshot_wanted = wanted ? 1 : 0;
}

/* Published to the label pass through OverlaySnapshotPack. Returns a view with
 * valid == 0 rather than NULL so the consumer has one shape to handle. */
OverlayDepthSnapshot glr_ctrl_depth_snapshot_view(void) {
    OverlayDepthSnapshot view;
    /* A generation-bumping document replacement since the capture makes these
     * pixels the OLD scene's, so refuse them here rather than at each ordinary
     * load site. Checked on read, not on write, because the replacement happens
     * between the capture and the consumption a frame later. */
    int fresh = g_depth_snapshot.valid &&
                g_depth_snapshot.generation == editor_undo_generation();
    view.pixels = fresh ? g_depth_snapshot.pixels : NULL;
    view.vw     = g_depth_snapshot.vw;
    view.vh     = g_depth_snapshot.vh;
    view.valid  = fresh && g_depth_snapshot.pixels != NULL;
    return view;
}

void glr_ctrl_capture_depth_snapshot(void) {
    int vp[4];
    size_t needed;

    /* Every early return leaves the snapshot invalid, never merely unrefreshed:
     * publishing last frame's pixels as this frame's is the failure mode this
     * whole block is written to avoid. */
    if (!g_depth_snapshot_wanted || !g_depth_readback_supported) {
        glr_ctrl_invalidate_depth_snapshot();
        return;
    }

    /* The SCENE rect, asked for explicitly - NOT the ambient GL_VIEWPORT.
     *
     * This runs at the end of the frame, after the code panel, the HUDs and
     * the compositor have each set the viewport to the whole window for their
     * own 2D passes. Reading ambient state here captures a window-sized buffer,
     * which the label pass then rejects next frame because its dimensions
     * disagree with the scene rect it projects into - so the app pays for a
     * LARGER readback and gets no occlusion culling at all, silently. The
     * symptom is invisible in a profile (the stall is still gone) and invisible
     * on screen unless you are looking for labels showing through geometry.
     *
     * ui_layout_scene_rect() is the same source render3d_x/y/w/h comes from in
     * glr_ctrl_build_scene_config(), so the capture rect is the projection
     * rect by construction rather than by luck. */
    ui_layout_scene_rect(&vp[0], &vp[1], &vp[2], &vp[3]);
    if (vp[2] <= 0 || vp[3] <= 0) {
        glr_ctrl_invalidate_depth_snapshot();
        return;
    }

    needed = (size_t)vp[2] * (size_t)vp[3];
    if (needed > g_depth_snapshot.capacity) {
        float *grown = (float *)realloc(g_depth_snapshot.pixels,
                                        needed * sizeof(float));
        if (!grown) {
            /* Keep whatever we had allocated - it is still a valid heap block -
             * but it no longer describes anything, so it must not be published. */
            glr_ctrl_invalidate_depth_snapshot();
            return;
        }
        g_depth_snapshot.pixels = grown;
        g_depth_snapshot.capacity = needed;
    }

    prof_begin(PROF_DEPTH_SNAPSHOT);
    (void)glGetError();
    glReadPixels(vp[0], vp[1], vp[2], vp[3], GL_DEPTH_COMPONENT, GL_FLOAT,
                 g_depth_snapshot.pixels);
    /* The startup probe reads one pixel and cannot promise that every later
     * full-viewport read succeeds, so the read is checked per capture. A failed
     * read leaves the buffer holding indeterminate contents; culling against
     * those would drop labels at random. */
    if (glGetError() != GL_NO_ERROR) {
        glr_ctrl_invalidate_depth_snapshot();
    } else {
        g_depth_snapshot.vx = vp[0];
        g_depth_snapshot.vy = vp[1];
        g_depth_snapshot.vw = vp[2];
        g_depth_snapshot.vh = vp[3];
        g_depth_snapshot.generation = editor_undo_generation();
        g_depth_snapshot.valid = 1;
    }
    prof_end(PROF_DEPTH_SNAPSHOT);
}

/* "This is a Mesa-family renderer" - probed once in glr_ctrl_init_gl from the
 * renderer/vendor strings. Two consumers, both of them cases where Mesa's
 * implementation choices make a feature a net loss rather than a capability
 * question, so no probe or extension bit can answer them:
 *
 *   - glr_ctrl_set_accum's AUTO mode. Distinct from accum_bits == 0 (no accum
 *     buffer at all): Mesa reports a real width and glAccum genuinely works,
 *     it is just implemented by mapping the color buffer and adding with the
 *     CPU, which costs more per pass than the extra scene render it is
 *     supposed to be smoothing.
 *   - the code panel's syntax-highlight default (see the init_gl call to
 *     glr_state_set_default_syntax_highlight).
 *
 * 0 until detection runs, so paths that never init GL (tests, render3d_demo,
 * dump-only runs) keep both features armed. */
static int g_renderer_is_mesa = 0;

/* GL_RENDERER, or a placeholder when the context refuses the query - only
 * ever used for human-readable diagnostics. */
static const char *glr_ctrl_gl_renderer_name(void) {
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    return (renderer && renderer[0]) ? renderer : "this renderer";
}

/* Is this a Mesa-family renderer? Every driver Mesa ships behaves the same way
 * for both consumers above - hardware Intel/AMD/nouveau included, not just
 * llvmpipe/softpipe/swrast - because the behavior comes from the shared state
 * tracker, not the backend. So the vendor/renderer string, not a capability
 * bit, is the signal. */
#if defined(__EMSCRIPTEN__)
/* The web build is deliberately exempt. The browser's underlying WebGL
 * renderer string often names Mesa, but nothing downstream of it is Mesa's:
 * gl4es owns both paths the flag drives. It has no accumulation buffer to
 * emulate badly (packaging/web/patches/gl4es-accum-fbo.patch implements
 * glAccum against a real GPU-side FBO, which the app picks up through the
 * accum_bits probe), and it draws bitmap text through its own WebGL2
 * translation rather than Mesa's raster path. */
static int glr_ctrl_renderer_is_mesa(void) { return 0; }
#else
/* Case-insensitive substring test (no strcasestr: POSIX-only, and the
 * project builds -std=c99). */
static int glr_ctrl_str_contains_ci(const char *hay, const char *needle) {
    size_t nl;
    if (!hay || !needle) return 0;
    nl = strlen(needle);
    if (nl == 0) return 0;
    for (; *hay; hay++) {
        size_t k = 0;
        while (hay[k] && needle[k] &&
               tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k]))
            k++;
        if (k == nl) return 1;
    }
    return 0;
}

static int glr_ctrl_renderer_is_mesa(void) {
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    /* "mesa" alone misses the software rasterizers, whose renderer string is
     * just the pipe name ("llvmpipe (LLVM 17.0.6, 256 bits)"). */
    static const char *const k_mesa[] = {
        "mesa", "llvmpipe", "softpipe", "swrast", "lavapipe"
    };
    for (size_t i = 0; i < ARRAY_LEN(k_mesa); i++) {
        if (glr_ctrl_str_contains_ci(renderer, k_mesa[i]) ||
            glr_ctrl_str_contains_ci(vendor, k_mesa[i]))
            return 1;
    }
    return 0;
}
#endif

void glr_ctrl_set_depth_readback_supported_for_test(int supported) {
    g_depth_readback_supported = supported ? 1 : 0;
    /* Losing the capability must drop whatever was already captured, not just
     * stop future captures - the retained pixels would otherwise keep being
     * published to a context that is no longer allowed to have them. */
    if (!g_depth_readback_supported)
        glr_ctrl_invalidate_depth_snapshot();
}

void glr_ctrl_set_stencil_readback_supported_for_test(int supported) {
    g_stencil_readback_supported = supported ? 1 : 0;
}

const char *glr_ctrl_depth_readback_unsupported_reason(void) {
    if (g_depth_readback_supported)
        return NULL;
#if defined(__EMSCRIPTEN__)
    return "Depth view unavailable: WebGL context can't read the depth "
           "buffer (works in the native build)";
#else
    return "Depth view unavailable: GL context can't read the depth buffer";
#endif
}

const char *glr_ctrl_stencil_readback_unsupported_reason(void) {
    if (g_stencil_readback_supported)
        return NULL;
    if (glr_state_render().stencil_bits == 0)
        return "Stencil view unavailable: GL context has no stencil buffer";
#if defined(__EMSCRIPTEN__)
    return "Stencil view unavailable: WebGL context can't read the stencil "
           "buffer (works in the native build)";
#else
    return "Stencil view unavailable: GL context can't read the stencil buffer";
#endif
}

/* Per-frame buffer-visualization modes. render3d fires three neutral
 * buffer hooks and knows nothing about what for; buffer_viz subscribes
 * them and reads this struct as their user_data (the g_overlay_pack
 * idiom), so what a frame asked for is explicit at the subscription site
 * rather than ambient in the subsystem. Capability masking happens where
 * it is built, in glr_ctrl_build_scene_config. */
static BufferVizFrameConfig g_buffer_viz_frame;

/* The 2D/3D view-mode transition state machine lives in
 * src/app/glr_ctrl_view_transition.c (carved out of this file). The frame
 * loop ticks it via glr_ctrl_tick_view_transition; the scene-config builder
 * reads the blend via glr_ctrl_view_projection_mix; reset_all calls
 * glr_ctrl_view_reset - all declared in glr_ctrl_internal.h. */

static void glr_ctrl_build_scene_config(FlatProgramView flat_program, Render3dRenderConfig *config) {
    GlrRenderState render = glr_state_render();
    ReplRenderState repl_render = repl_state_render();
    GlrPresentationState presentation = glr_state_presentation();
    GlrCameraState cam = glr_camera();
    const float *grid_major_steps = glr_state_grid_major_steps();
    const float *grid_extents = glr_state_grid_extents();
    float bg_lum;
    float as_val;

    /* Refresh cursor block highlight before reading cursor state */
    repl_flatten_refresh_current_block_highlight(editor_state_edit_line());

    /* --- Execute hook --- */
    config->execute_fn = scene_execute_adapter;
    config->execute_user_data = NULL;

    /* --- Post-fill hook (replay-fade overlay) ---
     * Wired up further below after the fade state is built; left NULL when
     * there are no fades to overlay. */
    config->post_fill_fn = NULL;
    config->post_fill_user_data = NULL;

    /* --- Post-overlays hook ---
     * Always wired; the body short-circuits per-overlay based on REPL
     * presentation flags. user_data carries the per-frame config so the
     * hook can read guide_snapshot for the cursor edit guides. The
     * pack itself is built at the END of this function (see
     * "Build overlay snapshot pack" below) so it reads the fully-
     * populated config fields (multisample_enabled, line_smooth_enabled,
     * alpha_scale) - those are assigned later in
     * this function, so building the pack here would read stack garbage. */
    config->post_overlays_fn        = edit_overlays_post_overlays;
    config->post_overlays_user_data = &g_overlay_pack;

    /* --- Post-resolve overlays hook (bitmap-text labels) ---
     * Same pack, but invoked once per frame after the accumulation
     * resolve so AA jitter / blur sub-passes can't ghost the
     * pixel-snapped label glyphs. */
    config->post_resolve_overlays_fn        = edit_overlays_post_resolve_overlays;
    config->post_resolve_overlays_user_data = &g_overlay_pack;

    /* --- Background colors ---
     * Two values, two jobs. Every scoped frame's walk starts from the
     * bootstrap default, so that is what render3d establishes as GL's clear
     * color; the program's own glClearColor moves it from there, in execution
     * order, exactly as the exported C does under its
     * glPushAttrib/glPopAttrib bracket.
     *
     * What the frame ends up *showing* is not predicted here at all: the
     * executor observes it while emitting the clears, and the controller
     * retains the last fully-known answer in g_presentation_rgba. The grid /
     * axes recede fog fades to that, and alpha_scale below lights overlays
     * against it - one number, so those consumers cannot disagree with each
     * other.
     *
     * They get the PREVIOUS frame's observation, and that is a consequence of
     * this function running before the walk, not a second stored value: the
     * chrome strips read the very same variable after the walk and therefore
     * see the current one. So on the frame a background *changes*, fog and
     * overlay alpha are one frame behind while chrome and the scene rect are
     * already correct - no accumulating error, and no invalidation protocol.
     *
     * Retention is also what keeps replay steady: a prefix that has not yet
     * executed an effective color clear publishes nothing known, so the
     * background holds instead of flickering to the default as the PC walks
     * toward the clear. */
    memcpy(config->baseline_clear_color, k_baseline_clear_rgba,
           sizeof(config->baseline_clear_color));
    memcpy(config->presentation_rgba, g_presentation_rgba,
           sizeof(config->presentation_rgba));

    /* --- Animation --- */
    config->anim_time = repl_state_variables().anim_time;

    /* --- Viewport and scene rectangle --- */
    ui_layout_scene_rect(&config->render3d_x, &config->render3d_y,
                           &config->render3d_w, &config->render3d_h);
    if (config->render3d_w < 1) config->render3d_w = 1;
    if (config->render3d_h < 1) config->render3d_h = 1;

    /* --- Camera state --- (modelview transform is applied separately by
     * the controller via glr_camera_load_modelview; these fields are
     * still needed for grid/axes orientation and the orbit-target
     * gizmo). */
    config->cam_dist = cam.dist;
    config->cam_rx = cam.rx;
    config->cam_ry = cam.ry;
    config->cam_tx = cam.tx;
    config->cam_ty = cam.ty;
    config->cam_tz = cam.tz;
    config->cam_motion_glow = cam.motion_glow;
    /* Two independent projection blends combined by min() (0 = ortho, 1 =
     * perspective): the 2D/3D view-mode transition and the free-camera
     * Projection toggle. Min => ortho wins - the scene is orthographic if
     * EITHER control asks for it. The view-mode 2D path additionally
     * flattens/locks the camera; the toggle leaves the camera free. */
    config->projection_mix = fminf(glr_ctrl_view_projection_mix(),
                                   glr_ctrl_projection_toggle_mix());

    /* --- Rendering quality --- */
    config->multisample_enabled = render.multisample_enabled;
    config->line_smooth_enabled = render.line_smooth_enabled;
    config->use_accum = render.use_accum;
    config->accum_effect = render.accum_effect;
    config->accum_passes = render.accum_passes;
    config->use_accum_aa_scissors = CFG_DEFAULT_USE_ACCUM_AA_SCISSORS;
    /* Blur sub-frame hook is resolved per frame in glr_ctrl_display_frame
     * (it needs the captured current camera pose); default to no hook so
     * effect==BLUR degrades to the AA jitter path until wired. */
    config->setup_subframe_fn = NULL;
    config->setup_subframe_user_data = NULL;

    /* --- Lighting ---
     * Merge the app-owned dimensional light data (theme-seeded positions /
     * colors / eye-space, in glr_state) with the REPL-owned enable bitmask
     * (light_enabled_mask, written by the executor as the program runs).
     * The light-indicator overlay reads the `.enabled` flags from here. */
    for (int i = 0; i < MAX_LIGHTS; i++) {
        config->lights[i] = render.lights[i];
        config->lights[i].enabled = repl_light_enabled(repl_render.light_enabled_mask, i);
    }
    config->show_light_indicators = presentation.show_light_indicators;

    /* Emphasize the indicator for the light whose glEnable/glDisable line the
     * cursor is on. Only meaningful when the indicators are drawn, so skip the
     * work otherwise. Prefer the live input buffer (so a still-being-typed
     * "glEnable(GL_LIGHT2" highlights immediately); fall back to the committed
     * source cmd, mapping GL_LIGHTn (args[0]) to a slot as the executor does. */
    config->highlight_light_slot = -1;
    if (config->show_light_indicators) {
        config->highlight_light_slot = light_slot_from_text(editor_state_input().input);
        if (config->highlight_light_slot < 0) {
            int edit_line = editor_state_edit_line();
            if (edit_line >= 0 && edit_line < repl_state_document_count()) {
                const GLCmd *cmd = repl_state_document_cmd_at(edit_line);
                if (cmd && (cmd->type == CMD_ENABLE || cmd->type == CMD_DISABLE)) {
                    int slot = (int)cmd->args[0] - (int)GL_LIGHT0;
                    if (slot >= 0 && slot < MAX_LIGHTS)
                        config->highlight_light_slot = slot;
                }
            }
        }
    }

    /* --- Environment --- */
    config->backdrop_mode = presentation.backdrop_mode;
    config->post_filter_mode = presentation.post_filter_mode;
    config->wireframe = presentation.wireframe;
    config->winding_view = presentation.winding_view;

    /* --- Buffer-visualization hooks ---
     * Capability masking is the controller's job (the subsystem is told a
     * mode, never asked to second-guess it): a context that cannot read
     * GL_DEPTH_COMPONENT gets Off regardless of the stored config value,
     * which stays intact so @cfg round-trips. */
    g_buffer_viz_frame.depth_mode =
        g_depth_readback_supported ? presentation.depth_viz
                                   : BUFFER_VIZ_DEPTH_OFF;
    g_buffer_viz_frame.stencil_mode =
        g_stencil_readback_supported ? presentation.stencil_viz
                                     : BUFFER_VIZ_STENCIL_OFF;
    buffer_viz_install(config, &g_buffer_viz_frame);
    /* Single source of truth: the executor's capability flag + loaded
     * proc set in glr_ctrl_init_gl. Lets the star backdrop reset point
     * attenuation through the same callable entry point the executor
     * uses for CMD_POINT_PARAMETER_FV. */
    config->point_parameter_supported = repl_executor_point_parameter_supported();
    config->point_parameter_proc = config->point_parameter_supported
        ? repl_executor_point_parameter_proc()
        : NULL;
    /* Scoped radial fog: probed once in glr_ctrl_init_gl. Only the city
     * backdrop and ocean/radar grid passes act on it. */
    config->nv_fog_distance_supported = g_nv_fog_distance_supported;

    /* --- Grid and axes ---
     * Effective theme/opacity come from the transition machines (ticked
     * in glr_ctrl_tick), NOT directly from presentation: `current` is
     * the theme to draw while `next` may already point elsewhere mid
     * fade-out. */
    config->grid_theme = g_grid_xn.current;
    config->grid_opacity = render3d_xn_opacity(&g_grid_xn);
    config->grid_xn_phase = g_grid_xn.phase;
    config->grid_extent_idx = presentation.grid_extent_idx;
    config->grid_major_idx = presentation.grid_major_idx;
    /* Resolve the Grid brightness index to an alpha multiplier the scene
     * folds into grid-line color (grid.c). NORMAL == 1.0 preserves the
     * historic look; the rest dial the deliberately-faint grid lines down
     * or up for contrast against the backdrop. Indexed by Render3dGridBrightness
     * (themes.h); out-of-range guards to NORMAL. */
    static const float k_grid_brightness_factors[GRID_BRIGHTNESS_COUNT] = {
        [GRID_BRIGHTNESS_DIM]    = 0.25f,
        [GRID_BRIGHTNESS_NORMAL] = 1.2f,
        [GRID_BRIGHTNESS_BRIGHT] = 2.5f,
        [GRID_BRIGHTNESS_BOLD]   = 4.5f,
    };
    int br_i = presentation.grid_brightness_idx;
    if (br_i < 0 || br_i >= GRID_BRIGHTNESS_COUNT) br_i = GRID_BRIGHTNESS_NORMAL;
    config->grid_brightness = k_grid_brightness_factors[br_i];
    config->grid_brightness_idx = br_i;
    config->axes_theme = g_axes_xn.current;
    config->axes_opacity = render3d_xn_opacity(&g_axes_xn);
    config->axes_xn_phase = g_axes_xn.phase;
    memcpy(config->grid_major_steps, grid_major_steps,
           sizeof(config->grid_major_steps));
    memcpy(config->grid_extents, grid_extents,
           sizeof(config->grid_extents));

    /* --- Replay-mode overlay state ---
     * post_fill_fn / post_overlays_fn read these directly from REPL
     * state; scene config no longer carries them. */
    int replaying = replay_active();

    /* --- Focus marker --- */
    config->focus = glr_ctrl_build_focus_vertex();

    /* Boost overlay alpha on dark backgrounds: bg_lum is the Rec. 709
     * relative luminance of the presentation background (0.2126/0.7152/0.0722
     * R/G/B weights); the reciprocal raises alpha as the background darkens,
     * with +0.02 as a black-background guard and the result clamped to
     * 1..3 below. The derivation stays here, in the controller: render3d
     * receives the finished scale and never recomputes it from the color. */
    bg_lum = 0.2126f * config->presentation_rgba[0]
        + 0.7152f * config->presentation_rgba[1]
        + 0.0722f * config->presentation_rgba[2];
    as_val = (CFG_DEFAULT_CLEAR_LUMA + 0.02f) /
             fmaxf(bg_lum + 0.02f, 1e-4f);
    config->alpha_scale = as_val < 1.0f ? 1.0f : (as_val > 3.0f ? 3.0f : as_val);

    /* --- Replay overlays ---
     * Build the controller-private fade state from REPL replay state, and
     * if there's anything to overlay on the main fill (fading batches or
     * the polygon-mode tess-preview wireframe) install our post_fill_fn
     * so the scene calls back between the user-geometry fill and the
     * grid/axes/backdrop helpers. */
    glr_ctrl_build_replay_fade_plan(flat_program, replaying);
    g_replay_fade_plan.tess_preview_active = replaying &&
                                             replay_mode() == REPLAY_MODE_VERTEX;
    if (g_replay_fade_plan.active || g_replay_fade_plan.tess_preview_active) {
        config->post_fill_fn        = replay_render_post_fill;
        config->post_fill_user_data = &g_replay_fade_plan;
    }

    /* --- Build overlay snapshot pack ---
     * Done at the end so the pack reads the fully-populated config
     * fields (multisample_enabled, line_smooth_enabled,
     * user_lighting_enabled, alpha_scale). Moving this earlier in
     * the function would copy stack garbage through
     * glr_ctrl_build_guide_snapshot(). */
    glr_ctrl_build_overlay_pack(&g_overlay_pack, config);
}

/* ========================================================================= */
/* UI snapshot builder (push model)                                           */
/* ========================================================================= */

static void glr_ctrl_fill_ui_variable_panel_vars(UiRenderSnapshot *snap,
                                                    ReplPredefView predef) {
    int count = predef.count;
    int written_slots[MAX_PREDEF_VARS];

    if (count < 0 || !predef.vars)
        count = 0;
    if (count > MAX_PREDEF_VARS)
        count = MAX_PREDEF_VARS;

    /* Which predef vars carry a `// @tune` tag (badged in the panel as
     * exported keyboard knobs). Query all matches (not just the knob cap) so
     * every tagged row is badged. */
    const char *tuned_names[MAX_PREDEF_VARS];
    int tuned_count = repl_collect_tuned_vars(
        repl_state_document_cmds(), repl_state_document_count(),
        source_document_view(), tuned_names, MAX_PREDEF_VARS, NULL);
    /* Vars on a `// @config` decl line stay bright even when assigned -
     * the tag says the writes are bounds-keeping (clamp/floor), not
     * program state, so the panel keeps presenting them as knobs. */
    const char *config_names[MAX_PREDEF_VARS];
    int config_count = repl_collect_config_vars(
        repl_state_document_cmds(), repl_state_document_count(),
        source_document_view(), config_names, MAX_PREDEF_VARS, NULL);
    repl_mark_written_var_slots(repl_state_document_cmds(),
                                repl_state_document_count(),
                                written_slots, MAX_PREDEF_VARS);

    snap->variable_panel_vars.vars = snap->variable_panel_var_storage;
    snap->variable_panel_vars.count = count;
    for (int i = 0; i < count; i++) {
        snprintf(snap->variable_panel_var_storage[i].name,
                 sizeof(snap->variable_panel_var_storage[i].name),
                 "%s", predef.vars[i].name);
        snap->variable_panel_var_storage[i].value = &predef.vars[i].value;
        int tuned = 0;
        for (int t = 0; t < tuned_count; t++) {
            if (strcmp(tuned_names[t], predef.vars[i].name) == 0) {
                tuned = 1;
                break;
            }
        }
        snap->variable_panel_var_storage[i].tuned = tuned;
        int config = 0;
        for (int c = 0; c < config_count; c++) {
            if (strcmp(config_names[c], predef.vars[i].name) == 0) {
                config = 1;
                break;
            }
        }
        snap->variable_panel_var_storage[i].written =
            written_slots[i] && !config;
    }
}

static int glr_ctrl_leading_ws_chars(const char *text) {
    int count = 0;

    while (text && text[count] && isspace((unsigned char)text[count]))
        count++;
    return count;
}

static int glr_ctrl_active_indent_chars(void) {
    int edit_line = editor_state_edit_line();
    int document_count = repl_state_document_count();

    if (editor_insert_mode())
        return repl_source_scope_cmd_indent_chars(edit_line);
    if (edit_line >= 0 && edit_line < document_count) {
        const char *line_text = editor_buffer_line(edit_line);
        if (!line_text || line_text[0] == '\0')
            return repl_source_scope_cmd_indent_chars(edit_line);
        return glr_ctrl_leading_ws_chars(line_text);
    }
    return repl_source_scope_cmd_indent_chars(document_count);
}

/* Constants in snapshot.h are hardcoded to keep the UI layer free of
 * repl-layer includes; assert equivalence here where repl/core.h is in
 * scope. */
STATIC_ASSERT(UI_SCENE_TAB_CAP >= MAX_USER_SCENES + 1,
              "scene-tab cap must fit every user slot plus the example tab");
STATIC_ASSERT(UI_SCENE_TAB_NAME_MAX == USER_SCENE_NAME_MAX,
              "scene-tab name buffer must match the user-scene name max");
STATIC_ASSERT(UI_WORKSPACE_NAME_MAX == WORKSPACE_IO_NAME_MAX,
              "workspace-chip name buffer must match the manifest name max");
/* Same rationale for the snapshot's resolved reshape-projection block:
 * snapshot.h hardcodes the dimensions (UI-layer purity), repl/export.h
 * owns the source-of-truth, equivalence is asserted here. */
STATIC_ASSERT(UI_RESHAPE_PROJ_LINES == REPL_EXPORT_PROJ_LINES,
              "snapshot reshape-projection line count must match repl/export");
STATIC_ASSERT(UI_RESHAPE_PROJ_LINE_MAX == REPL_EXPORT_PROJ_LINE_MAX,
              "snapshot reshape-projection line width must match repl/export");
STATIC_ASSERT(UI_GL_VECTOR_HELPER_MAX ==
              REPL_EXPORT_GL_VECTOR_HELPER_MAX_LINES,
              "snapshot GL vector helper line count must match repl/export");

/* Derive the scene tab strip each frame from existing state - no persistent
 * model. One tab per occupied user-scene slot (dense slot order, matching
 * the Scene menu / F12), plus one example tab iff an example is active.
 * Transient/"neither active" (Scene->New, tutorial) leaves active_idx == -1
 * with no synthetic tab. */
static void glr_ctrl_build_scene_tabs(UiSceneTabList *out) {
    int active_slot = repl_active_user_scene();
    int example_idx = repl_state_scenes().active_example_idx;
    int n = 0;

    memset(out, 0, sizeof(*out));
    out->active_idx = -1;

    for (int slot = 0; slot < MAX_USER_SCENES && n < UI_SCENE_TAB_CAP; slot++) {
        if (!repl_user_scene_slot_used(slot))
            continue;
        snprintf(out->tabs[n].name, UI_SCENE_TAB_NAME_MAX, "%s",
                 repl_user_scene_name(slot));
        out->tabs[n].kind = UI_SCENE_TAB_USER;
        out->tabs[n].slot = slot;
        out->tabs[n].active = (slot == active_slot);
        if (out->tabs[n].active)
            out->active_idx = n;
        n++;
    }

    if (example_idx >= 0 && n < UI_SCENE_TAB_CAP) {
        snprintf(out->tabs[n].name, UI_SCENE_TAB_NAME_MAX, "%s",
                 repl_example_name(example_idx));
        out->tabs[n].kind = UI_SCENE_TAB_EXAMPLE;
        out->tabs[n].slot = -1;
        out->tabs[n].active = (active_slot < 0);
        if (out->tabs[n].active)
            out->active_idx = n;
        n++;
    }

    out->count = n;
}

/* Keep the window title naming the active workspace binding and scene:
 * "gl-repl - Workspace: <workspace> | <scene>". If no workspace is bound, the workspace
 * segment is omitted: "gl-repl - <scene>" or just "gl-repl". The scene half is
 * omitted only when neither a user scene nor an example is active (Scene -> New,
 * or a running tutorial), matching the scene tab strip's "no synthetic tab" rule.
 *
 * ASCII separators only: X11's XStoreName takes Latin-1, so a nicer em dash
 * would not survive the trip on Linux.
 *
 * Called every frame, but glutSetWindowTitle only fires when the composed
 * string actually changes - the per-frame cost is two snprintfs and a strcmp,
 * with no filesystem work (glr_workspaces_active_name memoizes its manifest
 * read) and no window-system traffic. */
static void glr_ctrl_refresh_window_title(void) {
#ifdef __EMSCRIPTEN__
    /* Emscripten's glutSetWindowTitle is a no-op, so skip the string formatting and strcmp. */
    return;
#endif
    static char pushed[GLR_WINDOW_TITLE_MAX];
    char title[GLR_WINDOW_TITLE_MAX];
    char scene_part[USER_SCENE_NAME_MAX + 16];
    const char *workspace = glr_workspaces_active_name();
    const char *scene = NULL;
    const char *scene_label = "";
    int in_tutorial = tutorial_active();
    int slot = repl_active_user_scene();
    int example_idx = repl_state_scenes().active_example_idx;

    if (in_tutorial) {
        scene = repl_tutorial_name(tutorial_state_view().tutorial_idx);
        scene_label = "Tutorial: ";
    } else if (slot >= 0) {
        scene = repl_user_scene_name(slot);
    } else if (example_idx >= 0) {
        scene = repl_example_name(example_idx);
        scene_label = "Example: ";
    }

    if (workspace && workspace[0]) {
        scene_part[0] = '\0';
        if (scene && scene[0])
            snprintf(scene_part, sizeof(scene_part), " | %s%s", scene_label, scene);
        snprintf(title, sizeof(title), "%s - Workspace: %s%s", GLR_WINDOW_TITLE_BASE,
                 workspace, scene_part);
    } else {
        if (scene && scene[0]) {
            snprintf(title, sizeof(title), "%s - %s%s", GLR_WINDOW_TITLE_BASE,
                     scene_label, scene);
        } else {
            snprintf(title, sizeof(title), "%s", GLR_WINDOW_TITLE_BASE);
        }
    }

    if (strcmp(title, pushed) == 0)
        return;
    snprintf(pushed, sizeof(pushed), "%s", title);
    glutSetWindowTitle(title);
}

static const char *glr_ctrl_unbalanced_warning_for_type(CmdType type) {
    switch (type) {
    case CMD_BEGIN:        return "missing glEnd";
    case CMD_PUSH_MATRIX:  return "missing glPopMatrix";
    case CMD_PUSH_ATTRIB:  return "missing glPopAttrib";
    case CMD_END:          return "unmatched glEnd";
    case CMD_POP_MATRIX:   return "unmatched glPopMatrix";
    case CMD_POP_ATTRIB:   return "unmatched glPopAttrib";
    default:               return NULL;
    }
}

static void glr_ctrl_fill_unbalanced_warning(UiRenderSnapshot *snap,
                                              const int *lines,
                                              int count) {
    const GLCmd *cmds;
    const char *detail;

    if (!snap || count <= 0)
        return;

    if (count != 1) {
        snprintf(snap->unbalanced_warning,
                 sizeof snap->unbalanced_warning,
                 "%d unbalanced", count);
        return;
    }

    cmds = repl_state_document_cmds();
    detail = (lines && lines[0] >= 0 && lines[0] < repl_state_document_count() &&
              cmds)
                 ? glr_ctrl_unbalanced_warning_for_type(cmds[lines[0]].type)
                 : NULL;
    if (detail)
        snprintf(snap->unbalanced_warning,
                 sizeof snap->unbalanced_warning, "%s", detail);
    else
        snprintf(snap->unbalanced_warning,
                 sizeof snap->unbalanced_warning,
                 "%d unbalanced", count);
}

static void glr_ctrl_populate_numeric_swatch(UiRenderSnapshot *snap) {
    EditorInputView in;
    int edit_line;
    CmdType edit_type;
    ReplNumericArgAtCursor d;
    float anchor_y;
    int cp_x, cp_w;
    char parse_err[REPL_DIAG_TEXT_MAX] = "";
    ReplParsedLine pl;
    ExprVar vis_vars[MAX_EXPR_VARS];
    int num_vis_vars;

    snap->numeric_swatch.visible = 0;

    if (editor_insert_mode()) return;
    edit_line = editor_state_edit_line();
    if (edit_line < 0 || edit_line >= repl_state_document_count()) return;
    edit_type = repl_state_document_cmds()[edit_line].type;
    if (edit_type == CMD_COMMENT) return;
    if (ui_repl_code_panel_input_row_has_color_swatch(snap)) return;
    in = editor_state_input();
    if (in.cursor_pos < 0 || !in.input || !in.input[0]) return;
    if (editor_state_autocomplete()->match_count > 0) return;
    if (editor_inline_rename_active()) return;
    /* Pure predicate on purpose - this runs every frame, and the
     * status-setting twin would stomp the active step's message. */
    if (tutorial_step_rejects_commit()) return;

    d = repl_eval_numeric_arg_at_cursor(in.input, in.cursor_pos);
    if (!d.found) return;

    {
        ReplSourceScopeView source_scope;
        memset(vis_vars, 0, sizeof vis_vars);
        num_vis_vars = collect_visible_vars(edit_line, vis_vars,
                                            MAX_EXPR_VARS, NULL);
        repl_source_scope_view_bind(&source_scope,
                                    repl_state_document_cmds(),
                                    repl_state_document_count());
        ReplParseContext parse_ctx = {
            .source_line_idx = edit_line,
            .vars = vis_vars,
            .num_vars = num_vis_vars,
            .err_buf = parse_err,
            .err_sz = (int)sizeof parse_err,
            .func_aliases = repl_func_alias_view(),
            .source_scope = &source_scope,
        };
        if (!repl_parser_parse_command_ctx(in.input, &pl, &parse_ctx)) {
            if (edit_type != CMD_VAR_ASSIGN &&
                edit_type != CMD_VAR_DECLARE &&
                edit_type != CMD_SCRATCH_ASSIGN &&
                edit_type != CMD_SCRATCH_BLOCK_ASSIGN)
                return;
            memset(&pl, 0, sizeof pl);
            pl.cmd.type = edit_type;
        }
    }
    if (pl.cmd.type == CMD_COMMENT) return;

    ui_layout_code_panel_rect(&cp_x, NULL, &cp_w, NULL);
    if (!ui_repl_code_panel_input_row_y(snap, &anchor_y)) return;

    snap->numeric_swatch.visible   = 1;
    snap->numeric_swatch.arg_start = d.arg_start;
    snap->numeric_swatch.arg_end   = d.arg_end;
    snap->numeric_swatch.value     = d.value;
    snap->numeric_swatch.step      = repl_eval_swatch_step(d.value);
    snap->numeric_swatch.anchor_x  = (float)(cp_x + cp_w -
                                     UI_NUMERIC_SWATCH_BTN_W - 4);
    snap->numeric_swatch.anchor_y  = anchor_y;
}

static int glr_ctrl_current_begin_block_source_extent(int edit_line,
                                                      int *out_start,
                                                      int *out_end) {
    const GLCmd *cmds = repl_state_document_cmds();
    int count = repl_state_document_count();
    int open_begin = -1;

    if (out_start) *out_start = -1;
    if (out_end) *out_end = -1;
    if (edit_line < 0 || edit_line >= count)
        return 0;

    for (int i = 0; i < count; i++) {
        if (!cmds[i].valid)
            continue;

        if (cmds[i].type == CMD_BEGIN) {
            open_begin = i;
        } else if (cmds[i].type == CMD_END && open_begin >= 0) {
            if (edit_line >= open_begin && edit_line <= i) {
                if (out_start) *out_start = open_begin;
                if (out_end) *out_end = i;
                return 1;
            }
            open_begin = -1;
        }
    }

    if (open_begin >= 0 && edit_line >= open_begin) {
        if (out_start) *out_start = open_begin;
        if (out_end) *out_end = count - 1;
        return 1;
    }
    return 0;
}

/* Label for one assignment-plot series: the left-hand side of its row, as
 * the user wrote it ("angle", "A[i]"). Derived from the editor buffer rather
 * than from the GLCmd because per-line canonical text lives there and nowhere
 * else - and because re-deriving it every frame means an edited row retitles
 * its plot instead of stranding the label it opened with.
 *
 * `line_idx` is -1 when nothing is plotted, in which case the title is empty
 * and the panel is not drawn anyway. */
static void glr_ctrl_assign_plot_title(int line_idx, char *out, size_t out_sz) {
    const char *text;
    const char *eq;
    size_t len;

    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (line_idx < 0) return;

    text = editor_buffer_line(line_idx);
    if (!text) return;

    /* First '=' ends the target. An assignment's LHS is a name or a subscript,
     * so there is no earlier '=' to trip over. */
    eq = strchr(text, '=');
    len = eq ? (size_t)(eq - text) : strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t')) len--;
    while (len > 0 && (text[0] == ' ' || text[0] == '\t')) { text++; len--; }
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, text, len);
    out[len] = '\0';
}

void glr_ctrl_build_ui_snapshot(UiRenderSnapshot *snap) {
    FlatProgramView flat_program;
    ReplPredefView predef;
    memset(snap, 0, sizeof(*snap));

    /* Refresh derived workspace header lines so the import/export view
     * reflects the current frame before rendering reads it. */
    repl_state_refresh_workspace_header_lines();

    /* Mirror chrome-relevant presentation fields into ui_state.code_panel
     * so ui_*.c renderers and hit-tests can read them via ui_state_*()
     * without crossing the repl_state_*() boundary. */
    glr_ctrl_sync_ui_chrome();

    snap->viewport       = ui_state_viewport();
    snap->code_panel     = ui_state_code_panel();
    snap->help           = ui_state_help();
    snap->help_session   = editor_help_session_view();
    snap->variable_panel = variable_panel_view();
    snap->variable_drag.active_var = variable_panel_drag_active_var();
    snap->variable_drag.coarse     = variable_panel_drag_coarse();
    snap->profile_panel  = ui_state_profile_panel();
    snap->memory_panel   = ui_state_memory_panel();
    snap->assign_plot    = assign_plot_view();
    for (int i = 0; i < snap->assign_plot.series_count; i++)
        glr_ctrl_assign_plot_title(snap->assign_plot.series[i].source_line_idx,
                                   snap->assign_plot_titles[i],
                                   sizeof(snap->assign_plot_titles[i]));
    snap->status         = ui_state_status();
    snap->status_history = ui_state_status_history();
    /* Count of structurally unbalanced bracket commands, surfaced as a
     * persistent segment in the editor statusbar (the gutter markers,
     * pushed in glr_ctrl_push_highlights, pinpoint the lines). */
    {
        int unbalanced[MAX_HIGHLIGHTS];
        snap->unbalanced_count =
            repl_source_scope_collect_unbalanced(unbalanced, MAX_HIGHLIGHTS);
        glr_ctrl_fill_unbalanced_warning(snap, unbalanced,
                                          snap->unbalanced_count);
    }
    snap->search         = *editor_state_search();
    snap->autocomplete   = *editor_state_autocomplete();
    snap->pointer        = ui_state_pointer();
    snap->render         = glr_state_render();
    snap->replay         = replay_state_view();
    snap->tour           = glr_pointer_script_tour_view();
    snap->scenes         = repl_state_scenes();
    snap->scroll         = editor_state_scroll();
    snap->cursor_blink   = editor_state_cursor_blink();
    snap->color_picker   = color_picker_view();

    snap->editor_input   = editor_state_input();
    snap->import_export  = repl_state_import_export();
    snap->math_collision_mask = repl_export_math_collision_mask();
    {
        unsigned helper_mask = repl_export_gl_vector_helper_mask();
        int helper_lines = repl_export_gl_vector_helper_line_count(helper_mask);
        if (helper_lines < 0) helper_lines = 0;
        if (helper_lines > UI_GL_VECTOR_HELPER_MAX)
            helper_lines = UI_GL_VECTOR_HELPER_MAX;
        snap->gl_vector_helper_line_count = helper_lines;
        for (int i = 0; i < helper_lines; i++) {
            repl_export_gl_vector_helper_line(
                helper_mask, i, snap->gl_vector_helper_lines[i], MAX_LINE_LEN);
        }
    }
    flat_program         = repl_state_flat_program_view();
    predef = repl_eval_predef_view();
    glr_ctrl_fill_ui_variable_panel_vars(snap, predef);

    snap->document_cmds       = repl_state_document_cmds();
    snap->document_count      = repl_state_document_count();
    snap->edit_line           = editor_state_edit_line();

    snap->flat_program_count  = flat_program.cmd_count;
    snap->flat_program_overflow_count = flat_program.overflow_cmd_count;
    snap->can_undo            = editor_undo_can_undo();
    snap->can_redo            = editor_undo_can_redo();
    /* Cursor budget readout: resolve the attribution kind to the short
     * statusbar prefix. A plain line costing <= 1 is suppressed (every
     * ordinary top-level line costs exactly 1 - noise, not signal). */
    {
        ReplFlatCost cost = repl_flatten_cost_at_line(snap->edit_line);
        static const char *const k_cost_labels[] = {
            [REPL_FLAT_COST_NONE]  = "",
            [REPL_FLAT_COST_LINE]  = "line",
            [REPL_FLAT_COST_CALL]  = "call",
            [REPL_FLAT_COST_BLOCK] = "scope",
            [REPL_FLAT_COST_FUNC]  = "fn",
        };
        const char *label = k_cost_labels[cost.kind];
        if (cost.kind == REPL_FLAT_COST_LINE && cost.count <= 1)
            label = "";
        snap->cursor_cost_count = cost.count;
        snprintf(snap->cursor_cost_label, sizeof(snap->cursor_cost_label),
                 "%s", label);
    }
    snap->anim_time           = repl_state_variables().anim_time;

    snap->view_ortho_mode       = glr_state_presentation().ortho_mode;
    snap->view_projection_mix   = glr_ctrl_view_projection_mix();

    snap->user_scene_active_idx   = repl_active_user_scene();
    glr_ctrl_build_scene_tabs(&snap->scene_tabs);
    snprintf(snap->workspace.name, UI_WORKSPACE_NAME_MAX, "%s",
             glr_workspaces_active_name());

    snap->rename_active = editor_inline_rename_active();
    snprintf(snap->rename_text, sizeof(snap->rename_text), "%s",
             editor_inline_rename_buffer());

    snap->file_prompt_active = editor_inline_file_prompt_active();
    snprintf(snap->file_prompt_text, sizeof(snap->file_prompt_text), "%s",
             editor_inline_file_prompt_buffer());
    snprintf(snap->file_prompt_error, sizeof(snap->file_prompt_error), "%s",
             editor_inline_file_prompt_error());

    snap->app_modal_active = glr_modal_active();
    snap->app_modal_message[0] = '\0';
    if (snap->app_modal_active) {
        const char *text = glr_modal_text();
        const char *error = glr_modal_error();
        /* Presentation half of the modal contract: one arm per GlrModalKind,
         * no `default:`, so a new kind cannot render as a blank prompt bar.
         * The kind's input policy lives in glr_modal.c and its commit effects
         * in glr_actions.c - all three switches are exhaustive together. */
        switch (glr_modal_kind()) {
        case GLR_MODAL_WORKSPACE_NEW:
            snprintf(snap->app_modal_message, sizeof(snap->app_modal_message),
                     "New workspace: %s_   %s%s[Enter] create   [Esc] cancel",
                     text, error[0] ? error : "", error[0] ? "   " : "");
            break;
        case GLR_MODAL_WORKSPACE_SAVE_AS:
            snprintf(snap->app_modal_message, sizeof(snap->app_modal_message),
                     "Save workspace as: %s_   %s%s[Enter] save   [Esc] cancel",
                     text, error[0] ? error : "", error[0] ? "   " : "");
            break;
        case GLR_MODAL_WORKSPACE_OPEN_PATH:
            snprintf(snap->app_modal_message, sizeof(snap->app_modal_message),
                     "Open workspace: %s_   %s%s[Enter] open   [Esc] cancel",
                     text, error[0] ? error : "", error[0] ? "   " : "");
            break;
        case GLR_MODAL_SCENE_SAVE_AS:
            snprintf(snap->app_modal_message, sizeof(snap->app_modal_message),
                     "Save scene as: %s_   %s%s[Enter] save   [Esc] cancel",
                     text, error[0] ? error : "", error[0] ? "   " : "");
            break;
        case GLR_MODAL_CONFIRM_DELETE_SCENE:
            snprintf(snap->app_modal_message, sizeof(snap->app_modal_message),
                     "%s   %s%s[Y] delete   [Esc] cancel", text,
                     error[0] ? error : "", error[0] ? "   " : "");
            break;
        case GLR_MODAL_CONFIRM_WIP_RECOVER:
            /* Esc is "leave it alone", not "cancel": the sidecar stays on
             * disk and the watcher stops following it until the editor
             * touches it again. Nothing is deleted either way. */
            snprintf(snap->app_modal_message, sizeof(snap->app_modal_message),
                     "%s   %s%s[Y] follow it   [Esc] ignore it", text,
                     error[0] ? error : "", error[0] ? "   " : "");
            break;
        case GLR_MODAL_NONE:
        case GLR_MODAL_COUNT:
            break;   /* not open: glr_modal_active() already excluded these */
        }
    }

    snap->help_content = glr_ctrl_help_overlay_content();
    snap->editor_transformers = editor_state_transformers();
    snap->editor_highlights = editor_state_highlights();
    snap->editor_virtual_lines = editor_state_virtual_lines();

    /* Selection range materialized once for the per-row code-panel branch. */
    snap->selection_active = editor_clipboard_sel_active();
    snap->selection_lo     = snap->selection_active ? editor_clipboard_sel_lo() : -1;
    snap->selection_hi     = snap->selection_active ? editor_clipboard_sel_hi() : -1;

    /* Indent + statusbar metadata so the render path does not call back
     * into repl_source_scope_* / ui_repl_code_panel_* per row. */
    snap->active_indent_chars   = glr_ctrl_active_indent_chars();
    snap->trailing_indent_chars = repl_source_scope_cmd_indent_chars(snap->document_count);
    snap->in_begin_block        = repl_source_scope_in_begin_block();
    snap->current_begin_mode    = repl_current_begin_mode();
    snap->current_begin_block_valid =
        glr_ctrl_current_begin_block_source_extent(
            snap->edit_line,
            &snap->current_begin_block_start,
            &snap->current_begin_block_end);

    /* Resolve the dynamic reshape() projection ONCE here so the panel's
     * row-count and render passes (which straddle render3d_draw_scene)
     * read one frozen value. This is the previous frame's scene
     * projection - build_ui_snapshot runs before scene render - which is
     * exactly the consistency the panel needs; a 1-frame text lag during
     * a 2D/3D transition is invisible. */
    {
        const char *proj[REPL_EXPORT_PROJ_LINES];
        int pn = repl_export_reshape_projection_lines(proj);
        if (pn < 0) pn = 0;
        if (pn > UI_RESHAPE_PROJ_LINES) pn = UI_RESHAPE_PROJ_LINES;
        for (int i = 0; i < pn; i++)
            snprintf(snap->reshape_proj_lines[i],
                     UI_RESHAPE_PROJ_LINE_MAX, "%s", proj[i]);
        snap->reshape_proj_count = pn;
    }

    /* One-week pass: snapshot purity extensions */
    snap->editor_buffer = editor_buffer_view();
    snap->line_overrides = *editor_state_line_overrides();

    {
        TutorialRuntimeState tut = tutorial_state_view();
        snap->tutorial_fade.active = tut.active;
        snap->tutorial_fade.fade_line_idx = tut.fade_line_idx;
        snap->tutorial_fade.fade_start_t = tut.fade_start_t;
        snap->tutorial_fade.fade_duration = tut.fade_duration;

        snap->tutorial.active = tut.active;
        snap->tutorial.tutorial_idx = tut.tutorial_idx;
        snap->tutorial.visible_tag_count = repl_tutorial_visible_tag_count();
    }

    for (int i = 0; i < GLR_CONFIG_COUNT; i++) {
        snap->config_values[i] = glr_config_get((GlrConfigKey)i);
    }
    snap->example_visible_tag_count = repl_example_visible_tag_count();
    snap->user_scene_count = repl_user_scene_count();

    /* Menu-bar stepper tooltips: resolve both destinations here, where the
     * catalogs live, so the UI layer only formats what it is handed. */
    for (int i = 0; i < 2; i++) {
        int back = (i == 0);
        GlrCycleTarget target = glr_ctrl_cycle_peek(back ? -1 : 1);
        int tutorial = (target.kind == GLR_CYCLE_TARGET_TUTORIAL);
        const char *noun = NULL;
        /* Prev/next share the same key (F11 tutorials, F12 examples);
         * only the modifiers differ by direction. */
        int key = tutorial ? KM_KEY(GLR_NEXT_TUTORIAL)
                           : KM_KEY(GLR_NEXT_EXAMPLE);
        int mods = tutorial
                       ? (back ? KM_MODS(GLR_PREV_TUTORIAL)
                               : KM_MODS(GLR_NEXT_TUTORIAL))
                       : (back ? KM_MODS(GLR_PREV_EXAMPLE)
                               : KM_MODS(GLR_NEXT_EXAMPLE));
        switch (target.kind) {
        case GLR_CYCLE_TARGET_EXAMPLE:  noun = "Example"; break;
        case GLR_CYCLE_TARGET_SCENE:    noun = "Scene";   break;
        case GLR_CYCLE_TARGET_TUTORIAL: noun = "Lesson";  break;
        case GLR_CYCLE_TARGET_NONE:     break;
        }
        snap->step_target[i].name[0] = '\0';
        snap->step_target[i].noun[0] = '\0';
        snap->step_target[i].shortcut[0] = '\0';
        if (!noun || !target.name)
            continue;
        snprintf(snap->step_target[i].name,
                 sizeof snap->step_target[i].name, "%s", target.name);
        snprintf(snap->step_target[i].noun,
                 sizeof snap->step_target[i].noun, "%s", noun);
        /* Both cycles are bound to GLUT special keys (F11 / F12). */
        keymap_binding_to_string(snap->step_target[i].shortcut,
                                 (int)sizeof snap->step_target[i].shortcut,
                                 key, mods, 1);
    }

    {
        int lc = repl_export_lights_pre_camera_line_count();
        if (lc < 0) lc = 0;
        if (lc > UI_LIGHTS_DISPLAY_MAX) lc = UI_LIGHTS_DISPLAY_MAX;
        snap->lights_pre_camera_count = lc;
        for (int i = 0; i < lc; i++) {
            repl_export_lights_pre_camera_line(
                i, snap->lights_pre_camera_lines[i], MAX_LINE_LEN);
        }
    }

    {
        int lc = repl_export_lights_display_line_count();
        if (lc < 0) lc = 0;
        if (lc > UI_LIGHTS_DISPLAY_MAX) lc = UI_LIGHTS_DISPLAY_MAX;
        snap->lights_display_count = lc;
        for (int i = 0; i < lc; i++) {
            repl_export_lights_display_line(i, snap->lights_display_lines[i], MAX_LINE_LEN);
        }
    }

    {
        int ic = repl_export_init_section_line_count();
        if (ic < 0) ic = 0;
        if (ic > UI_INIT_SECTION_MAX) ic = UI_INIT_SECTION_MAX;
        snap->init_section_count = ic;
        for (int i = 0; i < ic; i++) {
            repl_export_init_section_line(i, snap->init_section_lines[i], MAX_LINE_LEN);
        }
    }

    glr_ctrl_populate_numeric_swatch(snap);
}

/* Overlay layout inputs from a frame snapshot. The replay band reuses the
 * reservation from the controller's last tick (ui_overlay_layout_last_band_h)
 * so render and hit paths agree with the eased positions. */
static UiOverlayLayoutIn glr_ctrl_overlay_layout_inputs(
        const UiRenderSnapshot *snap) {
    return ui_overlay_layout_inputs((UiOverlayLayoutInputs){
        .var_visible                = snap->variable_panel.visible,
        .var_count                  = snap->variable_panel_vars.count,
        .var_collapsed              = snap->variable_panel.collapsed,
        .profile_mode               = snap->profile_panel.mode,
        .profile_collapsed_sections = snap->profile_panel.collapsed_sections,
        .memory_mode                = snap->memory_panel.mode,
        .assign_plot_visible        = snap->assign_plot.open,
        .assign_plot_expanded       = snap->assign_plot.expanded,
        .assign_plot_series_count   = snap->assign_plot.series_count,
        .band_h                     = ui_overlay_layout_last_band_h(),
    });
}

/* Assignment-value plot: same overlay-layout slot resolution as the other
 * floating panels. Visibility is the capture side's own open flag - this
 * panel has no config key, because it is opened by right-clicking a row. */
static UiAssignPlotPanelView glr_ctrl_build_assign_plot_view(
        const UiRenderSnapshot *snap) {
    UiAssignPlotPanelView v;
    v.window_w = snap->viewport.window_w;
    v.window_h = snap->viewport.window_h;
    v.visible  = snap->assign_plot.open;
    for (int i = 0; i < MAX_ASSIGN_PLOT_SERIES; i++)
        v.titles[i] = snap->assign_plot_titles[i];
    v.pointer_x = snap->pointer.mouse_x;
    v.pointer_y = snap->pointer.mouse_y;
    v.plot     = snap->assign_plot;
    /* replay_active drives the overridden-rate chip and so is on for the whole
     * of replay; the markers themselves only exist for the frames and series
     * the progress scan could place one on. */
    v.replay_active = replay_active();
    for (int i = 0; i < MAX_ASSIGN_PLOT_SERIES; i++) {
        v.replay_frac[i] = g_assign_plot_replay_marker
                         ? g_assign_plot_replay_frac[i] : -1.0f;
        v.replay_value[i] = g_assign_plot_replay_marker
                          ? g_assign_plot_replay_value[i] : 0.0f;
    }
    UiOverlayLayoutIn in = glr_ctrl_overlay_layout_inputs(snap);
    ui_overlay_layout_panel_pos(&in, UI_OVERLAY_PANEL_ASSIGN_PLOT,
                                &v.panel_x, &v.panel_y);
    return v;
}

/* Resolve the memory panel's overlay-layout slot into the narrow view the
 * renderer consumes. All floating scene panels share one placement policy
 * (src/ui/app/overlay_layout.c); the renderer stays snapshot-free (links
 * against {support, ui/core} alone). */
static UiMemoryPanelView glr_ctrl_build_memory_panel_view(const UiRenderSnapshot *snap) {
    UiMemoryPanelView v;
    v.window_w = snap->viewport.window_w;
    v.window_h = snap->viewport.window_h;
    v.mode     = (UiMemoryPanelMode)snap->memory_panel.mode;
    UiOverlayLayoutIn in = glr_ctrl_overlay_layout_inputs(snap);
    ui_overlay_layout_panel_pos(&in, UI_OVERLAY_PANEL_MEMORY,
                                &v.panel_x, &v.panel_y);
    return v;
}

/* Compute-profile section listing: same overlay-layout slot resolution as
 * the memory panel - mirrors glr_ctrl_build_memory_panel_view. */
static UiProfilePanelView glr_ctrl_build_profile_panel_view(const UiRenderSnapshot *snap) {
    UiProfilePanelView v;
    v.window_w = snap->viewport.window_w;
    v.window_h = snap->viewport.window_h;
    v.mode     = (UiProfilePanelMode)snap->profile_panel.mode;
    v.collapsed_sections = snap->profile_panel.collapsed_sections;
    UiOverlayLayoutIn in = glr_ctrl_overlay_layout_inputs(snap);
    ui_overlay_layout_panel_pos(&in, UI_OVERLAY_PANEL_PROFILE,
                                &v.panel_x, &v.panel_y);
    return v;
}

/* Compute-profile section histograms: the final profile mode, in its own
 * overlay-layout slot. */
static UiHistogramPanelView glr_ctrl_build_histogram_panel_view(
        const UiRenderSnapshot *snap) {
    UiHistogramPanelView v;
    v.window_w = snap->viewport.window_w;
    v.window_h = snap->viewport.window_h;
    v.visible  = (snap->profile_panel.mode == PROFILE_PANEL_HISTOGRAM);
    v.hidden_series = snap->profile_panel.hidden_histogram_series;
    /* Hovering a legend entry pops that series' running statistics. The
     * pointer is snapshot state, so this needs nothing from the input path
     * beyond the passive-motion redraw the other hover treatments already
     * rely on. */
    v.pointer_x = snap->pointer.mouse_x;
    v.pointer_y = snap->pointer.mouse_y;
    UiOverlayLayoutIn in = glr_ctrl_overlay_layout_inputs(snap);
    ui_overlay_layout_panel_pos(&in, UI_OVERLAY_PANEL_HISTOGRAM,
                                &v.panel_x, &v.panel_y);
    return v;
}

/* FPS plot panel (Compute profile level 1): its own overlay-layout slot,
 * visible at every non-OFF profile level. */
static UiFpsPanelView glr_ctrl_build_fps_panel_view(const UiRenderSnapshot *snap) {
    UiFpsPanelView v;
    v.window_w = snap->viewport.window_w;
    v.window_h = snap->viewport.window_h;
    v.visible  = (snap->profile_panel.mode != PROFILE_PANEL_OFF);
    UiOverlayLayoutIn in = glr_ctrl_overlay_layout_inputs(snap);
    ui_overlay_layout_panel_pos(&in, UI_OVERLAY_PANEL_FPS,
                                &v.panel_x, &v.panel_y);
    return v;
}

/* Per-sample hook for accum motion blur (installed only when the frame's
 * blur mode is CAMERA or TIME). Resets to the frame baseline so each sample
 * is independent, then applies the sample's camera pose or time sub-step. */
static void glr_ctrl_setup_subframe(void *ud, int pass_idx, int pass_count,
                                    Render3dRenderConfig *pass_cfg) {
    GlrSubframeCtx *c = (GlrSubframeCtx *)ud;
    float f = (pass_count > 1) ? (float)pass_idx / (float)(pass_count - 1) : 0.0f;

    /* Per-sample isolation: start from the frame baseline so accumulating
     * programs (A[0]=A[0]+1, t=t+1) don't compound across samples. */
    repl_restore_predef_values(c->base_predef, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(c->base_scratch);
    repl_state_render_set(&c->base_render);

    if (c->mode == GLR_BLUR_CAMERA) {
        GlrCameraPose p = glr_camera_pose_lerp(&c->prev, &c->cur, f);
        glr_camera_load_modelview(&p);
        /* Expose the interpolated pose so grid/axes/orbit-target/lights and
         * the ortho projection (recomputed per sample by the scene) blur with
         * the camera, not just the user geometry's modelview. */
        pass_cfg->cam_rx = p.rx;
        pass_cfg->cam_ry = p.ry;
        pass_cfg->cam_dist = p.dist;
        pass_cfg->cam_tx = p.tx;
        pass_cfg->cam_ty = p.ty;
        pass_cfg->cam_tz = p.tz;
    } else if (c->mode == GLR_BLUR_TIME) {
        /* Trailing shutter [t_end - dt, t_end]: re-bake geometry at the
         * sub-step t (the modelview stays at the current camera pose). The
         * last sample (f==1) bakes at exactly t_end, so the flat program is
         * left at the true frame time. Route by the `t` bit like the frame
         * gate: when t is value-only (rebake_ok and t not in the structural mask) an
         * in-place rebake suffices per sample; otherwise re-flatten fully.
         * The per-sample baseline reset above already restored the frame's
         * predef/scratch, so both paths thread assignments from the same
         * start each sample (no compounding). */
        int t_idx = repl_state_variables().time_var_idx;
        ReplExprDepMask t_bit = (t_idx >= 0 && t_idx < MAX_PREDEF_VARS)
                              ? (ReplExprDepMask)1u << t_idx : 0;
        repl_state_time_set_transient(c->t_end - c->dt * (1.0f - f));
        repl_refresh_flat_program_for_deps(c->edit_line, t_bit);
    }
}

/* Decide this frame's blur mode and, when active, install the per-sample
 * hook + capture the baseline the hook resets to. Leaves setup_subframe_fn
 * NULL when blur is off/inapplicable so the scene takes the AA jitter path
 * (the paused + still-camera fallback, and the replay degradation). */
static void glr_ctrl_resolve_blur_subframe(Render3dRenderConfig *config) {
    config->setup_subframe_fn = NULL;
    config->setup_subframe_user_data = NULL;

    if (!RENDER3D_ACCUM_EFFECT_IS_BLUR(config->accum_effect) ||
        !config->use_accum || config->accum_passes <= 1 ||
        replay_active())
        return;

    int camera_moved = g_prev_frame_pose_valid &&
        glr_camera_pose_changed(&g_prev_frame_pose, &g_cur_frame_pose);

    /* The effect selects the blur axis; camera motion never overrides it.
     * This lets you reposition the camera under full "Blur" without losing
     * the per-object (time) blur. */
    GlrBlurMode mode = GLR_BLUR_NONE;
    if (config->accum_effect == RENDER3D_ACCUM_EFFECT_BLUR) {
        /* Full "Blur" is per-object (time) blur only - even while the camera
         * moves. Runs even when t is paused: the trailing shutter
         * [t_end - dt, t_end] still samples a frame's worth of motion, so a
         * paused scene that uses t stays motion-blurred. Scenes that don't
         * reference t have nothing to interpolate -> AA jitter fallback. */
        if (repl_state_source_uses_time())
            mode = GLR_BLUR_TIME;
    } else if (camera_moved) {
        /* "Blur Cam": camera-pose blur, and only ever camera blur. Still
         * camera -> AA jitter fallback. */
        mode = GLR_BLUR_CAMERA;
    }

    if (mode == GLR_BLUR_NONE)
        return;  /* still camera (and not time-blurring) -> AA jitter fallback */

    g_subframe_ctx.mode = mode;
    g_subframe_ctx.prev = g_prev_frame_pose;
    g_subframe_ctx.cur  = g_cur_frame_pose;
    g_subframe_ctx.dt   = GLR_FRAME_DT_SECS;
    g_subframe_ctx.edit_line = editor_state_edit_line();
    {
        ReplVariableView vv = repl_state_variables();
        g_subframe_ctx.t_end =
            (vv.time_var_idx >= 0 && vv.time_var_idx < vv.var_count)
                ? vv.vars[vv.time_var_idx].value : 0.0f;
    }
    repl_copy_predef_values(g_subframe_ctx.base_predef, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(g_subframe_ctx.base_scratch);
    g_subframe_ctx.base_render = repl_state_render();

    config->setup_subframe_fn = glr_ctrl_setup_subframe;
    config->setup_subframe_user_data = &g_subframe_ctx;
}

/* Set between glr_frame_begin() and glr_frame_ended() so an unpaired end (a
 * test driving glr_ctrl_display_frame() directly) cannot close spans that were
 * never opened - prof_end() on an unstarted section would record the whole
 * epoch as one frame. Work closes first and independently, so it gets its own
 * flag rather than sharing the frame's. */
static int g_frame_open = 0;
static int g_frame_work_open = 0;

void glr_frame_begin(void) {
    prof_frame_tick();
    memprof_frame_tick();
    /* Dial GPU timer-query capture to what the profile panel can show this
     * frame (Off/FPS -> none, Sections/Histogram -> the full tree):
     * query boundaries aren't free, so don't issue ones nobody can see.
     * Then open the frame's query slot - this must precede the first
     * prof_begin of a GPU-bracketed section (FRAME_WORK, below).
     * Both no-op while gpu_prof is disabled. */
    {
        UiProfilePanelMode prof_mode =
            (UiProfilePanelMode)ui_state_profile_panel().mode;
        /* OFF and FPS issue no timer queries - the FPS plot needs no
         * per-section GPU data, and query boundaries cost real GPU time. */
        glr_prof_set_gpu_capture_mode(
            (prof_mode == PROFILE_PANEL_OFF ||
             prof_mode == PROFILE_PANEL_FPS) ? GLR_PROF_GPU_CAPTURE_OFF
                                             : GLR_PROF_GPU_CAPTURE_ALL);
    }
    gpu_prof_frame_begin();
    /* Both spans start here. They differ only in where they end: work stops
     * before the present, the frame runs to the end of the callback. */
    prof_begin(PROF_FRAME_TOTAL);
    prof_begin(PROF_FRAME_WORK);
    g_frame_open = 1;
    g_frame_work_open = 1;
}

void glr_frame_work_end(void) {
    if (!g_frame_work_open)
        return;
    g_frame_work_open = 0;
    prof_end(PROF_FRAME_WORK);
}

static void glr_ctrl_prof_dump_tick(void);
static void glr_ctrl_prof_nesting_warn_once(void);

void glr_frame_ended(void) {
    if (g_frame_open) {
        int work_was_closed = !g_frame_work_open;
        g_frame_open = 0;
        g_frame_work_open = 0;
        prof_end(PROF_FRAME_TOTAL);
        /* Present is what the frame spent outside its work: the glFinish drain
         * and the swap, plus anything else the callback does after
         * glr_frame_work_end() that no row of its own claims. Derived rather
         * than bracketed so nothing in that stretch can go unattributed - see
         * prof_sections.h. A frame that never closed its work span leaves the
         * difference at the full total, which is the honest reading of "none of
         * this was accounted for".
         *
         * The depth-snapshot capture is the one measured stage in that stretch
         * (host-side, after the glFinish), so it comes out of the difference:
         * Present is drawn as slack, and a pixel transfer is not headroom. The
         * three parts then sum to the total exactly. Subtracted only if it
         * actually ran this frame - the capture is gated, and its last_us
         * otherwise still holds whatever an earlier frame measured. */
        double present_us = prof_section_last_us(PROF_FRAME_TOTAL);
        if (work_was_closed) {
            present_us -= prof_section_last_us(PROF_FRAME_WORK);
            if (prof_section_sampled_this_frame(PROF_DEPTH_SNAPSHOT))
                present_us -= prof_section_last_us(PROF_DEPTH_SNAPSHOT);
        }
        prof_section_record_us(PROF_PRESENT, present_us);
    }
    /* In GLR_TICK_PER_FRAME mode this is where the simulation advances, so a
     * captured sequence is t0, t0+dt, ... . Deliberately after the total is
     * closed: in the normal (timer-paced) mode the tick runs from the frame
     * timer, entirely outside this callback, and counting it here only in
     * capture mode would make the two modes' frame times incomparable. */
    if (g_tick_per_frame)
        glr_ctrl_tick();

    glr_ctrl_prof_dump_tick();
    glr_ctrl_prof_nesting_warn_once();
}

/* A nested profile row whose span is not inside its parent's makes the panel
 * lie in a way the panel cannot detect (see prof_nesting_violations): the child
 * renders indented under a parent that does not contain it, so its time reads
 * as part of a total it was never in. cpuprof counts those at the seam; this is
 * the one place in the app that looks at the count, once, on the frame that
 * first trips it. Warn rather than assert: a mis-attributed row is a diagnostic
 * defect, not a reason to take down a session someone is drawing in. */
static void glr_ctrl_prof_nesting_warn_once(void) {
    static int warned = 0;
    if (warned || prof_nesting_violations() == 0)
        return;
    warned = 1;
    {
        ProfSection orphan = prof_first_nesting_violation();
        fprintf(stderr,
                "[prof] nested section \"%s\" begins outside its parent's span "
                "- its row is indented under a total it is not part of "
                "(see prof_sections.h)\n",
                prof_section_info(orphan).label);
    }
}

/* GLR_PROF_DUMP=N: every N frames, print the profile panel's rows to stderr.
 * The panel is the same data, but reading it needs a window, a mouse and a
 * screenshot - which makes A/B measurement (feature on vs. off, two machines,
 * two drivers) impractical. This is that panel in a form a shell loop can
 * diff. Rows are the catalog's, indented by ProfSectionInfo.depth, carrying
 * the running average rather than the last frame so the numbers are stable.
 *
 * Rows under GLR_PROF_DUMP_MIN_MS (default 0.02 ms) are dropped, so a dump is
 * the handful of sections that actually cost something instead of the whole
 * catalog. Set it to 0 to print every row.
 *
 * Read the numbers as attribution, not as work: a section containing a
 * synchronous GL readback absorbs whatever pipeline drain the driver owes,
 * and on a render-ahead driver that drain is most of a refresh interval. When
 * a section's cost is suspiciously close to the vsync period, compare against
 * a run with vsync off before believing it - see docs/ADVANCED_USAGE.md. */
static void glr_ctrl_prof_dump_tick(void) {
    static int    dump_every = -1;
    static double min_us     = 0.0;
    static long   frames     = 0;
    int s;

    if (dump_every < 0) {
        const char *interval = getenv("GLR_PROF_DUMP");
        const char *floor_ms = getenv("GLR_PROF_DUMP_MIN_MS");
        dump_every = interval ? atoi(interval) : 0;
        if (dump_every < 0)
            dump_every = 0;
        min_us = floor_ms ? atof(floor_ms) * 1000.0 : 20.0;
    }
    if (dump_every <= 0 || ++frames % dump_every != 0)
        return;

    fprintf(stderr, "[prof] frame %ld (averages, ms)\n", frames);
    for (s = 0; s < PROF_SECTION_COUNT; s++) {
        ProfSectionInfo info = prof_section_info((ProfSection)s);
        double avg_us = prof_section_avg_us((ProfSection)s);
        int indent = info.depth > 0 ? info.depth * 2 : 0;
        if (avg_us < min_us)
            continue;
        fprintf(stderr, "  %*s%-*s %8.3f\n",
                indent, "", 40 - indent,
                info.label ? info.label : "(unnamed)", avg_us / 1000.0);
    }
}

void glr_ctrl_run_scripted_input_frame(void) {
    glr_pointer_script_frame();
}

void glr_ctrl_render_script_overlay(int win_w, int win_h) {
    if (glr_pointer_script_active())
        glr_pointer_script_render_overlay(win_w, win_h);
}

void glr_ctrl_render_tour_presence(int win_w, int win_h) {
    /* Ticked unconditionally, drawn conditionally: the presence clock has to
     * keep running for the length of the exit collapse after the tour itself
     * is gone, so the phase machine - not the caller - decides when there is
     * nothing left to draw. */
    GlrTourPlaybackView tour = glr_pointer_script_tour_view();
    GlrTourPresenceView presence =
        glr_tour_presence_tick(tour.active, tour.name);

    if (presence.phase == GLR_TOUR_PRESENCE_OFF)
        return;
    /* Self-timed, like the narration overlay it follows: the row stays honestly
     * stale outside a tour rather than reporting a per-frame zero. */
    prof_begin(PROF_TOUR_PRESENCE);
    tour_ui_presence_render(&presence, win_w, win_h);
    prof_end(PROF_TOUR_PRESENCE);
}

int glr_ctrl_start_tour(int tour_idx) {
    return glr_tours_start(tour_idx);
}

void glr_ctrl_start_tutorial(int tutorial_idx) {
    glr_ctrl_reset_transients();
    editor_undo_note_wholesale_replacement();
    glr_ctrl_invalidate_depth_snapshot();
    tutorial_start(tutorial_idx);
}

void glr_ctrl_display_frame(void) {
    int saved_flat_count;
    float live_predef_vals[MAX_PREDEF_VARS];
    float live_scratch_arrays[REPL_SCRATCH_ARRAY_COUNT][REPL_SCRATCH_ARRAY_LEN];
    FlatProgramView flat_program = repl_state_flat_program_view();
    int num_flat_cmds = flat_program.cmd_count;
    ReplFlatRefreshKind flat_refresh = REPL_FLAT_REFRESH_NONE;
    /* Capture replay state once before replay_prepare_frame so the
     * HUD shows the per-frame "before-prepare" view (the contract that
     * test_glr_ctrl pins). Per-field narrow accessors elsewhere in
     * the frame are reading post-prepare state, which is what they want. */
    ReplayRuntimeState frame_replay = replay_state_view();
    Render3dRenderConfig scene_config;
    UiRenderSnapshot ui_snap;

    /* No frame-boundary bookkeeping here: the staleness/FPS tick, the GPU
     * query-slot rotation and both frame spans belong to the application's
     * glr_frame_begin() / _work_end() / _ended() trio, so the frame also covers
     * the callback stages that sit outside this function. */
    glr_ctrl_refresh_window_title();
    /* The first refresh below and any time-blur refreshes inside Render 3D
     * are one displayed-frame cost. The REPL boundary retains direct timing
     * for export/debug/replay callers outside this scope. */
    repl_flat_refresh_profile_frame_begin();

    if (repl_state_normals_dirty()) {
        prof_begin(PROF_AUTONORMAL);
        /* Caller-owned cursor: read edit-line into a local int, pass
         * &local so the pipeline never reaches into editor_state_*. */
        int edit_line = editor_state_edit_line();
        repl_recompute_autonormals(glr_state_presentation().autonormal,
                                   &edit_line);
        editor_state_edit_line_set(edit_line);
        repl_state_normals_dirty_clear();
        prof_end(PROF_AUTONORMAL);
    }
    /* Bring the flat program current through the one refresh boundary. It
     * owns dirty routing, rebake fallback, and PROF_REBAKE/PROF_FLATTEN.
     * This runs inside the frame-level predef/scratch save/restore below, so
     * assignment threading is undone after the frame. */
    {
        flat_refresh = repl_refresh_flat_program(editor_state_edit_line());
        if (flat_refresh != REPL_FLAT_REFRESH_NONE) {
            flat_program = repl_state_flat_program_view();
            num_flat_cmds = flat_program.cmd_count;
        }
    }

    /* A freshly-loaded document may ask for the plot itself, via `// @plot`
     * tags on its assignment rows. Resolved here rather than at load time
     * because the compatibility check between series reads the flat program,
     * which the refresh above has just brought current; self-gated on the
     * undo generation, so this is free on every frame that did not replace
     * the document. */
    glr_assign_plot_sync_tags();

    /* Assignment-value capture, immediately after the flat program is current
     * and before anything narrows it (replay clamps the executed count, but
     * the plot wants the whole frame).
     *
     * PROF_ASSIGN_PLOT accumulates across this scan and the panel draw far
     * below, because the two phases sit at opposite ends of the frame and no
     * single begin/end could bracket both. Gated on the panel being open so a
     * closed plot leaves all three rows stale ("--") rather than reporting a
     * truthful-but-noisy zero every frame. */
    if (assign_plot_is_open()) {
        prof_accum_reset(PROF_ASSIGN_PLOT);
        prof_begin(PROF_ASSIGN_PLOT);
        prof_begin(PROF_ASSIGN_PLOT_CAPTURE);
        /* While replay is scrubbing, the plot's PC marker is only truthful over
         * a trace from the frame the PC is walking - so capture every frame
         * regardless of the rate chip. Set unconditionally, so the override
         * lifts the moment replay stops. (ASSIGN_PLOT_RATE_ONCE ignores it; see
         * assign_plot_set_live_capture.) */
        assign_plot_set_live_capture(replay_active());
        /* GLUT_ELAPSED_TIME is milliseconds since glutInit and monotonic,
         * which is all the 1 Hz gate needs - no second clock to keep. */
        assign_plot_capture((double)glutGet(GLUT_ELAPSED_TIME) * 1000.0);
        prof_end(PROF_ASSIGN_PLOT_CAPTURE);
        prof_accum_end(PROF_ASSIGN_PLOT);
    }

    /* Stencil is deliberately not host-cleared: the program's own
     * glClear(GL_STENCIL_BUFFER_BIT) is the only clear that preserves export
     * parity. When inspection is active, point out the easy-to-miss result
     * once after each program refresh (or when the view is newly enabled),
     * not every frame. A context that cannot display stencil is masked Off
     * and therefore does not produce an irrelevant warning. */
    {
        int stencil_view_on = g_stencil_readback_supported &&
            glr_state_presentation().stencil_viz != BUFFER_VIZ_STENCIL_OFF;
        int missing_clear = stencil_view_on &&
            !repl_flat_clears_stencil(flat_program.cmds, flat_program.cmd_count);
        if (missing_clear && (!g_stencil_clear_warning_active ||
                              flat_refresh != REPL_FLAT_REFRESH_NONE)) {
            repl_set_status("Stencil view: program never clears GL_STENCIL_BUFFER_BIT");
        }
        g_stencil_clear_warning_active = missing_clear;
    }

    /* Snapshot production: every per-frame list/snapshot the UI consumes
     * is built here so timing of the producer side is visible in profile.
     * The aggregate PROF_SNAPSHOT section sums the four sub-phases plus
     * the scene-config and ui-snapshot builders below. */
    prof_begin(PROF_SNAPSHOT);

    prof_begin(PROF_SNAPSHOT_TRANSFORMERS);
    glr_ctrl_push_color_transformers();
    prof_end(PROF_SNAPSHOT_TRANSFORMERS);

    prof_begin(PROF_SNAPSHOT_HIGHLIGHTS);
    glr_ctrl_push_highlights();
    prof_end(PROF_SNAPSHOT_HIGHLIGHTS);

    /* Prepare replay annotations + push the virtual-line list and
     * the per-line text-override list. Layout reads both as snapshot
     * data; the editor is agnostic to which feature pushed them.
     * prepare() fills an output struct and the controller publishes
     * via glr_publish_replay_annotations so REPL code doesn't reach
     * directly into editor_state_virtual_lines. */
    prof_begin(PROF_SNAPSHOT_VIRTUAL_LINES);
    ReplReplayAnnotationOutput replay_annotations;
    replay_annotations_prepare(source_document_view(),
                                    &replay_annotations);
    glr_publish_replay_annotations(&replay_annotations);
    glr_ctrl_push_line_overrides();
    prof_end(PROF_SNAPSHOT_VIRTUAL_LINES);

    /* Per-frame prep that sits between virtual-line refresh and
     * scene-config build: replay state-machine prepare_frame plus
     * the import/export render-state and camera string refresh.
     * Wrapped in its own subsection so the SNAPSHOT_* subsections
     * sum to PROF_SNAPSHOT exactly. */
    prof_begin(PROF_SNAPSHOT_PREP);
    saved_flat_count = num_flat_cmds;
    repl_copy_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    repl_eval_copy_scratch_arrays(live_scratch_arrays);
    if (replay_active()) {
        g_frame_replay_exec_limit = replay_prepare_frame(flat_program, saved_flat_count);
        int post_prep_src_line = replay_src_line();
        if (post_prep_src_line >= 0 &&
            post_prep_src_line != g_last_replay_follow_src_line) {
            editor_scroll_follow_cursor_set(1);
        }
        g_last_replay_follow_src_line = post_prep_src_line;
    } else {
        g_frame_replay_exec_limit = saved_flat_count;
        g_last_replay_follow_src_line = -1;
    }

    /* Assignment-plot replay markers, once the frame's exec limit is known.
     * Its own accumulate bracket rather than a silent rider on SNAPSHOT_PREP:
     * this is a flat-program scan, and per-frame work inside someone else's
     * span shows up only as unattributed remainder. */
    g_assign_plot_replay_marker = 0;
    if (replay_active() && assign_plot_is_open()) {
        prof_begin(PROF_ASSIGN_PLOT);
        g_assign_plot_replay_marker =
            assign_plot_exec_progress(g_frame_replay_exec_limit,
                                      g_assign_plot_replay_frac,
                                      g_assign_plot_replay_value);
        prof_accum_end(PROF_ASSIGN_PLOT);
    }

    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();
    prof_end(PROF_SNAPSHOT_PREP);

    prof_begin(PROF_SNAPSHOT_SCENE_CONFIG);
    glr_ctrl_build_scene_config(flat_program, &scene_config);
    prof_end(PROF_SNAPSHOT_SCENE_CONFIG);

    prof_begin(PROF_SNAPSHOT_UI);
    /* Build, let follow-scroll adjust editor scroll, then update scroll-dependent
     * fields in the snapshot selectively (instead of rebuilding the heavy snapshot):
     * the second pass is to reflect post-follow-scroll offset in the published snapshot. */
    glr_ctrl_build_ui_snapshot(&ui_snap);
    {
        int old_scroll = editor_state_scroll().scroll;
        glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(&ui_snap, NULL, NULL);
        if (editor_state_scroll().scroll != old_scroll) {
            ui_snap.scroll = editor_state_scroll();
            glr_ctrl_populate_numeric_swatch(&ui_snap);
        }
    }
    g_last_ui_snapshot = ui_snap;
    g_last_ui_snapshot_valid = 1;
    prof_end(PROF_SNAPSHOT_UI);

    prof_end(PROF_SNAPSHOT);

    /* 3D scene - render3d_draw_scene() handles optional accumulation-buffer AA */
    /* Reset subsection accumulators so timings across all AA samples sum up
     * correctly before the first (or only) render3d_draw_scene() call. */
    for (ProfSection section_idx = PROF_RENDER3D_SETUP; section_idx <= PROF_RENDER3D_LAST; section_idx++)
        prof_accum_reset(section_idx);
    prof_begin(PROF_RENDER3D);
    {
        GlrCameraState cam = glr_camera();
        g_cur_frame_pose = glr_camera_pose_from_state(&cam);
        glr_camera_load_modelview(&g_cur_frame_pose);
    }
    /* Drain any external-3D-pose record an example-mode camera apply
     * enqueued. It has to happen here and not at the apply: the record's
     * documented precondition is "after the camera modelview is loaded in
     * the display frame", and example loads run off the action path. */
    {
        float rec_rx, rec_ry, rec_tz;
        if (glr_camera_export_take_pending_3d_pose(&rec_rx, &rec_ry, &rec_tz))
            glr_ctrl_view_record_external_3d_pose(rec_rx, rec_ry, rec_tz);
    }
    /* Resolve motion-blur mode for this frame now that the current pose is
     * captured; installs the per-sample hook on scene_config when active. */
    glr_ctrl_resolve_blur_subframe(&scene_config);
    /* Confine the whole scene render - including the program's own
     * glClear, the one thing the viewport does not bound - to the scene
     * rect, so it cannot repaint the chrome cleared just below. This
     * module owns the window layout, so it owns this scissor;
     * render3d_draw_scene sets none of its own (see
     * render3d_execute_user_geometry). */
    glPushAttrib(GL_SCISSOR_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(scene_config.render3d_x, scene_config.render3d_y,
              scene_config.render3d_w, scene_config.render3d_h);
    if (render3d_draw_scene(&g_scene_renderer, &scene_config) != 0) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                    "glr_ctrl: render3d_draw_scene rejected config (errno=%d)\n",
                    errno);
            warned = 1;
        }
    }
    glPopAttrib();  /* scene-rect scissor: the 2D overlays paint the chrome */
    /* Chrome last, and reading g_presentation_rgba HERE - after the walk that
     * may have just rewritten it - is what gives the strips this frame's own
     * background while the scene config built above kept the previous one. A
     * frame that established none leaves the retained value in place, so the
     * same read covers that case with no branch.
     *
     * The strips exclude the scene rect, so clearing them late cannot touch
     * user pixels; the intervening readers - buffer visualization and the post
     * filter - read the scene rect only, and the whole-window compositor runs
     * after the 2D overlays. */
    glr_ctrl_clear_chrome(&scene_config, g_presentation_rgba);
    prof_end(PROF_RENDER3D);
    /* Motion-blur sub-frames re-bake geometry via repl_flatten_commands()
     * (glr_ctrl_setup_subframe), which rewrites the live flat-program count
     * as a side effect. The last sample bakes at the true frame time, so the
     * count is already correct in the common case; restore defensively for
     * the rare sub-step whose count differs, before HUD/snapshot readers run. */
    if (repl_state_flat_program_count() != saved_flat_count)
        repl_state_flat_program_set_count(saved_flat_count);

    /* All accumulation subframes have completed, so publish one frame-total
     * Flatten/Rebake sample (and their diagnostic children) before the
     * profile panel reads them below. */
    repl_flat_refresh_profile_frame_end();

    int frame_replaying = replay_active();
    if (frame_replaying) {
        prof_begin(PROF_REPLAY_HUD);
        /* HUD reads replay/scene/layout state from the per-frame snapshot;
         * the explicit UiReplayHudState struct it used to receive was just
         * a copy of fields already on snap.
         *
         * Override the replay slice with the pre-prepare capture taken at
         * frame top, mirroring the historic "HUD shows the per-frame
         * before-prepare view" contract pinned by test_glr_ctrl. Other
         * snap->replay readers only touch fields replay_prepare_frame
         * doesn't write (src_line_idx, active), so the override is HUD-
         * specific and ephemeral. */
        ReplayRuntimeState saved_snap_replay = ui_snap.replay;
        ui_snap.replay = frame_replay;
        replay_ui_hud_render(&ui_snap);
        ui_snap.replay = saved_snap_replay;
        prof_end(PROF_REPLAY_HUD);
    }

    /* Controlled-tour transport HUD (top of scene). Separate from the bottom
     * replay HUD above so a tour demonstrating replay shows both. No-ops for
     * env-capture scripts and when no controlled tour is running. Rendered
     * before the compositor pass; the tour narration overlay still composits
     * on top afterward in gl_repl.c, staying visually topmost (and is timed
     * there as PROF_TOUR_OVERLAY - this section is the HUD alone). */
    prof_begin(PROF_TOUR_HUD);
    tour_ui_hud_render(&ui_snap);
    prof_end(PROF_TOUR_HUD);

    /* Commit the accumulated subsection totals now that all AA samples are done. */
    for (ProfSection section_idx = PROF_RENDER3D_SETUP; section_idx <= PROF_RENDER3D_LAST; section_idx++)
        prof_accum_commit(section_idx);

    prof_begin(PROF_CODE_PANEL);
    UiCodePanelOutput cp_out = { 0, 0, 0 };
    ui_panels_render_code_panel(&ui_snap, &cp_out);
    prof_end(PROF_CODE_PANEL);

    /* Assignment-value plot. Drawn before the panel block because the
     * OpenGL-state popup in it is anchored to a click anywhere in the code
     * panel and routinely lands on top of this parked panel: a popup the
     * reader just summoned must not be buried under it. Closes the accum
     * bracket opened around the capture earlier in the frame, so
     * PROF_ASSIGN_PLOT reports what the whole feature costs rather than just
     * its draw. The `open` guard has to match the one at the capture site:
     * committing an accumulator that was never reset this frame would report
     * the previous plot's time forever. */
    if (ui_snap.assign_plot.open) {
        UiAssignPlotPanelView plot_view = glr_ctrl_build_assign_plot_view(&ui_snap);
        prof_begin(PROF_ASSIGN_PLOT);
        prof_begin(PROF_ASSIGN_PLOT_PANEL);
        ui_assign_plot_panel_render(&plot_view);
        prof_end(PROF_ASSIGN_PLOT_PANEL);
        prof_accum_end(PROF_ASSIGN_PLOT);
        prof_accum_commit(PROF_ASSIGN_PLOT);
    }

    prof_begin(PROF_UI_PANELS);
    /* Stencil legend first in the panel block: it is scene chrome parked
     * in a corner, so every popup below draws over it rather than under
     * it. Drawing here - after render3d_draw_scene returned - is also why
     * the panel is never clipped by the stencil test it reports on. */
    {
        UiBufferVizLegendView legend_view = glr_ctrl_build_buffer_viz_legend_view();
        ui_buffer_viz_legend_render(&legend_view);
    }
    /* Autocomplete popup anchors under the editor cursor. When the
     * active input row didn't render this frame (code panel hidden,
     * row scrolled offscreen) cp_out.cursor_valid is 0 and we skip
     * the popup - there's no visible cursor to anchor to. Same
     * semantic as the legacy mid-render publish, which simply didn't
     * fire on those frames. */
    if (cp_out.cursor_valid)
        ui_autocomplete_panel_render(&ui_snap, cp_out.cursor_px, cp_out.cursor_py);
    {
        UiCommandDescriptionPanelView command_description_view =
            glr_ctrl_build_command_description_panel_view();
        ui_command_description_panel_render(&command_description_view);
    }
    {
        UiGlStatePanelView gl_state_view = glr_ctrl_build_gl_state_panel_view(&ui_snap);
        ui_gl_state_panel_render(&gl_state_view);
    }
    ui_menu_bar_render_example_dropdown(&ui_snap);
    {
        UiVariablePanelView var_view = ui_app_variable_panel_view(&ui_snap);
        ui_variable_panel_render(&var_view);
    }
    {
        UiOverlayState help_overlay = {
            .visible    = ui_snap.help.visible,
            .tab_idx    = ui_snap.help_session.tab_idx,
            .scroll     = ui_snap.help_session.scroll,
            .viewport_w = ui_snap.viewport.window_w,
            .viewport_h = ui_snap.viewport.window_h,
            .content    = ui_snap.help_content,
        };
        ui_tabbed_overlay_render(&help_overlay);
    }
    prof_end(PROF_UI_PANELS);

    prof_begin(PROF_PROFILE_PANEL);
    {
        /* All three compute-profile surfaces: the FPS plot panel, the section
         * listing, and the section histograms (each renderer no-ops below its
         * own visibility level). */
        prof_begin(PROF_PROFILE_PANEL_FPS);
        UiFpsPanelView fps_view = glr_ctrl_build_fps_panel_view(&ui_snap);
        ui_fps_panel_render(&fps_view);
        prof_end(PROF_PROFILE_PANEL_FPS);

        prof_begin(PROF_PROFILE_PANEL_SECTIONS);
        UiProfilePanelView prof_view = glr_ctrl_build_profile_panel_view(&ui_snap);
        ui_profile_panel_render(&prof_view);
        prof_end(PROF_PROFILE_PANEL_SECTIONS);

        prof_begin(PROF_PROFILE_PANEL_HISTOGRAM);
        UiHistogramPanelView hist_view = glr_ctrl_build_histogram_panel_view(&ui_snap);
        ui_histogram_panel_render(&hist_view);
        prof_end(PROF_PROFILE_PANEL_HISTOGRAM);
    }
    prof_end(PROF_PROFILE_PANEL);

    prof_begin(PROF_MEMORY_PANEL);
    {
        UiMemoryPanelView mem_view = glr_ctrl_build_memory_panel_view(&ui_snap);
        ui_memory_panel_render(&mem_view);
    }
    prof_end(PROF_MEMORY_PANEL);

    /* The status-history list is anchored to the bell, but is still a popup:
     * paint it after every floating telemetry surface so its rows remain
     * readable and own their overlapping pixels. The variable panel was
     * already below this layer; this extends that established ordering to the
     * CPU/memory panels (the assignment-value plot draws earlier still, under
     * the OpenGL-state popup). */
    ui_panels_render_scene_status(&ui_snap);

    /* Compositor post-process: the whole-frame filter runs over all
     * controller-owned drawing (3D scene + controller-owned 2D UI) now that
     * this stage is done, before the host-owned splash/tour layers and the
     * buffer swap in display_func(). The scene-viewport filter
     * (PROF_RENDER3D_POST_PROCESS) is the separate scene-layer pass; Post FX
     * Scope selects at most one of these filters per frame. */
    prof_begin(PROF_COMPOSITOR);
    glr_compositor_postprocess_frame(
        glr_state_presentation().compositor_filter_mode,
        ui_snap.viewport.window_w, ui_snap.viewport.window_h);
    prof_end(PROF_COMPOSITOR);

    prof_begin(PROF_FRAME_RESTORE);
    g_frame_replay_exec_limit = -1;
    repl_restore_predef_values(live_predef_vals, MAX_PREDEF_VARS);
    repl_eval_restore_scratch_arrays(live_scratch_arrays);
    prof_end(PROF_FRAME_RESTORE);

    /* Remember this frame's camera pose so the next frame can detect camera
     * motion and interpolate prev<->cur for camera blur. */
    g_prev_frame_pose = g_cur_frame_pose;
    g_prev_frame_pose_valid = 1;

    /* Decide whether the host should capture depth for next frame's labels.
     * Derived here, once, from what the pass that just ran actually did - the
     * capture itself runs later (after glFinish) and only consumes this.
     *
     * The label-mode test is not redundant with the pass's own flag: when
     * labels are off the pass returns before touching it, so the flag would
     * keep reporting whatever the last enabled frame did. Being off is also
     * what makes the retained snapshot stale, and glr_ctrl_capture_depth_snapshot()
     * invalidates on a false gate for exactly that reason. */
    glr_ctrl_set_depth_snapshot_wanted(
        glr_state_presentation().show_vertex_labels != OVERLAY_VERTEX_LABEL_OFF &&
        edit_overlays_vertex_labels_wanted_depth());
}

void glr_ctrl_reshape(int w, int h) {
    if (h < 1) h = 1;
    ui_state_viewport_set_size(w, h);
    g_last_ui_snapshot_valid = 0;
    /* The scene rect moves with the window, so the retained depth describes a
     * region that no longer exists. The consumer's size check would catch most
     * of this, but not a resize that happens to preserve the scene rect's
     * dimensions, so drop it here as well. */
    glr_ctrl_invalidate_depth_snapshot();
}

/* Applies example tag-default overrides dynamically after a global state reset. */
static const GlrExampleTagDefault k_example_tag_defaults[] =
    GLR_EXAMPLE_TAG_DEFAULTS;

int glr_ctrl_apply_tag_defaults(int example_idx,
                                 const GlrExampleTagDefault *table,
                                 int n) {
    /* Track GlrConfigKey values already set during this call so we can
     * surface policy collisions. The shipped table has one entry; the
     * cap covers an order-of-magnitude expansion plus any synthetic
     * test policies. Iteration order is the table's declaration order;
     * later entries that target the same key overwrite earlier ones
     * (matches glr_config_set's last-write-wins semantics) and bump
     * the returned collision count. */
    enum { SEEN_CAP = 32 };
    GlrConfigKey seen[SEEN_CAP];
    int seen_count = 0;
    int collisions = 0;

    if (!table || n <= 0) return 0;

    for (int i = 0; i < n; i++) {
        const GlrExampleTagDefault *d = &table[i];
        if (example_idx < 0 || !repl_example_has_tag(example_idx, d->tag_idx))
            continue;

        int seen_idx = -1;
        for (int j = 0; j < seen_count; j++) {
            if (seen[j] == d->key) { seen_idx = j; break; }
        }
        if (seen_idx >= 0) {
            fprintf(stderr,
                    "glr_ctrl: tag-default collision on key=%d at "
                    "table index %d (later entry wins)\n",
                    (int)d->key, i);
            collisions++;
        } else if (seen_count < SEEN_CAP) {
            seen[seen_count++] = d->key;
        }

        glr_config_set(d->key, d->value);
    }
    return collisions;
}

static void glr_ctrl_reset_example_chrome(int example_idx) {
    glr_state_presentation_reset_example_defaults();
    glr_camera_mut()->auto_rotate = CFG_DEFAULT_CAMERA_ROTATE;
    variable_panel_set_visible(CFG_DEFAULT_VARIABLE_PANEL);

    glr_ctrl_apply_tag_defaults(
        example_idx, k_example_tag_defaults,
        (int)(sizeof(k_example_tag_defaults) /
              sizeof(k_example_tag_defaults[0])));

    /* glr_state_presentation_reset_example_defaults() writes the
     * light_theme field directly (it is a pure storage module and
     * cannot reach render3d_lights_apply_theme), so the app-state lights[]
     * positions/colors/eye-space flags are left on the *previous*
     * theme. Re-seed them from whatever theme the reset + tag defaults
     * settled on - same call reset_all makes. An example's own leading
     * `@cfg light_theme = X` runs after this through glr_config_set,
     * which re-applies via the same path (idempotent). */
    render3d_lights_apply_theme(glr_state_render_mut()->lights,
                             glr_state_presentation().light_theme);
}

/* Tutorial variant of the chrome reset. Examples deliberately inherit the
 * camera and (via the shared defaults) the FAR grid and the vertex
 * decorations; a tutorial should not. It opens a fresh transient scene
 * whose geometry is authored in whole units, so the lesson starts from a
 * known view: default chrome, the built-in camera orbit pulled back to
 * frame that geometry, and the CLOSE grid as its stage. Eases rather than
 * snaps so the switch reads as a move, not a cut. Runs before the
 * tutorial's own leading `@cfg`, which still wins over all of it. */
static void glr_ctrl_reset_tutorial_chrome(void) {
    GlrPresentationState *p = glr_state_presentation_mut();

    glr_ctrl_reset_example_chrome(-1);
    p->grid_extent_idx      = CFG_DEFAULT_TUTORIAL_GRID_EXTENT_IDX;
    p->show_vertex_outlines = CFG_DEFAULT_TUTORIAL_VERTEX_OUTLINES;
    p->show_vertex_points   = CFG_DEFAULT_TUTORIAL_VERTEX_POINTS;
    /* Clear first: ease_to_default prefers a scene camera default (set by
     * the previous example's `// camera` header) over the built-in pose. */
    glr_camera_clear_scene_default();
    glr_camera_ease_to_default();
    /* ...then re-aim the same ease at the tutorial distance. glr_camera has
     * no "read the built-in pose" accessor, so take it off the ease we just
     * issued: destination() is that pose, and re-easing to it with only
     * dist replaced keeps orbit/target authoritative in one place. */
    {
        GlrCameraState d = glr_camera_destination();
        glr_camera_ease_to(d.rx, d.ry, CFG_DEFAULT_TUTORIAL_CAMERA_DIST,
                           d.tx, d.ty, d.tz);
    }
}

/* Adapter for repl_executor_install_camera_distance_source. The
 * source is unconditionally installed; only the point-size fallback
 * (taken when the runtime GL lacks glPointParameterfv) consumes it,
 * so this is a small zero-cost shim when point parameters are
 * supported. */
static float glr_ctrl_camera_distance(void) {
    return glr_camera().dist;
}

/* Adapter for the export reshape-projection bridge. Translates the
 * scene's currently-applied projection (cached by render3d_apply_projection)
 * into the C lines the exported reshape() / live code panel emit between
 * glLoadIdentity() and glMatrixMode(GL_MODELVIEW). Ortho uses the
 * aspect-independent half-height and recomputes the aspect at runtime
 * from w/h so the exported program stays resolution-independent. */
static void glr_ctrl_export_reshape_projection(ReplExportProjectionBlock *blk) {
    Render3dProjectionDesc p;

    render3d_get_active_projection(&g_scene_renderer, &p);
    blk->count = 0;
    if (p.projection == PROJ_ORTHO) {
        snprintf(blk->lines[blk->count++], REPL_EXPORT_PROJ_LINE_MAX,
                 "  double _t = %.6g, _r = _t * (double)w / (double)h;",
                 p.ortho_top);
        snprintf(blk->lines[blk->count++], REPL_EXPORT_PROJ_LINE_MAX,
                 "  glOrtho(-_r, _r, -_t, _t, %.6g, %.6g);",
                 p.ortho_near, p.ortho_far);
    } else {
        snprintf(blk->lines[blk->count++], REPL_EXPORT_PROJ_LINE_MAX,
                 "  gluPerspective(%.6g, (double)w/(double)h, %.6g, %.6g);",
                 p.fovy_deg, p.near_z, p.far_z);
    }
}

static const ReplExportProjectionBridge g_export_projection_bridge_impl = {
    glr_ctrl_export_reshape_projection
};

/* Adapter for the export light bridge. Copies the app-owned theme-seeded
 * light data (GlrRenderState.lights) into the exporter's neutral float
 * struct so src/repl/export.c can emit the glLightfv init/display blocks
 * without including render3d/app headers. Enable state is intentionally not
 * carried - the export bootstrap disables every slot and the program's own
 * glEnable(GL_LIGHTn) re-enables in display(). */
static void glr_ctrl_export_fill_light(int slot, ReplExportLightInfo *out) {
    if (slot < 0 || slot >= MAX_LIGHTS) return;  /* out is pre-zeroed by caller */
    GlrRenderState render = glr_state_render();
    const Render3dLight *l = &render.lights[slot];
    memcpy(out->pos, l->pos, sizeof(out->pos));
    memcpy(out->diffuse, l->diffuse, sizeof(out->diffuse));
    memcpy(out->ambient, l->ambient, sizeof(out->ambient));
    memcpy(out->specular, l->specular, sizeof(out->specular));
    out->pos_is_eye_space = l->pos_is_eye_space;
}

static const ReplExportLightBridge g_export_light_bridge_impl = {
    glr_ctrl_export_fill_light
};

/* Editor-input cleanup that the REPL loaders used to do inline now
 * routes through callback sinks. The two helpers below are the
 * full-app implementations the controller installs at startup; the
 * demo leaves both unset. */
static void glr_ctrl_host_input_reset(void) {
    editor_insert_mode_set(0);
    editor_input_clear();
    EditorInputState *inp = editor_state_input_mut();
    inp->pending_newline[0] = '\0';
    inp->pending_newline_len = 0;
}

static void glr_ctrl_host_insert_mode_off(void) {
    editor_insert_mode_set(0);
}

/* Route the residual editor scroll/follow writes through host-effect
 * sinks. */
static void glr_ctrl_scroll_to_line(int target) {
    editor_scroll_set(target);
    editor_scroll_follow_cursor_set(0);
}

const UiOverlayContent *glr_ctrl_help_overlay_content(void) {
    /* Keep headroom beyond today's five tabs so extending the REPL-owned
     * help content cannot silently hide the trailing tab again. */
    enum { GLR_HELP_OVERLAY_MAX_TABS = 8 };
    static UiOverlayTab tabs[GLR_HELP_OVERLAY_MAX_TABS];
    static UiOverlayContent content;
    static int cached = 0;

    if (cached)
        return &content;

    const ReplHelpContent *help = repl_help_text_build();
    if (!help)
        return NULL;

    int count = help->tab_count;
    if (count < 0)
        count = 0;
    if (count > GLR_HELP_OVERLAY_MAX_TABS)
        count = GLR_HELP_OVERLAY_MAX_TABS;

    for (int i = 0; i < count; i++) {
        tabs[i].label = help->tabs[i].label;
        tabs[i].lines = help->tabs[i].lines;
    }

    content.title = help->title;
    content.tabs = tabs;
    content.tab_count = count;
    cached = 1;
    return &content;
}

void glr_publish_replay_annotations(const ReplReplayAnnotationOutput *out) {
    editor_state_virtual_lines_clear();
    if (!out) return;
    for (int i = 0; i < out->count; i++) {
        const ReplReplayAnnotation *row = &out->items[i];
        UiVirtualLineStyle style;
        switch (row->kind) {
        case REPL_REPLAY_ANNOTATION_KIND_SUBST:
            style = VIRTUAL_STYLE_REPLAY_SUBST;
            break;
        case REPL_REPLAY_ANNOTATION_KIND_EVAL:
            style = VIRTUAL_STYLE_REPLAY_EVAL;
            break;
        default:
            continue;
        }
        editor_state_virtual_lines_append(row->after_line_idx, style,
                                          row->text,
                                          row->aux[0] ? row->aux : NULL);
    }
}

int glr_ctrl_gl_state_live_input_is_blank_at_line(int source_line_idx) {
    const char *text;

    if (source_line_idx < 0 ||
        source_line_idx != editor_state_edit_line())
        return 0;
    text = editor_input_text();
    while (text && *text && isspace((unsigned char)*text))
        text++;
    return !text || *text == '\0';
}

static int glr_ctrl_gl_state_anchor_is_valid(int source_line_idx) {
    const GLCmd *anchor_cmd;

    if (source_line_idx < 0)
        return 0;

    /* In overwrite mode the input row replaces the document row visually,
     * so its live contents decide whether the anchor is still blank. Insert
     * mode shows both rows: a blank live insertion slot is valid, while a
     * committed empty command at the same index remains independently valid. */
    if (!editor_insert_mode() &&
        source_line_idx == editor_state_edit_line())
        return glr_ctrl_gl_state_live_input_is_blank_at_line(source_line_idx);
    if (glr_ctrl_gl_state_live_input_is_blank_at_line(source_line_idx))
        return 1;

    anchor_cmd = repl_state_document_cmd_at(source_line_idx);
    return anchor_cmd && anchor_cmd->type == CMD_EMPTY;
}

/* Build the right-click OpenGL-state popup's per-frame view. Validates that
 * the anchor is still a visually blank committed or live editor row (closing
 * the popup otherwise), folds the current flat program up to that boundary
 * into g_gl_state_report, and flips the stored GLUT click y into the
 * renderer's y-up coords. */
UiGlStatePanelView glr_ctrl_build_gl_state_panel_view(const UiRenderSnapshot *snap) {
    UiGlStatePanelView view;
    UiGlStateInspectorState inspector = ui_state_gl_state_inspector();
    UiViewportState vp = ui_state_viewport();

    memset(&view, 0, sizeof(view));
    view.window_w = vp.window_w;
    view.window_h = vp.window_h;
    if (!inspector.visible)
        return view;
    if (!glr_ctrl_gl_state_anchor_is_valid(inspector.source_line_idx)) {
        ui_state_gl_state_inspector_close();
        return view;
    }

    repl_gl_state_report_at_line(repl_state_flat_program_view(),
                                 inspector.source_line_idx,
                                 &g_gl_state_report);

    /* A pinned comparison basis is a second anchor row and lives under the
     * same validity rule as the first - editing its line out of blankness
     * drops the comparison rather than the popup. */
    if (inspector.basis_line_idx >= 0 &&
        !glr_ctrl_gl_state_anchor_is_valid(inspector.basis_line_idx)) {
        ui_state_gl_state_inspector_set_basis(-1);
        inspector = ui_state_gl_state_inspector();
    }
    /* Re-folded every frame beside the main one rather than captured when the
     * user pinned it: the fold is a function of the flat program at the
     * current `t`, so a snapshot would describe a frame that has since gone.
     * The basis fold itself belongs to the inspector - it keeps rows a
     * displayed report filters out - so the report is not built here. */
    if (inspector.basis_line_idx >= 0)
        repl_gl_state_report_rebase(repl_state_flat_program_view(),
                                    inspector.basis_line_idx,
                                    &g_gl_state_report);

    /* Resolve each program-authored row's code-panel gutter label, so the
     * popup's source column cites the number the user can actually see in the
     * margin. They differ whenever code focus is off: the gutter counts the
     * derived-C chrome rows, the report carries document indices. Both the
     * render and the router reach this through the controller's cached
     * snapshot, so the solved column width agrees frame to frame. */
    {
        int lines[REPL_GL_STATE_REPORT_MAX_ROWS];
        int i;
        for (i = 0; i < g_gl_state_report.count; i++)
            lines[i] = g_gl_state_report.rows[i].source.source_line_idx;
        /* The display path hands in the very snapshot the code panel just
         * rendered from, so the row cache hits and the labels come from the
         * rows on screen. Callers with no snapshot in hand (the router's
         * hit-test entries) fall back to the controller's cached one. */
        ui_repl_code_panel_gutter_labels_for_lines(
            snap ? snap : glr_ctrl_drag_hit_test_snapshot(), lines,
            g_gl_state_gutter_labels, g_gl_state_report.count);
    }
    view.visible = 1;
    view.anchor_px = inspector.anchor_px;
    view.anchor_py = vp.window_h - inspector.anchor_py;
    view.scroll_rows = inspector.scroll_rows;
    view.details_expanded = inspector.details_expanded;
    view.setup_expanded = inspector.setup_expanded;
    view.report = &g_gl_state_report;
    view.source_gutter_labels = g_gl_state_gutter_labels;
    /* The basis column header cites the pinned line the same way the source
     * column cites a row's, so it needs the same gutter-label resolution. */
    view.basis_gutter_label = -1;
    if (inspector.basis_line_idx >= 0) {
        int basis_line = inspector.basis_line_idx;
        ui_repl_code_panel_gutter_labels_for_lines(
            snap ? snap : glr_ctrl_drag_hit_test_snapshot(), &basis_line,
            &view.basis_gutter_label, 1);
    }
    return view;
}

void glr_ctrl_buffer_viz_legend_select_rows(
        const BufferVizStencilHistogram *hist, int stencil_mode,
        UiBufferVizLegendView *view) {
    unsigned char taken[256];
    unsigned int listed_px = 0;
    int slot;

    if (!view)
        return;
    view->row_count = 0;
    view->hidden_rows = 0;
    view->hidden_px = 0;
    view->zero_px = 0;
    view->total_px = 0;
    if (!hist || !hist->valid)
        return;

    view->total_px = (unsigned int)(hist->total_px > 0 ? hist->total_px : 0);
    view->zero_px = hist->counts[0];

    /* Top-N by pixel count, ties broken by ascending value (the scan
     * keeps the first - lowest - value at a given count). A linear
     * re-scan per slot rather than a sort: 8 x 255 comparisons per frame
     * is cheaper than sorting 256 bins, and it needs no scratch array of
     * pairs. */
    memset(taken, 0, sizeof taken);
    for (slot = 0; slot < UI_BUFFER_VIZ_LEGEND_MAX_ROWS; slot++) {
        UiBufferVizLegendRow *row;
        int best = -1;
        int v;

        for (v = 1; v < 256; v++) {
            if (taken[v] || hist->counts[v] == 0)
                continue;
            if (best < 0 || hist->counts[v] > hist->counts[best])
                best = v;
        }
        if (best < 0)
            break;
        taken[best] = 1;
        row = &view->rows[view->row_count++];
        row->value = best;
        row->count = hist->counts[best];
        buffer_viz_stencil_swatch_rgb(best, (BufferVizStencilMode)stencil_mode,
                                      row->rgb);
        listed_px += hist->counts[best];
    }

    view->hidden_rows = hist->distinct - view->row_count;
    if (view->hidden_rows < 0)
        view->hidden_rows = 0;
    if ((unsigned int)hist->nonzero_px > listed_px)
        view->hidden_px = (unsigned int)hist->nonzero_px - listed_px;
}

/* Build the stencil legend's per-frame view. The subsystem publishes raw
 * per-value counts from the final accumulation pass; which of them are
 * worth a row is a presentation decision, so it is made here rather than
 * in the renderer. Invisible whenever the viz is Off (including masked
 * Off by the capability probe) or no capture has been scanned yet. */
UiBufferVizLegendView glr_ctrl_build_buffer_viz_legend_view(void) {
    UiBufferVizLegendView view;
    UiViewportState vp = ui_state_viewport();
    const BufferVizStencilHistogram *hist;
    int mode = g_stencil_readback_supported
        ? glr_state_presentation().stencil_viz
        : BUFFER_VIZ_STENCIL_OFF;

    memset(&view, 0, sizeof(view));
    view.window_w = vp.window_w;
    view.window_h = vp.window_h;
    ui_layout_scene_rect(&view.scene_x, &view.scene_y,
                         &view.scene_w, &view.scene_h);
    if (mode <= BUFFER_VIZ_STENCIL_OFF || mode >= BUFFER_VIZ_STENCIL_COUNT)
        return view;

    hist = buffer_viz_stencil_histogram();
    if (!hist->valid || hist->total_px <= 0)
        return view;

    snprintf(g_buffer_viz_legend_title, sizeof g_buffer_viz_legend_title,
             "Stencil: %s", glr_config_state_name(GLR_CONFIG_STENCIL_VIZ, mode));
    view.title = g_buffer_viz_legend_title;
    glr_ctrl_buffer_viz_legend_select_rows(hist, mode, &view);
    view.visible = 1;
    return view;
}

static UiCommandDescriptionPanelView
glr_ctrl_build_command_description_panel_view(void) {
    UiCommandDescriptionPanelView view;
    UiCommandDescriptionState state = ui_state_command_description();
    UiViewportState vp = ui_state_viewport();
    ReplCommandDescription description;
    const GLCmd *cmd;

    memset(&view, 0, sizeof(view));
    view.window_w = vp.window_w;
    view.window_h = vp.window_h;
    if (!state.visible)
        return view;

    cmd = repl_state_document_cmd_at(state.source_line_idx);
    if (!cmd || !repl_command_description_lookup(cmd, &description)) {
        ui_state_command_description_close();
        return view;
    }

    if (cmd->type == CMD_ENABLE || cmd->type == CMD_DISABLE) {
        snprintf(g_command_description_title,
                 sizeof(g_command_description_title), "%s(%s)",
                 cmd->type == CMD_ENABLE ? "glEnable" : "glDisable",
                 description.title);
        view.title = g_command_description_title;
    } else {
        /* Prefer the parameter signature (e.g. "glVertex3f(x, y, z)") over
         * the bare command name, falling back to the catalog title for any
         * command without a completion-table entry. */
        const char *signature = repl_func_signature_for_name(description.title);
        view.title = signature ? signature : description.title;
    }
    view.visible = 1;
    view.anchor_px = state.anchor_px;
    view.anchor_py = vp.window_h - state.anchor_py;
    view.body = description.body;
    return view;
}

static void glr_ctrl_host_editor_cursor_park(int line, int insert_mode) {
    editor_state_edit_line_set(line);
    editor_insert_mode_set(insert_mode);
}

/* Put the cursor IN a committed row: park it, load the row's text (even
 * when the tutorial has that row locked - see
 * editor_load_line_to_input_readonly), drop insert mode so no blank line
 * opens above it, and follow-scroll it into view. glr_ctrl_set_edit_line is
 * deliberately not reused: that is the capture/user-navigation entry point,
 * and it refuses locked rows, which is exactly the case a tutorial park
 * hits whenever the target step committed its row without a comment above
 * it (the row itself is then the step's locked line). */
static void glr_ctrl_host_focus_line(int line) {
    int count = repl_state_document_count();
    if (line < 0 || count <= 0) return;
    if (line >= count) line = count - 1;
    editor_state_edit_line_set(line);
    editor_load_line_to_input_readonly(line);
    editor_insert_mode_set(0);
    editor_scroll_follow_cursor_set(1);
}

static void glr_ctrl_host_completion_clear(void) {
    editor_completion_clear();
}

static void glr_ctrl_host_completion_update(void) {
    editor_completion_update();
}

static const char *glr_ctrl_host_editor_input_get(void) {
    return editor_state_input().input;
}

static void glr_ctrl_host_set_time_playing(int playing) {
    repl_state_time_set_playing(playing);
}

/* The host-effect bridge routing core pipeline events to the UI and editor state. */
static const ReplHostEffects g_glr_host_effects = {
    .status                      = ui_state_status_set,
    .status_error                = ui_state_status_set_error,
    .example_presentation_reset  = glr_ctrl_reset_example_chrome,
    .tutorial_presentation_reset = glr_ctrl_reset_tutorial_chrome,
    .input_reset                 = glr_ctrl_host_input_reset,
    .insert_mode_off             = glr_ctrl_host_insert_mode_off,
    .scroll_to_line              = glr_ctrl_scroll_to_line,
    .tutorial_teardown           = tutorial_teardown,
    .edit_line_get               = editor_state_edit_line,
    .edit_line_set               = editor_state_edit_line_set,
    .host_cursor_park            = glr_ctrl_host_editor_cursor_park,
    .host_focus_line             = glr_ctrl_host_focus_line,
    .completion_clear            = glr_ctrl_host_completion_clear,
    .completion_update           = glr_ctrl_host_completion_update,
    .host_input_get              = glr_ctrl_host_editor_input_get,
    .set_time_playing            = glr_ctrl_host_set_time_playing,
};

/* Seed both overlay fade machines to the current presentation theme at steady full opacity. */
static void glr_ctrl_seed_overlay_xn(void) {
    GlrPresentationState p = glr_state_presentation();
    /* Bind each overlay's curve plugin once; from here the controller only
     * feeds the machines dt and reads opacity back - the grid/axes module
     * owns the durations + per-theme speed. */
    render3d_xn_init(&g_grid_xn, p.grid_theme, &render3d_grid_reveal);
    render3d_xn_init(&g_axes_xn, p.axes_theme, &render3d_axes_reveal);
}

/* Fixed frame timestep (~60 Hz). Every per-frame advance (time var, replay
 * fade, camera momentum, grid/axes fade, view transition) uses the fixed
 * GLR_FRAME_DT_SECS, so the sim is deterministic and headless capture is
 * unaffected. The GLUT host owns matching wall-clock pacing via
 * glr_frame_pacer. GLR_CURSOR_BLINK_TICKS is the cursor blink half-period
 * counted in those ticks (~0.5 s). */
#define GLR_CURSOR_BLINK_TICKS 30

/* Per-frame diff + advance, called from glr_ctrl_tick (the animation
 * timer) with the same fixed GLR_FRAME_DT_SECS dt as repl_advance_time /
 * replay_tick_fade_batches / glr_camera_tick. NOT the display path:
 * display fires on reshape/expose without the timer, which would
 * couple fade speed to redraw rate. Rule 6 off-source short-circuit:
 * when the overlay is currently the off index, render3d_xn_show skips the
 * pointless OUT of an already-invisible overlay (controller owns the
 * off-index policy; the machine stays theme-index-agnostic). */
static void glr_ctrl_tick_overlay_xn(void) {
    GlrPresentationState p = glr_state_presentation();

    if (p.grid_theme != g_grid_xn.current &&
        g_grid_xn.current == GRID_THEME_OFF)
        render3d_xn_show(&g_grid_xn, p.grid_theme);
    else
        render3d_xn_set(&g_grid_xn, p.grid_theme);
    /* Pure clock: just advance by dt. The grid's bound reveal curve owns the
     * durations + per-theme speed and decides when the fade is done. */
    render3d_xn_tick(&g_grid_xn, GLR_FRAME_DT_SECS);

    if (p.axes_theme != g_axes_xn.current &&
        g_axes_xn.current == AXES_THEME_OFF)
        render3d_xn_show(&g_axes_xn, p.axes_theme);
    else
        render3d_xn_set(&g_axes_xn, p.axes_theme);
    render3d_xn_tick(&g_axes_xn, GLR_FRAME_DT_SECS);
}

/* Look up the label of the config row currently bound to F<fn>.
 * Used by the help overlay's Config Controls groups through the
 * controller-installed ReplHelpFkeyProvider, so src/repl/help_text.c can
 * obtain labels without reaching into src/app/glr_config.h. */
static const char *glr_ctrl_help_fkey_label(int fn) {
    int cfg_count = 0;
    int key_code = GLUT_KEY_F1 + fn - 1;
    const GlrConfigItem *items = glr_config_items(&cfg_count);
    for (int i = 0; i < cfg_count; i++) {
        const GlrConfigItem *item = &items[i];
        if (item->section_header || item->key == GLR_CONFIG_NONE)
            continue;
        if (item->is_special && item->key_code == key_code)
            return glr_config_item_display_label(item);
    }
    return NULL;
}

static const ReplHelpFkeyProvider g_glr_help_fkey_provider = {
    .fkey_label = glr_ctrl_help_fkey_label,
};

/* REPL-backed value source for the variable-panel drag handlers. The peer
 * reads the declared variable's name + value through this bridge instead of
 * touching the eval table directly, which keeps src/subsystems/variable_panel
 * linkable from the standalone variable_panel_demo (it installs its own). */
static int glr_ctrl_var_read_row(int row, char *name_out, int name_cap, float *value_out) {
    ReplPredefView predef = repl_eval_predef_view();
    if (row < 0 || row >= predef.count) return 0;
    snprintf(name_out, (size_t)name_cap, "%s", predef.vars[row].name);
    *value_out = predef.vars[row].value;
    return 1;
}
static const VariablePanelValueSource g_glr_var_value_source = {
    glr_ctrl_var_read_row,
};

/* Idempotent app-service installer required for any REPL loading/export path
 * (including CLI). */
static void glr_ctrl_install_app_services(void) {
    /* Install the host-effect bridge (status sink, example presentation, editor effects, tutorial). */
    repl_install_host_effects(&g_glr_host_effects);
    /* Variable-panel drag value source: name + value reads from the REPL eval table. */
    variable_panel_install_value_source(&g_glr_var_value_source);
    /* Color-picker host: document read/write + screen geometry. */
    glr_color_picker_install_host();
    /* Assignment-plot host: flat-program execution trace + document row kind. */
    glr_assign_plot_install_host();
    /* OS-clipboard bridge: mirrors editor copy/cut out to the system
     * clipboard and adopts it on paste when another app has taken it over. */
    glr_clipboard_install();
    /* Install the export-config bridge for @cfg headers and per-scene config snapshotting. */
    glr_actions_install_export_cfg_bridge();
    /* Install the export-camera bridge for // camera blocks serialization. */
    glr_camera_export_install_bridge();
    /* Install the executor's camera-distance source to support dynamic point-attenuation scaling fallbacks. */
    repl_executor_install_camera_distance_source(glr_ctrl_camera_distance);
    /* Reshape-projection bridge: queries the active 2D/3D projection for export and layout calculations. */
    repl_export_install_projection_bridge(&g_export_projection_bridge_impl);
    /* Light bridge: feeds the app-owned theme-seeded light data to the exporter's glLightfv blocks. */
    repl_export_install_light_bridge(&g_export_light_bridge_impl);
    /* Help-overlay F-key label provider so src/repl/help_text.c can
     * place F2..F10 labels in Config-menu order without including
     * app/glr_config.h. */
    repl_help_text_install_fkey_provider(&g_glr_help_fkey_provider);
    /* Seed the grid/axes overlay transition machines. */
    glr_ctrl_seed_overlay_xn();
}

/* Full-world reset entry point. Clears REPL, editor, UI, and all peer subsystems. */
void glr_ctrl_reset_all(void) {
    editor_undo_note_wholesale_replacement();
    /* The retained depth describes the outgoing scene. Dropping it here (rather
     * than trusting the next capture to overwrite it) is what stops the first
     * pass after a reset culling labels against the previous document. */
    glr_ctrl_depth_snapshot_reset();
    repl_state_reset_program();
    /* Reset presentation, rendering, and camera defaults. */
    glr_state_presentation_reset_defaults();
    glr_state_render_reset_defaults();
    /* Seed the app-state light slots with the active theme's
     * positions / colors. The render3d module owns the theme presets;
     * the controller wires them into GlrRenderState so the merge in
     * glr_ctrl_build_scene_config (dimensional data + REPL enable mask)
     * and the export light bridge see a coherent set of lights.
     * glr_state defaults only seed .id; without this call positions /
     * colors stay zero. */
    render3d_lights_apply_theme(glr_state_render_mut()->lights,
                             glr_state_presentation().light_theme);
    glr_camera_reset_default();
    glr_ctrl_view_reset();
    g_last_ui_snapshot_valid = 0;
    g_last_replay_follow_src_line = -1;
    g_frame_replay_exec_limit = -1;
    editor_state_reset();
    ui_state_reset();
    ui_overlay_layout_reset();
    variable_panel_state_reset();
    replay_state_reset();
    color_picker_state_reset();
    /* Hard reset rather than an outro: a wholesale world reset is not a tour
     * the user chose to leave, so there is no exit to animate. */
    glr_tour_presence_reset();
    /* tutorial_teardown (rather than tutorial_state_reset) so an active
     * tutorial's cfg baseline gets restored before any presentation cfg
     * that follows this reset locks the tutorial-mutated state in. */
    tutorial_teardown();
    editor_help_session_reset();
    glr_ctrl_reset_transients();
    /* Inline modals are transient editor state too: a post-reset
     * world should not still be hosting a half-typed rename or
     * file-load prompt. */
    editor_inline_rename_cancel();
    editor_inline_file_prompt_cancel();
    glr_modal_cancel();
    /* Register the default editor completion provider. Editor input
     * dispatch calls editor_completion_* without knowing about
     * glr_completion; the registration here installs the
     * REPL-aware backing. */
    glr_completion_register_provider();
    glr_ctrl_install_app_services();
    /* Refresh derived export/camera text caches AFTER peer resets
     * so the cached strings reflect post-reset state, not whatever
     * was on the peers before this call. These read app-side cfg /
     * camera state, so they cannot live on the REPL-side reset
     * (would pull glr_config / glr_camera into the demo link set
     * AND would pre-fire before peers were reset).
     * The frame loop refreshes them every frame in build_ui_snapshot
     * + display, so tests that go through glr_ctrl_reset_all see
     * coherent caches without waiting for a frame. */
    repl_state_refresh_workspace_header_lines();
    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();
    glr_ctrl_sync_ui_chrome();
}

void glr_ctrl_after_tour_restore(void) {
    /* glr_tour_snapshot_restore() puts presentation/camera/menu/peer state
     * back; here we re-derive the app-side caches that hang off them. NOT
     * glr_ctrl_reset_transients() - that would clear the restored camera scene
     * default, close the restored menu, and stop the restored color picker. */
    repl_state_refresh_workspace_header_lines();
    repl_refresh_render_state_strings();
    repl_refresh_camera_lines();
    glr_ctrl_sync_ui_chrome();
    /* The camera control mode is a function of the restored view-transition
     * phase + presentation ortho flag; re-derive it so a mid-transition
     * baseline resumes with the right 2D/3D drag behavior. */
    glr_ctrl_sync_camera_control_mode();
}

void glr_ctrl_sync_ui_chrome(void) {
    GlrPresentationState p = glr_state_presentation();
    UiCodePanelRuntimeState *cp = ui_state_code_panel_mut();
    cp->layout_mode         = p.code_panel_layout;
    cp->wrap_at_comma       = p.wrap_at_comma;
    cp->syntax_highlight    = p.syntax_highlight;
    cp->code_focus          = p.code_focus;
    cp->paren_match         = p.paren_match;
    cp->paren_scope         = p.paren_scope;
}

void glr_ctrl_apply_code_panel_follow_scroll(
    const UiReplCodePanelLayout *layout) {
    int max_scroll;
    EditorScrollState *scroll;

    if (!layout)
        return;

    max_scroll = layout->total_lines - layout->visible_lines;
    if (max_scroll < 0)
        max_scroll = 0;

    scroll = editor_state_scroll_mut();
    if (scroll->scroll > max_scroll)
        scroll->scroll = max_scroll;
    if (scroll->scroll < 0)
        scroll->scroll = 0;

    if (scroll->scroll_follow_cursor) {
        if (layout->follow_doc_line < scroll->scroll)
            scroll->scroll = layout->follow_doc_line;
        if (layout->follow_doc_line >= scroll->scroll + layout->visible_lines)
            scroll->scroll = layout->follow_doc_line - layout->visible_lines + 1;
        if (scroll->scroll > max_scroll)
            scroll->scroll = max_scroll;
        if (scroll->scroll < 0)
            scroll->scroll = 0;
        scroll->scroll_follow_cursor = 0;
    }
}

static int glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(
    const UiRenderSnapshot *snap,
    int *out_follow_doc_line,
    int *out_visible_lines) {
    UiReplCodePanelLayout layout;
    int cp_x;
    int cp_y;
    int cp_w;
    int cp_h;
    int text_x;
    int scroll;

    if (!snap)
        return 0;

    text_x = ui_repl_code_panel_compute_text_x(snap);

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    (void)cp_x;
    (void)cp_y;

    ui_repl_code_panel_build_layout(snap, &layout, cp_w, text_x, cp_h);
    glr_ctrl_apply_code_panel_follow_scroll(&layout);

    if (out_follow_doc_line)
        *out_follow_doc_line = layout.follow_doc_line;
    if (out_visible_lines)
        *out_visible_lines = layout.visible_lines;

    scroll = editor_scroll();
    return layout.follow_doc_line >= scroll &&
           layout.follow_doc_line < scroll + layout.visible_lines;
}

int glr_ctrl_code_panel_apply_scroll_follow_for_test(
    const UiRenderSnapshot *snap,
    int *out_follow_doc_line,
    int *out_visible_lines) {
    return glr_ctrl_apply_code_panel_follow_scroll_for_snapshot(
        snap, out_follow_doc_line, out_visible_lines);
}

void glr_ctrl_init_gl(void) {
    tutorial_state_init_explicit();
    glr_ctrl_reset_all();
    repl_ensure_init_bootstrap_ready();
    render3d_init_gl();
    /* Buffer-viz caches are the app's to reset now that render3d owns no
     * buffer inspection: a fresh GL context must not reuse a stale
     * texture name or a stale EMA range. */
    buffer_viz_reset();
    render3d_state_init(&g_scene_renderer);
    repl_executor_init_resources();
    hidden_lines_init_resources();

    /* Runtime point-parameter capability (replaces the old
     * compile-time NO_POINT_PARAMETER macro). glPointParameterfv is
     * core GL 1.4 but on older Linux stacks the context can advertise
     * support while the unsuffixed C symbol is not safely callable. The
     * GL context is already current here (glr_ctrl_init_gl runs
     * post-glutInit/window), so detect BOTH pieces now:
     *   (1) capability from GL_VERSION >= 1.4 or ARB/EXT extension bits
     *   (2) a callable core/ARB/EXT proc loaded after the context exists
     * This MUST run before repl_apply_init_bootstrap() below: on
     * unsupported hardware the point-attenuation bootstrap entry has
     * to be skipped entirely rather than invoking a missing entry
     * point. GLR_NO_POINT_PARAMETER (any non-empty value) still forces
     * the unsupported path for testing on capable HW. */
    int gl_major = 0, gl_minor = 0;
    const char *gl_ver = (const char *)glGetString(GL_VERSION);
    if (gl_ver) sscanf(gl_ver, "%d.%d", &gl_major, &gl_minor);
    int has_point_param_core =
        (gl_major > 1) || (gl_major == 1 && gl_minor >= 4);
    int has_point_param_arb =
        glutExtensionSupported("GL_ARB_point_parameters") ? 1 : 0;
    int has_point_param_ext =
        glutExtensionSupported("GL_EXT_point_parameters") ? 1 : 0;
    int point_param_advertised =
        has_point_param_core || has_point_param_arb || has_point_param_ext;
    ReplExecutorPointParameterProc point_param_proc =
        point_param_advertised
            ? glr_ctrl_load_point_parameter_proc(has_point_param_core,
                                                 has_point_param_arb,
                                                 has_point_param_ext)
            : NULL;
    repl_executor_install_point_parameter_proc(point_param_proc);
    const char *no_pp = getenv("GLR_NO_POINT_PARAMETER");
    int forced_off = (no_pp && no_pp[0]) ? 1 : 0;
    int point_param_ok = (point_param_proc != NULL) && !forced_off;
    /* Always log the outcome (both branches) through glr_ctrl_init_log, so
     * the active point-sizing path shows up as an `[init +N.NNNs]` line in
     * the boot trace without a repro. On the unsupported branch it names
     * which of the reasons applies - a deliberate env override vs. a GL
     * context that genuinely lacks the entry point (the gl4es/WebGL case:
     * the extension is advertised but glutGetProcAddress -> eglGetProcAddress
     * yields no callable glPointParameterfv, so the software glPointSize
     * distance approximation is used and GL_POINTS/polygon-mode-point
     * overlays are not distance-attenuated). */
    if (!point_param_ok) {
        if (forced_off) {
            const char *forced_detail =
                point_param_proc
                    ? " (this GL context does support it)"
                    : point_param_advertised
                        ? " (this GL context advertises it, but no callable entry point was found)"
                        : " (this GL context does not support it either)";
            glr_ctrl_init_log(
                "glPointParameterfv disabled via GLR_NO_POINT_PARAMETER=%s; "
                "using the glPointSize distance approximation%s.",
                no_pp,
                forced_detail);
        } else if (!point_param_advertised) {
            glr_ctrl_init_log(
                "glPointParameterfv unsupported by this GL context "
                "(GL_VERSION \"%s\", no GL_ARB/EXT_point_parameters); "
                "using the glPointSize distance approximation. Set "
                "GLR_NO_POINT_PARAMETER=1 to force this path on capable "
                "hardware.",
                gl_ver ? gl_ver : "unknown");
        } else {
            glr_ctrl_init_log(
                "GL point-parameter support is advertised by this GL context "
                "(GL_VERSION \"%s\"), but no glPointParameterfv/"
                "glPointParameterfvARB/glPointParameterfvEXT entry point "
                "could be loaded; using the glPointSize distance "
                "approximation.",
                gl_ver ? gl_ver : "unknown");
        }
    } else {
        glr_ctrl_init_log(
            "glPointParameterfv supported (GL_VERSION \"%s\"); "
            "points use hardware GL_POINT_DISTANCE_ATTENUATION.",
            gl_ver ? gl_ver : "unknown");
    }
    repl_executor_set_point_parameter_supported(point_param_ok);

    /* Hand cpuprof the row tree so its nesting guard can check that every
     * indented section's span really runs inside its parent's. Unconditional
     * and ahead of the GPU block below: it depends on no GL capability, only on
     * this binary's catalog. */
    glr_prof_install_nesting_guard();

    /* GPU timer-query profiling (GL_EXT_timer_query / GL_ARB_timer_query /
     * GL 3.3+). Feeds the profile panel's GPU column: GL_TIME_ELAPSED
     * queries bracket the GPU-relevant prof sections (table in
     * src/app/glr_prof.c, routed through the cpuprof section hooks) with
     * results read back asynchronously a few frames later - never a
     * glFinish. Unsupported context or GLR_NO_GPU_PROF=1 (any non-empty
     * value) leaves gpu_prof disabled and the panel column reads "--". */
    {
        const char *no_gpu_prof = getenv("GLR_NO_GPU_PROF");
        int gpu_prof_forced_off = (no_gpu_prof && no_gpu_prof[0]) ? 1 : 0;
        /* Timestamp capability (additive interval mode) is strictly ARB /
         * GL 3.3+; plain GL_EXT_timer_query contexts (Apple GL 2.1) only
         * get elapsed-bracket mode. */
        int gpu_timer_has_timestamp =
            glutExtensionSupported("GL_ARB_timer_query") ||
            (gl_major > 3 || (gl_major == 3 && gl_minor >= 3));
        int gpu_timer_advertised =
            gpu_timer_has_timestamp ||
            glutExtensionSupported("GL_EXT_timer_query");
        int gpu_prof_on = 0;
        if (!gpu_prof_forced_off && gpu_timer_advertised) {
            GpuProfGlFns gpu_fns =
                glr_ctrl_load_gpu_prof_fns(gpu_timer_has_timestamp);
            gpu_prof_on = gpu_prof_init(&gpu_fns);
        }
        if (gpu_prof_on) {
            glr_prof_install_gpu_section_hooks();
        } else {
            /* Re-init safety (tests call init_gl repeatedly): make sure no
             * stale hooks fire into a now-disabled gpu_prof. */
            prof_install_section_hooks(NULL, NULL);
            gpu_prof_shutdown();
            glr_ctrl_init_log(
                "GPU profile timing disabled (%s); the profile "
                "panel's GPU column will read \"--\".",
                gpu_prof_forced_off
                    ? "GLR_NO_GPU_PROF set"
                    : gpu_timer_advertised
                        ? "timer-query entry points not loadable"
                        : "no GL_EXT/ARB_timer_query support");
        }
    }

    /* Scoped radial fog (GL_NV_fog_distance). The city backdrop and the
     * ocean/radar grid themes draw distance fog over geometry that wraps
     * around the camera, where the default eye-plane (|z|) metric makes
     * the fringes pop in and out as the camera orbits. When the NV
     * extension is present, those passes switch to true radial eye
     * distance; every other (eye-plane-tuned) theme is left alone.
     * Mirrored into Render3dRenderConfig per frame and applied per-pass -
     * NOT set globally, which would fog out the tuned grid themes. */
    g_nv_fog_distance_supported =
        glutExtensionSupported("GL_NV_fog_distance") ? 1 : 0;

    repl_apply_init_bootstrap();

    /* Query actual MSAA sample count from OpenGL for the menu label. */
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    glr_actions_set_msaa_label((int)samples);

    /* Identify the renderer family once; two features below key off it. */
    g_renderer_is_mesa = glr_ctrl_renderer_is_mesa();

    /* Probe the accumulation buffer depth once. A context without one
     * (Emscripten/WebGL: GL_ACCUM_RED_BITS is not even a valid pname
     * there, so the query errors and leaves the 0) would render every
     * accum pass at full cost while glAccum does nothing --
     * glr_ctrl_set_accum masks the feature off when this reports 0. */
    GLint accum_bits = 0;
    glGetIntegerv(GL_ACCUM_RED_BITS, &accum_bits);
    (void)glGetError(); /* clear the GL_INVALID_ENUM a GLES context raises */
    glr_state_render_mut()->accum_bits = (int)accum_bits;
    glr_ctrl_init_log("accum buffer: %d bits%s", (int)accum_bits,
                      (accum_bits > 0 && g_renderer_is_mesa)
                          ? " (software emulation)" : "");

    /* Syntax highlighting defaults off on Mesa. Highlighting does not change
     * the glyph count, it changes how many colored spans those glyphs are
     * split across, and each span issues its own glColor before glRasterPos -
     * which Mesa answers by re-validating raster state (glColor+glRasterPos
     * costs 98.8ns against 61.6ns when the color is unchanged; a whole code
     * panel runs ~2ms of the ~4ms it spends, measured by
     * bench/bench_code_panel_text.c). The On+Shadow mode is worse than slow
     * there: the offset dark copy does not composite correctly.
     *
     * The *default* moves, not the setting - the Config > Syntax highlight row
     * still cycles, a scene's own `@cfg syntax_highlight` still wins (this runs
     * before the initial load below), and GLR_SYNTAX_HIGHLIGHT overrides it for
     * a capture. Logged either way so the boot trace says which branch ran. */
    if (g_renderer_is_mesa) {
        glr_state_set_default_syntax_highlight(SYNTAX_HIGHLIGHT_OFF);
        glr_ctrl_init_log(
            "syntax highlighting off by default: %s re-validates raster state "
            "per color change, making a highlighted code panel cost several ms "
            "a frame (Config > Syntax highlight, or GLR_SYNTAX_HIGHLIGHT=on, "
            "turns it back on).",
            glr_ctrl_gl_renderer_name());
    } else {
        glr_ctrl_init_log("syntax highlighting default: %s (renderer \"%s\").",
                          glr_config_state_name(
                              GLR_CONFIG_SYNTAX_HIGHLIGHT,
                              glr_state_default_syntax_highlight()),
                          glr_ctrl_gl_renderer_name());
    }

    /* Requesting GLUT_STENCIL is not a promise that a native visual actually
     * has stencil planes. Keep the queried width so the stencil visualizer can
     * refuse an otherwise silent no-op context with a useful explanation. */
    GLint stencil_bits = 0;
    glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
    (void)glGetError(); /* GLES may reject this pname too. */
    glr_state_render_mut()->stencil_bits = (int)stencil_bits;
    glr_ctrl_init_log("stencil buffer: %d bits", (int)stencil_bits);

    /* Probe depth readback once (same probe-and-mask pattern as the
     * accum-bits check above): read one depth pixel and see whether the
     * context objects. WebGL cannot read GL_DEPTH_COMPONENT from the
     * default framebuffer at all - and gl4es may silently no-op instead
     * of raising an error - so the web build is hard-disabled instead.
     *
     * Hard-disabled means the probe must not RUN there either. It answers
     * nothing (the verdict below is fixed), but it is not free: the depth
     * read reaches WebGL as a real GL_INVALID_ENUM in the browser console,
     * and the stencil one is worse - gl4es has no GL_STENCIL_INDEX
     * conversion, so it falls back to a full synchronous RGBA glReadPixels
     * (a pipeline drain) and then fails the convert with a four-line
     * "unsupported pixel format 0x1901 / pixel conversion, anticipated
     * abort" burst. All of it startup noise that reads like a crash and
     * buries the real diagnostics. */
#if defined(__EMSCRIPTEN__)
    g_depth_readback_supported = 0;
    g_stencil_readback_supported = 0;
#else
    {
        GLfloat probe_depth = 0.0f;
        (void)glGetError();
        glReadPixels(0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &probe_depth);
        g_depth_readback_supported = (glGetError() == GL_NO_ERROR);
    }
    /* A stencil visual can be requested but not supplied by native GLUT, so
     * require both a non-zero queried width and a working readback. */
    {
        GLubyte probe_stencil = 0;
        (void)glGetError();
        glReadPixels(0, 0, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE,
                     &probe_stencil);
        g_stencil_readback_supported =
            glr_state_render().stencil_bits > 0 &&
            glGetError() == GL_NO_ERROR;
    }
#endif
    /* Reported through glr_ctrl_init_log, like the accum/stencil width lines
     * just above and for the same reason: these are one-shot capability
     * findings, not failures. The tag matters beyond tidiness on the web
     * build, where the shell classifies stderr by the init-trace stamp - an
     * untagged line would render as a console error on every single load. */
    if (!g_depth_readback_supported)
        glr_ctrl_init_log("GL context cannot read the depth buffer; "
                          "Depth view is disabled");
    if (!g_stencil_readback_supported)
        glr_ctrl_init_log("GL context cannot read the stencil buffer; "
                          "Stencil view is disabled");

    /* glutInit has run by the time glr_ctrl_init_gl is called.
     * Unlock glutGetModifiers() reads in editor_input so Cmd / Ctrl /
     * Shift modifier checks land. Tests skip this hook so modifier
     * reads default to "no modifiers held" instead of aborting
     * freeglut for being called pre-init. */
    editor_input_enable_glut_modifier_reads();
    /* Editor reads the code-panel layout through a provider hook so
     * src/editor/ stops depending on src/app/glr_state.h. The
     * controller owns the layout field; the editor only needs the
     * read view for hit-test geometry and the hidden-panel auto-
     * restore. */
    editor_input_set_code_panel_layout_provider(glr_ctrl_code_panel_layout_provider);
    /* App-level config: tell the editor what comment prefix to use
     * for Ctrl+/ toggle. Editor itself has no default - tests that
     * exercise the toggle key path set the prefix explicitly. */
    editor_set_line_comment_prefix("// ");

    /* Capture the memory baseline after REPL bootstrap so it reflects
     * "REPL ready and idle" rather than the bare process at main()
     * entry - the more useful baseline for leak attribution. */
    memprof_init();
}

/* Program name for user-facing messages. Defaults to "gl-repl" so it is
 * sensible even if main() never forwards argv[0] (tests, demos). */
static char g_program_name[64] = "gl-repl";

void glr_ctrl_set_program_name(const char *argv0) {
    const char *base;

    if (!argv0 || !*argv0)
        return;
    base = argv0;
    for (const char *p = argv0; *p; p++)
        if (*p == '/')
            base = p + 1;
    if (*base)
        snprintf(g_program_name, sizeof(g_program_name), "%s", base);
}

const char *glr_ctrl_program_name(void) {
    return g_program_name;
}

void glr_ctrl_bootstrap_repl(const char *input_file) {
    /* Dump-only CLI paths (--dump-code / --dump-flat) call this without
     * going through glr_ctrl_init_gl, so the status sink and
     * export-config bridge would otherwise be missing here and any
     * @cfg in imported files would be silently dropped.
     * glr_ctrl_install_app_services is idempotent so the windowed path
     * (which already installed via glr_ctrl_reset_all) is unaffected. */
    glr_ctrl_install_app_services();
    /* The windowed path patches REPL-state sentinels via
     * glr_ctrl_init_gl -> glr_ctrl_reset_all -> repl_state_reset_program
     * before reaching here. The dump-only paths skip init_gl, so without
     * this the document/flat capacities stay 0 (raw BSS) and every load
     * insert is rejected with a misleading "command store at capacity"
     * - i.e. --dump-code / --dump-flat would print an empty buffer for any
     * non-empty file. Idempotent, so the windowed path is unaffected. */
    repl_state_ensure_sentinels();
    repl_state_variables_reset_predefs();
    editor_state_edit_line_set(repl_load_initial_commands(input_file));
    /* Startup banner. Lives on the controller side so pipeline TUs
     * don't own display-string side effects. */
    if (ui_state_status().kind != UI_STATUS_ERROR) {
        repl_set_status("Ready - type GL commands, press ; to execute. F1 for help. F12 for examples.");
    }
}

void glr_ctrl_set_accum(int mode) {
    GlrRenderState *render = glr_state_render_mut();
    /* GLR_CLI_ACCUM_AUTO (-1): no explicit --accum / --no-accum, so let the
     * renderer decide. A software accumulation buffer (Mesa) satisfies every
     * capability probe and still makes the effect a net loss - each pass is a
     * full-scene re-render plus a CPU-side read/add/write of the color buffer
     * - so AUTO leaves it off there and says how to override. */
    int enabled = (mode != 0);
    if (mode < 0 && g_renderer_is_mesa) {
        fprintf(stderr, "accum: %s emulates the accumulation buffer in "
                        "software; accum effects disabled (--accum forces "
                        "them on)\n",
                glr_ctrl_gl_renderer_name());
        enabled = 0;
    }
    /* accum_bits == 0 means glr_ctrl_init_gl probed the context and found
     * no accumulation buffer at all: each accum pass would re-render the
     * scene at full cost while glAccum stays a no-op, so force the feature
     * off - this one overrides even an explicit --accum. -1 (never probed:
     * tests, dump-only paths) leaves the caller's choice untouched. */
    if (enabled && render->accum_bits == 0) {
        fprintf(stderr, "accum: GL context has no accumulation buffer; "
                        "accum effects disabled\n");
        enabled = 0;
    }
    render->use_accum = enabled ? 1 : 0;
}

void glr_ctrl_set_time(float value) {
    repl_set_time(value);
}

void glr_ctrl_set_edit_line(int line) {
    int count = repl_state_document_count();
    if (line < 0 || count <= 0) return;
    if (line >= count) line = count - 1;
    editor_state_edit_line_set(line);
    editor_load_line_to_input(line);
    editor_scroll_follow_cursor_set(1);
}

void glr_ctrl_set_code_panel_scroll(int top_row) {
    /* Capture affordance: park the panel's first visible row. Cancels
     * follow-scroll first - GLR_EDIT_LINE requests it, and a follow pass would
     * drag the view straight back to the cursor. Rows here are panel rows, not
     * document lines: with code focus off the generated C occupies rows the
     * cursor cannot address at all, which is the reason to scroll instead of
     * parking a cursor. */
    if (top_row < 0) return;
    editor_scroll_follow_cursor_set(0);
    editor_scroll_set(top_row);
}

void glr_ctrl_open_color_picker(int line) {
    int count = repl_state_document_count();
    int sy = 0, sh = 0;
    if (line < 0 || count <= 0) return;
    if (line >= count) line = count - 1;
    /* my (GLUT screen y) anchors the popup vertically; centering on the
     * scene rect keeps it clear of the code panel and window edges. */
    ui_layout_scene_rect(NULL, &sy, NULL, &sh);
    color_picker_start(line,
                       ui_state_viewport().window_h - (sy + sh / 2));
}

/* Shared body of the three capture affordances below: right-click a document
 * row the way a user does. Resolve the row through the live code-panel model,
 * then take the exact production input path, which keeps capture placement,
 * blank-row validation, popup toggling, pointer state, and right-click
 * behavior identical to a real click. The routing - not the caller - decides
 * which of the three right-click popups a row gets, so each caller checks its
 * own popup afterwards rather than asking for one here.
 *
 * Returns 0 without clicking when the row has no on-screen point:
 * GLR_EDIT_LINE may have requested follow-scroll before the main loop, and
 * until a rendered frame applies it an off-screen row must fail so the capture
 * hook retries rather than clicking the wrong row. */
static int glr_ctrl_right_click_source_line(int line) {
    UiRenderSnapshot snap;
    int x;
    int y;

    if (line < 0)
        return 0;

    glr_ctrl_build_ui_snapshot(&snap);
    if (!ui_repl_code_panel_source_line_point(&snap, line, &x, &y))
        return 0;

    glr_ctrl_scripted_passive_motion(x, y);
    glr_ctrl_scripted_mouse(GLUT_RIGHT_BUTTON, GLUT_DOWN, x, y);
    glr_ctrl_scripted_mouse(GLUT_RIGHT_BUTTON, GLUT_UP, x, y);
    return 1;
}

int glr_ctrl_open_gl_state_popup(int line) {
    if (!glr_ctrl_right_click_source_line(line))
        return 0;
    return ui_state_gl_state_inspector().visible &&
           ui_state_gl_state_inspector().source_line_idx == line;
}

int glr_ctrl_open_assign_plot(int line) {
    /* The click carries the "row is not an assignment" rejection with it, so
     * a non-assignment row reports failure here instead of opening a plot. */
    if (!glr_ctrl_right_click_source_line(line))
        return 0;
    return assign_plot_is_open() && assign_plot_source_line() == line;
}

int glr_ctrl_open_command_description(int line, int anchor_dx) {
    /* A row with no authored description opens one of the other popups (or
     * nothing), so this reports failure rather than silently posing something
     * else. Note the routing closes the description when a later right-click
     * lands on an assignment row, so a capture posing both wants this hook
     * last. */
    if (!glr_ctrl_right_click_source_line(line))
        return 0;
    if (!ui_state_command_description().visible ||
        ui_state_command_description().source_line_idx != line)
        return 0;
    /* Re-anchor rather than clicking somewhere else: the click has to land on
     * the row to pick the right card, but where the card then sits is a
     * curation choice for a capture - the row fixes its y, and this slides it
     * along x, away from whatever it would otherwise cover. The renderer
     * clamps into the window, so an offset past the edge simply pins there. */
    if (anchor_dx) {
        UiCommandDescriptionState st = ui_state_command_description();
        ui_state_command_description_open(line, st.anchor_px + anchor_dx,
                                          st.anchor_py);
    }
    return 1;
}

int glr_ctrl_add_assign_plot_series(int line) {
    /* The Shift+right-click path has no synthetic-modifier route through
     * glr_ctrl_mouse (modifiers are read from GLUT, not passed in), so this
     * calls the same subsystem entry the router does. The click routing
     * itself is covered by glr_ctrl_open_assign_plot above and by
     * test_glr_ctrl. */
    if (line < 0)
        return 0;
    assign_plot_toggle_series(line);
    return assign_plot_has_series(line);
}

void glr_ctrl_set_accum_passes(int count) {
    static const int steps[] = { GLR_ACCUM_PASS_LADDER(GLR_ACCUM_PASS_STEP_ENTRY) };
    for (int i = 0; i < (int)ARRAY_LEN(steps); i++) {
        if (steps[i] == count) {
            glr_state_render_mut()->accum_passes = count;
            return;
        }
    }
    fprintf(stderr,
            "gl-repl: GLR_ACCUM_PASSES=%d ignored "
            "(want 1/2/4/6/8/10/12/14/16)\n",
            count);
}

void glr_ctrl_fill_export_layout(ReplExportLayout *out) {
    if (!out) return;
    int sx = 0, sy = 0, sw = 0, sh = 0;
    ui_layout_scene_rect(&sx, &sy, &sw, &sh);
    *out = (ReplExportLayout){
        .render3d_w = sw,
        .render3d_h = sh,
    };
}

/* Per-frame tick (16 ms): advance audio playlist, surface track-change
 * status, advance time variable, advance replay state, decay camera
 * momentum, blink the cursor, decay the status TTL.
 *
 * The work is split from GLUT scheduling so test fixtures (which don't
 * initialize GLUT) can drive a single tick by calling glr_ctrl_tick directly. */
void glr_ctrl_tick(void) {
    int tour_reconstructing =
        glr_pointer_script_tour_suppresses_app_tick();

    /* SIGINT (Ctrl+C) requested quit: the handler only set a flag; the
     * router owns the flag + the recovery save + exit (so no stdio/file
     * I/O runs inside the signal handler). Runs it here on the normal path. */
    glr_ctrl_router_run_pending_quit();

    /* Advance the audio playlist if the current song reached its end
     * (no-op under loop=Song; see glr_audio_tick). */
    glr_audio_tick();

    /* When the playing track changes (either auto-advance from tick
     * or manual next/prev), surface the song name in the status bar.
     * Tracking by generation avoids needing a callback hook into
     * the audio module. */
    if (!glr_audio_is_paused()) {
        static unsigned int last_track_gen = 0;
        unsigned int gen = glr_audio_track_generation();
        if (gen != last_track_gen) {
            last_track_gen = gen;
            int track_idx = glr_audio_current_index();
            const char *name = glr_audio_track_display_name(track_idx);
            if (name && *name) {
                char msg[REPL_DIAG_TEXT_MAX];
                snprintf(msg, sizeof(msg), "Now playing: %s", name);
                ui_state_status_set_music(msg);
            }
        }
    }

    /* A backstep restores one baseline and reconstructs a prefix over as many
     * as eight rendered frames. Those transport frames are not simulation
     * time: advancing t, REPL replay, a view transition, or camera easing here
     * would make the reconstructed boundary depend on prefix length. */
    if (!tour_reconstructing) {
        ReplayRuntimeState *replay = replay_state_mut();

        repl_advance_time(GLR_FRAME_DT_SECS);

        if (replay->active)
            replay_tick_fade_batches(GLR_FRAME_DT_SECS);

        if (replay->active && replay->state == REPLAY_PLAYING) {
            replay->accum += replay->speed * GLR_FRAME_DT_SECS;
            FlatProgramView flat_program = repl_state_flat_program_view();
            while (replay->accum >= 1.0f &&
                   replay->state == REPLAY_PLAYING) {
                replay->accum -= 1.0f;
                replay_advance(flat_program);
            }
        }

        /* An example camera apply defers its saved-3D snapshot update until
         * the next display frame loads the camera modelview. Do not let the
         * view transition consume the older snapshot first: on a rapid
         * 2D-example -> 3D-example switch the projection can still be at its
         * perspective endpoint, so one timer tick would otherwise start the
         * camera-to-3D leg immediately and restore the outgoing 2D orbit.
         * The display drains the pending pose at its documented frame-safe
         * point; the following timer tick can then advance from fresh state. */
        if (!glr_camera_export_has_pending_3d_pose())
            glr_ctrl_tick_view_transition(GLR_FRAME_DT_SECS);
        glr_camera_tick();
    }
    glr_ctrl_tick_overlay_xn();

    {
        /* Ease all floating scene panels (variable / profile / memory)
         * toward their overlay-layout slots. Replay reserves a bottom band
         * so the whole stack lifts above the HUD - the old variable-panel-
         * only replay_lift_px, generalized (Smell #21/#22/#40). */
        int band_h = replay_active()
                   ? REPLAY_HUD_BOTTOM_Y + GLR_CTRL_REPLAY_PANEL_CLEARANCE_PX
                   : 0;
        UiOverlayLayoutIn in = ui_overlay_layout_inputs((UiOverlayLayoutInputs){
            .var_visible                = variable_panel_visible(),
            .var_count                  = repl_eval_predef_view().count,
            .var_collapsed              = variable_panel_collapsed(),
            .profile_mode               = ui_state_profile_panel().mode,
            .profile_collapsed_sections = ui_state_profile_panel().collapsed_sections,
            .memory_mode                = ui_state_memory_panel().mode,
            .assign_plot_visible        = assign_plot_is_open(),
            .assign_plot_expanded       = assign_plot_is_expanded(),
            .assign_plot_series_count   = assign_plot_series_count(),
            .band_h                     = band_h,
        });
        ui_overlay_layout_tick(&in);
    }

    {
        EditorCursorBlinkState *cb = editor_state_cursor_blink_mut();
        (cb->blink_tick)++;
        if (cb->blink_tick >= GLR_CURSOR_BLINK_TICKS) {
            cb->blink_tick = 0;
            cb->cursor_visible = !cb->cursor_visible;
        }
    }

    {
        /* Inline rename no longer rides the status line (it owns its
         * own display state via the snapshot rename view), so the TTL
         * just ages normally. */
        UiStatusState *status = ui_state_status_mut();
        if (status->ttl > 0) {
            status->ttl--;
            /* Age the live message so the banner's telescope-out animation
             * advances even while a per-frame re-emitter keeps renewing the
             * ttl (see ui_state_status_set_kind). Capped to avoid overflow
             * on long-lived hints. */
            if (status->age < (1 << 24))
                status->age++;
        }

        /* Tutorial COMMAND-step hint persistence: while a COMMAND step
         * is active, keep the affordance hint visible by re-emitting it
         * each frame - but only when the status slot is empty (TTL has
         * fully expired) or already shows one of our hint variants
         * (recognised by the "Tutorial: step " prefix via
         * tutorial_status_is_hint). Non-tutorial messages (parse
         * errors, save confirmations, etc.) keep their full TTL window
         * uncontested; once they fade, the next tick brings the hint
         * back. */
        char hint[REPL_STATUS_TEXT_MAX];
        if (tutorial_status_hint(hint, sizeof hint) &&
            (status->ttl <= 0 || tutorial_status_is_hint(status->text))) {
            ui_state_status_set(hint);
        }
    }
}

void glr_ctrl_set_tick_per_frame(int enabled) {
    g_tick_per_frame = enabled ? 1 : 0;
}

/* Application-side response to the host's frame timer. Capture mode
 * transfers fixed-dt advancement to glr_frame_ended(), so the host
 * can keep requesting frames without advancing simulation twice. */
void glr_ctrl_on_frame_timer(void) {
    if (!g_tick_per_frame)
        glr_ctrl_tick();
}

void glr_shutdown(void) {
    glr_ctrl_depth_snapshot_free();
    glr_audio_shutdown();
    hidden_lines_destroy_resources();
    repl_executor_destroy_resources();
}
