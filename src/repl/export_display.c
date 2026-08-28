#include "repl/export_internal.h"

typedef struct {
    char names[EXPORT_NAME_SET_MAX][EXPORT_NAME_MAX];
    int  count;
} ExportNameSet;

typedef void (*ExportDisplayPassSetupFn)(FILE *f);

typedef struct {
    const char                 *label;
    int                         enabled;
    ExportDisplayPassSetupFn    emit_setup;
} ExportDisplayPassSpec;

typedef struct ReplExportRenderPolicy {
    int include_fill;
    int include_vertex_outlines;
    int include_vertex_points;
    int include_scene_overlays;
} ReplExportRenderPolicy;

enum {
    EXPORT_DISPLAY_PASS_COUNT = 3
};

static int export_name_set_has(const ExportNameSet *set, const char *name) {
    if (!set || !name || !name[0])
        return 0;
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->names[i], name) == 0)
            return 1;
    }
    return 0;
}

static void export_name_set_add(ExportNameSet *set, const char *name) {
    if (!set || !name || !name[0] || export_name_set_has(set, name))
        return;
    if (set->count >= EXPORT_NAME_SET_MAX)
        return;
    snprintf(set->names[set->count++], EXPORT_NAME_MAX, "%s", name);
}

static void export_choose_name(ExportNameSet *set, char out[EXPORT_NAME_MAX],
                               const char *preferred, const char *fallback) {
    const char *base = preferred && preferred[0] ? preferred : "generated_name";
    if (!export_name_set_has(set, base)) {
        snprintf(out, EXPORT_NAME_MAX, "%s", base);
        export_name_set_add(set, out);
        return;
    }
    if (fallback && fallback[0] && !export_name_set_has(set, fallback)) {
        snprintf(out, EXPORT_NAME_MAX, "%s", fallback);
        export_name_set_add(set, out);
        return;
    }
    for (int suffix = 2; suffix < 10000; suffix++) {
        char candidate[EXPORT_NAME_MAX];
        snprintf(candidate, sizeof(candidate), "%s_%d", base, suffix);
        if (!export_name_set_has(set, candidate)) {
            snprintf(out, EXPORT_NAME_MAX, "%s", candidate);
            export_name_set_add(set, out);
            return;
        }
    }
    snprintf(out, EXPORT_NAME_MAX, "%s_generated", base);
    export_name_set_add(set, out);
}

static void export_name_set_add_user_identifiers(ExportNameSet *set) {
    for (int var_idx = 0; var_idx < g_num_predef_vars; var_idx++)
        export_name_set_add(set, g_predef_vars[var_idx].name);

    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        const char *alias = repl_func_alias_get(slot);
        if (alias)
            export_name_set_add(set, alias);
    }

    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        const GLCmd *cmd = &repl_state_document_cmds()[cmd_idx];
        if (!cmd->valid || cmd->type != CMD_FUNC_DEF)
            continue;
        int fn = (int)cmd->args[0];
        const char *alias = repl_func_alias_get(fn);
        char fallback_name[REPL_FUNC_NAME_MAX + 8];
        if (alias) {
            export_name_set_add(set, alias);
        } else {
            snprintf(fallback_name, sizeof(fallback_name), "func%d", fn);
            export_name_set_add(set, fallback_name);
        }
    }
}

void export_generated_names_init(ExportGeneratedNames *names) {
    ExportNameSet used;

    memset(&used, 0, sizeof(used));
    memset(names, 0, sizeof(*names));
    export_name_set_add_user_identifiers(&used);

    export_choose_name(&used, names->draw_scene,
                       "draw_scene", "draw_repl_scene");
    export_choose_name(&used, names->draw_tuning_overlay,
                       "draw_tuning_overlay", "draw_repl_tuning_overlay");
    export_choose_name(&used, names->tuning_step,
                       "tuning_step", "tune_compute_step");
    export_choose_name(&used, names->hud_text,
                       "hud_text", "repl_hud_text");
    export_choose_name(&used, names->tuning_window_width,
                       "window_width", "tuning_window_width");
    export_choose_name(&used, names->tuning_window_height,
                       "window_height", "tuning_window_height");
    export_choose_name(&used, names->keyboard_key,
                       "key", "pressed_key");
    export_choose_name(&used, names->keyboard_mouse_x,
                       "mouse_x", "keyboard_mouse_x");
    export_choose_name(&used, names->keyboard_mouse_y,
                       "mouse_y", "keyboard_mouse_y");
    export_choose_name(&used, names->timer_now_ms,
                       "timer_now_ms", "repl_timer_now_ms");
    export_choose_name(&used, names->timer_delay_ms,
                       "timer_delay_ms", "repl_timer_delay_ms");
    export_choose_name(&used, names->tick,
                       "tick", "repl_tick");
    export_choose_name(&used, names->tune_modifiers,
                       "modifiers", "tuning_modifiers");
    export_choose_name(&used, names->tune_normalized_key,
                       "normalized_key", "tuning_key");
    export_choose_name(&used, names->tune_step_scale,
                       "step_scale", "tuning_step_scale");
    export_choose_name(&used, names->overlay_width,
                       "overlay_width", "tuning_overlay_width");
    export_choose_name(&used, names->overlay_height,
                       "overlay_height", "tuning_overlay_height");
    export_choose_name(&used, names->overlay_text_y,
                       "text_y", "overlay_text_y");
}

/* Word-boundary-aware substring scan: returns 1 iff `needle` appears in
 * `haystack` and the byte immediately before its match position is NOT
 * an identifier character. Avoids false hits like `glRand(` triggering
 * `rand(` detection or `dataA[0]` triggering `A[`. */
static int export_text_uses_token(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t nlen = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        if (p == haystack ||
            (!isalnum((unsigned char)p[-1]) && p[-1] != '_'))
            return 1;
        p += nlen;
    }
    return 0;
}

ExportNeeds export_collect_needs(void) {
    ExportNeeds needs = {
        .needs_tess = export_uses_tess_commands(),
        .needs_rand = 0,
        .needs_clamp = 0,
        .needs_lerp = 0,
        .needs_smoothstep = 0,
        .needs_sign = 0,
        .needs_label = 0,
        .needs_console = 0,
        .needs_scratch_a = 0,
        .needs_scratch_b = 0,
        .needs_scratch_c = 0,
        .needs_glfloat1 = 0,
        .needs_glfloat3 = 0,
        /* The generated glLightModelfv initialization always uses this
         * helper, independently of the user document. */
        .needs_glfloat4 = 1,
        .needs_gldouble4 = 0,
        .needs_glfloat16 = 0,
    };

    /* The generated point-attenuation bootstrap is conditional on both the
     * runtime entry point and its export toggle, matching export_setup.c's
     * emission gates. */
    if (repl_executor_point_parameter_supported() &&
        repl_cfg_get_int(REPL_EXPORT_CFG_SLUG_POINT_ATTENUATION, 1))
        needs.needs_glfloat3 = 1;

    /* Check each command for rand() / scratch-array / label
     * references. Detection is intentionally a textual scan over
     * source lines (not a CmdType check) so that reads inside arg
     * expressions - e.g. `glVertex3f(A[i], B[i], C[i])` - count
     * alongside writes (`A[0] = ...`). The label wrapper is keyed
     * off the command type rather than text, so indirect label codegen is
     * classified consistently. */
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        const GLCmd *cmd = &repl_state_document_cmds()[cmd_idx];
        if (!cmd->valid) continue;
        if (cmd->type == CMD_LABEL) needs.needs_label = 1;
        if (cmd->type == CMD_CONSOLE) needs.needs_console = 1;
        switch (cmd->type) {
        case CMD_MATERIALFV:
            if (cmd->num_args == 3)
                needs.needs_glfloat1 = 1;
            else
                needs.needs_glfloat4 = 1;
            break;
        case CMD_POINT_PARAMETER_FV:
            needs.needs_glfloat3 = 1;
            break;
        case CMD_CLIP_PLANE:
            needs.needs_gldouble4 = 1;
            break;
        case CMD_MULT_MATRIXF:
            if (!repl_cmd_mult_matrix_from_array(cmd))
                needs.needs_glfloat16 = 1;
            break;
        case CMD_FOG_FV:
            needs.needs_glfloat4 = 1;
            break;
        default:
            break;
        }
        /* glMultMatrixf(A) reads its scratch array by bare name, so the
         * "A[" text scan below cannot see it; the command carries the array
         * in args[0], so key off that rather than widening the scan to a
         * bare `A` token. The compound-literal form names no array at all
         * and must not conjure an unused global.
         *
         * CMD_SCRATCH_BLOCK_ASSIGN is here for the same reason even though
         * its subscript is mandatory and the scan would therefore find it:
         * the row *exports* as `A[0] = ...`, so getting this wrong emits C
         * that assigns to an undeclared array, and the command type is the
         * authoritative answer where the scan is a heuristic. */
        if ((cmd->type == CMD_MULT_MATRIXF &&
             repl_cmd_mult_matrix_from_array(cmd)) ||
            cmd->type == CMD_SCRATCH_BLOCK_ASSIGN) {
            switch ((int)cmd->args[0]) {
            case 0: needs.needs_scratch_a = 1; break;
            case 1: needs.needs_scratch_b = 1; break;
            case 2: needs.needs_scratch_c = 1; break;
            default: break;
            }
        }
        const char *src = export_document_text(cmd_idx);
        if (export_text_uses_token(src, "rand(")) needs.needs_rand = 1;
        if (export_text_uses_token(src, "rand2(")) needs.needs_rand = 1;
        if (export_text_uses_token(src, "repl_randf(")) needs.needs_rand = 1;
        if (export_text_uses_token(src, "repl_rand2f(")) needs.needs_rand = 1;
        if (export_text_uses_token(src, "clamp(") ||
            export_text_uses_token(src, "repl_clampf(")) needs.needs_clamp = 1;
        if (export_text_uses_token(src, "lerp(") ||
            export_text_uses_token(src, "repl_lerpf(")) needs.needs_lerp = 1;
        if (export_text_uses_token(src, "smoothstep(") ||
            export_text_uses_token(src, "repl_smoothstepf("))
            needs.needs_smoothstep = 1;
        if (export_text_uses_token(src, "sign(") ||
            export_text_uses_token(src, "repl_signf(")) needs.needs_sign = 1;
        if (export_text_uses_token(src, "A["))    needs.needs_scratch_a = 1;
        if (export_text_uses_token(src, "B["))    needs.needs_scratch_b = 1;
        if (export_text_uses_token(src, "C["))    needs.needs_scratch_c = 1;
    }

    needs.tune_count = repl_collect_tuned_vars(
        repl_state_document_cmds(), repl_state_document_count(),
        export_source_text_view(), needs.tune_names, REPL_TUNE_MAX_KNOBS,
        &needs.tune_total);
    /* Which of those knobs are `// @bool` toggles. Collected as a separate
     * name set and matched by name (rather than threaded through the shared
     * collector) because the two tags are independent: a var may be @bool
     * without @tune, and the knob order is the @tune walk's. */
    {
        const char *bool_names[MAX_PREDEF_VARS];
        int bool_count = repl_collect_bool_vars(
            repl_state_document_cmds(), repl_state_document_count(),
            export_source_text_view(), bool_names, MAX_PREDEF_VARS, NULL);
        for (int i = 0; i < needs.tune_count; i++) {
            needs.tune_is_bool[i] = 0;
            for (int b = 0; b < bool_count; b++) {
                if (strcmp(bool_names[b], needs.tune_names[i]) == 0) {
                    needs.tune_is_bool[i] = 1;
                    break;
                }
            }
        }
    }

    return needs;
}

static ReplExportRenderPolicy export_render_policy(void) {
    ReplExportRenderPolicy policy = {
        .include_fill = 1,
        .include_vertex_outlines = 0,
        .include_vertex_points = 0,
        .include_scene_overlays = 0,
    };
    return policy;
}

static void emit_export_outline_pass_setup(FILE *f) {
    fprintf(f, "  glEnable(GL_COLOR_MATERIAL);\n");
    fprintf(f, "  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);\n");
    fprintf(f, "  glColor3f(0.0f, 0.0f, 0.0f);\n");
    fprintf(f, "  glDisable(GL_COLOR_MATERIAL);\n");
    fprintf(f, "  glEnable(GL_POLYGON_OFFSET_LINE);\n");
    fprintf(f, "  glPolygonOffset(%#.6gf, %#.6gf);\n",
                  (double)REPL_OUTLINE_POLYGON_OFFSET_FACTOR, (double)REPL_OUTLINE_POLYGON_OFFSET_UNITS);
    fprintf(f, "  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);\n");
    fprintf(f, "  glLineWidth(1.2f);\n");
    fprintf(f, "  glEnable(GL_LIGHTING);\n");
}

static void emit_export_point_pass_setup(FILE *f) {
    fprintf(f, "  glEnable(GL_COLOR_MATERIAL);\n");
    fprintf(f, "  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);\n");
    fprintf(f, "  glColor3f(0.0f, 0.0f, 0.0f);\n");
    fprintf(f, "  glDisable(GL_COLOR_MATERIAL);\n");
    fprintf(f, "  glPointSize(8.0f);\n");
    fprintf(f, "  glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);\n");
    fprintf(f, "  glEnable(GL_LIGHTING);\n");
}

/* Build the display() pass list. Multipass scaffolding must count
 * these enabled flags, not raw cfg toggles, because cfg-backed overlay
 * passes may be intentionally disabled in export. */
static size_t export_build_display_passes(
    ExportDisplayPassSpec passes[EXPORT_DISPLAY_PASS_COUNT]) {
    ReplExportRenderPolicy policy = export_render_policy();

    if (!passes)
        return 0;

    passes[0].label = "Vertex Fill Pass";
    passes[0].enabled = policy.include_fill;
    passes[0].emit_setup = NULL;

    /* Standalone export currently emits scene geometry only. Live overlay
     * cfg flags are not mirrored because generated C does not share the
     * app's overlay code-generation path - unifying the two would let export
     * emit a matching outline pass. A stencil-buffer approach (draw with the
     * color buffer off using line/point polygon mode, then fill the stencil
     * in a second pass) would be more robust and simpler than the current
     * approach of using LIGHTING and setting lights to black for the
     * outline pass. */
    passes[1].label = "Vertex Outline Pass";
    passes[1].enabled = policy.include_vertex_outlines;
    passes[1].emit_setup = emit_export_outline_pass_setup;

    passes[2].label = "Vertex Point Pass";
    passes[2].enabled = policy.include_vertex_points;
    passes[2].emit_setup = emit_export_point_pass_setup;

    (void)policy.include_scene_overlays;
    return EXPORT_DISPLAY_PASS_COUNT;
}

static int export_count_enabled_pass_specs(
    const ExportDisplayPassSpec *passes, size_t pass_count) {
    int count = 0;
    size_t pass_idx;

    if (!passes)
        return 0;

    for (pass_idx = 0; pass_idx < pass_count; pass_idx++) {
        if (passes[pass_idx].enabled)
            count++;
    }
    return count;
}

int export_display_has_multiple_enabled_passes(void) {
    ExportDisplayPassSpec passes[EXPORT_DISPLAY_PASS_COUNT];
    size_t pass_count = export_build_display_passes(passes);
    return export_count_enabled_pass_specs(passes, pass_count) > 1;
}

static void emit_export_geometry_pass(FILE *f,
                                      const ExportDisplayPassSpec *pass,
                                      int needs_restore,
                                      const ExportGeneratedNames *names) {
    if (!pass || !pass->enabled)
        return;

    fprintf(f, "\n  /* %s */\n", pass->label);
    fprintf(f, "  glPushAttrib(GL_ALL_ATTRIB_BITS);\n");
    if (pass->emit_setup)
        pass->emit_setup(f);
    fprintf(f, "  glPushMatrix();\n");
    if (needs_restore)
        fprintf(f, "  restore_repl_vars();\n");
    fprintf(f, "  %s();\n", names->draw_scene);
    fprintf(f, "  glPopMatrix();\n");
    fprintf(f, "  glPopAttrib();\n");
}

static void emit_export_display_begin(FILE *f, const ExportNeeds *needs) {
    /* display() opening lines come from g_display_header so the panel
     * (which renders the same array) and the exported file stay
     * byte-identical here. */
    fprintf(f, "\n/* Draw one frame. */\n");
    for (int line_idx = 0; g_display_header[line_idx]; line_idx++) {
        fprintf(f, "%s\n", g_display_header[line_idx]);
        if (line_idx == 0 && needs && needs->needs_console) {
            fprintf(f,
                "  static int s_frame = 0;\n"
                "  printf(\"\\033[2J\\033[H--- frame %%d ---\\n\", ++s_frame);\n");
        }
    }
    /* Eye-space positions are submitted with the identity modelview, once per
     * frame, before the camera transform is applied. */
    {
        int n_pos = repl_export_lights_pre_camera_line_count();
        for (int pos_idx = 0; pos_idx < n_pos; pos_idx++) {
            char line[MAX_LINE_LEN];
            repl_export_lights_pre_camera_line(pos_idx, line, sizeof(line));
            export_write_c89_line(f, line);
        }
    }
    emit_export_cam_lines(f);
    /* Light positions are set after the camera transforms so
     * glLightfv(GL_POSITION) snapshots the post-camera modelview and
     * lights stay anchored in world space as the camera orbits. The
     * Eye-space positions were emitted above, before the camera. The
     * non-positional light state (colors + baseline glDisable) is emitted
     * into init(). The render-config state is emitted there as well; user
     * render state remains in source order inside the geometry pass,
     * matching immediate-mode execution.
     *
     * Lights are emitted before g_header_post to match the panel's
     * rendering order; both consumers walk: display_header -> cam ->
     * lights -> header_post -> user code. */
    {
        int n_pos = repl_export_lights_display_line_count();
        for (int pos_idx = 0; pos_idx < n_pos; pos_idx++) {
            char line[MAX_LINE_LEN];
            repl_export_lights_display_line(pos_idx, line, sizeof(line));
            export_write_c89_line(f, line);
        }
    }
    /* g_header_post: additional setup after the camera/light lines. */
    for (int line_idx = 0; g_header_post[line_idx]; line_idx++)
        export_write_c89_line(f, g_header_post[line_idx]);
}

static void emit_export_display_geometry(FILE *f,
                                         const ExportGeneratedNames *names) {
    ExportDisplayPassSpec passes[EXPORT_DISPLAY_PASS_COUNT];
    size_t pass_count = export_build_display_passes(passes);

    /* `t` is advanced by the tick() timer at a fixed step (see the
     * footer's tick() / glutTimerFunc setup), mirroring the live
     * REPL's repl_state_time_advance(0.01667). display() only renders;
     * it does NOT touch t. Setting t from elapsed wall time here
     * would make tDelta = (t - tLast) * 10 frame-rate dependent and
     * diverge from the REPL preview, where tDelta is constant. */

    /* Multipass rendering: snapshot predef vars before the first pass
     * and restore between passes so each pass starts from the same
     * state, while the LAST pass's mutations carry into the next
     * frame. */
    int multipass = export_count_enabled_pass_specs(passes, pass_count) > 1 &&
                    export_has_persistent_predef_vars();
    if (multipass)
        fprintf(f, "  save_repl_vars();\n");

    int rendered_passes = 0;
    for (size_t i = 0; i < pass_count; i++) {
        if (!passes[i].enabled) continue;
        int needs_restore = multipass && rendered_passes > 0;
        emit_export_geometry_pass(f, &passes[i], needs_restore, names);
        rendered_passes++;
    }
}

/* QWERTY columns: numeric knob i raises with k_tune_up_keys[i] and lowers
 * with k_tune_down_keys[i]; a bool knob uses only k_tune_up_keys[i] as its
 * toggle key. Index-aligned; length REPL_TUNE_MAX_KNOBS. */
static const char k_tune_up_keys[]   = "qwertyuio";
static const char k_tune_down_keys[] = "asdfghjkl";

STATIC_ASSERT(sizeof(k_tune_up_keys) - 1 == REPL_TUNE_MAX_KNOBS,
              tune_up_key_count);
STATIC_ASSERT(sizeof(k_tune_down_keys) - 1 == REPL_TUNE_MAX_KNOBS,
              tune_down_key_count);

/* 1 if any knob is a stepped number. All-`@bool` knob sets need neither the
 * swatch-step mirror nor the Shift/Ctrl step scale, and emitting them unused
 * would trip -Wunused-function / -Wunused-variable in the exported file. */
static int tune_has_stepped_knob(const ExportNeeds *needs) {
    for (int i = 0; i < needs->tune_count; i++)
        if (!needs->tune_is_bool[i])
            return 1;
    return 0;
}

/* Prologue: window-size globals (captured in reshape, read by the HUD),
 * the swatch-step mirror, a formatted HUD-text helper, and the HUD draw pass.
 * Emitted only when at least one variable is @tune-tagged. */
void write_tune_helpers(FILE *f, const ExportNeeds *needs,
                        const ExportGeneratedNames *names) {
    if (needs->tune_total > needs->tune_count)
        fprintf(f,
            "\n/* @tune: %d variables tagged; capped at %d keyboard knobs. */\n",
            needs->tune_total, REPL_TUNE_MAX_KNOBS);
    fprintf(f,
        "\n/* @tune knobs: keyboard-adjustable variables + overlay, generated because\n"
        " * one or more `float` decls carried a `// @tune` tag. */\n"
        "static int %s = 800;\n"
        "static int %s = 600;\n",
        names->tuning_window_width,
        names->tuning_window_height);
    if (tune_has_stepped_knob(needs))
        fprintf(f,
        "\n/* Mirror of repl_eval_swatch_step() (src/repl/eval.c); pinned by the\n"
        " * swatch-parity test in tests/test_repl_tune.c so it cannot drift. */\n"
        "static float %s(float value) {\n"
        "  float magnitude = fabsf(value);\n"
        "  float exponent = (magnitude < 10.0f) ? 0.0f : floorf(log10f(magnitude));\n"
        "  return 0.05f * powf(10.0f, exponent);\n"
        "}\n",
        names->tuning_step);
    fprintf(f,
        "\n/* Draw formatted bitmap text in screen-space HUD coordinates. */\n"
        "static void %s(float x, float y, const char *fmt, ...) {\n"
        "  const char *ch;\n"
        "  char line_text[96];\n"
        "  va_list args;\n"
        "\n"
        "  va_start(args, fmt);\n"
        "  vsnprintf(line_text, sizeof line_text, fmt, args);\n"
        "  va_end(args);\n"
        "\n"
        "  glRasterPos2f(x, y);\n"
        "  for (ch = line_text; *ch; ch++)\n"
        "    glutBitmapCharacter(GLUT_BITMAP_9_BY_15, (unsigned char)*ch);\n"
        "}\n"
        "\nstatic void %s(void) {\n"
        "  int %s = %s;\n"
        "  int %s = %s;\n"
        "  float %s = (float)%s - 18.0f;\n"
        "\n"
        "  glMatrixMode(GL_PROJECTION);\n"
        "  glPushMatrix();\n"
        "  glLoadIdentity();\n"
        "  glOrtho(0, %s, 0, %s, -1, 1);\n"
        "\n"
        "  glMatrixMode(GL_MODELVIEW);\n"
        "  glPushMatrix();\n"
        "  glLoadIdentity();\n"
        "\n"
        "  glPushAttrib(GL_ALL_ATTRIB_BITS);\n"
        "  glDisable(GL_LIGHTING);\n"
        "  glDisable(GL_DEPTH_TEST);\n"
        "  glColor3f(1.0f, 1.0f, 1.0f);\n"
        "\n",
        names->hud_text,
        names->draw_tuning_overlay,
        names->overlay_width,
        names->tuning_window_width,
        names->overlay_height,
        names->tuning_window_height,
        names->overlay_text_y,
        names->overlay_height,
        names->overlay_width,
        names->overlay_height);
    for (int i = 0; i < needs->tune_count; i++) {
        /* A `@bool` knob reads as a checkbox: the variable is still a float,
         * so "on" is the same > 0.5 test the scene's own `if` uses. */
        if (needs->tune_is_bool[i])
            fprintf(f,
                "\n"
                "  %s(8.0f, %s, \"%c  %s [%%s]\", %s > 0.5f ? \"x\" : \" \");\n"
                "  %s -= 16.0f;\n",
                names->hud_text,
                names->overlay_text_y,
                k_tune_up_keys[i], needs->tune_names[i],
                needs->tune_names[i],
                names->overlay_text_y);
        else
            fprintf(f,
                "\n"
                "  %s(8.0f, %s, \"%c/%c  %s = %%.4g\", (double)%s);\n"
                "  %s -= 16.0f;\n",
                names->hud_text,
                names->overlay_text_y,
                k_tune_up_keys[i], k_tune_down_keys[i],
                needs->tune_names[i], needs->tune_names[i],
                names->overlay_text_y);
    }
    fprintf(f,
        "  glPopAttrib();\n"
        "\n"
        "  glMatrixMode(GL_PROJECTION);\n"
        "  glPopMatrix();\n"
        "  glMatrixMode(GL_MODELVIEW);\n"
        "  glPopMatrix();\n"
        "}\n");
}

/* Injected into the exported keyboard() body: decode the key (folding Shift
 * uppercase and Ctrl control-codes back to the base letter) and apply the
 * swatch step, Shift = fine and Ctrl = coarse - mirroring the in-app numeric
 * swatch and variable-panel adjustment multipliers. */
static void emit_tune_keyboard_decls(FILE *f, const ExportNeeds *needs,
                                     const ExportGeneratedNames *names) {
    fprintf(f,
        "  int %s = glutGetModifiers();\n"
        "  unsigned char %s = %s;\n",
        names->tune_modifiers,
        names->tune_normalized_key,
        names->keyboard_key);
    if (tune_has_stepped_knob(needs))
        fprintf(f, "  float %s = 1.0f;\n", names->tune_step_scale);
    fprintf(f, "\n");
}

static void emit_tune_keyboard_handlers(FILE *f, const ExportNeeds *needs,
                                        const ExportGeneratedNames *names) {
    fprintf(f,
        "  if ((%s & GLUT_ACTIVE_CTRL) && %s >= 1 && %s <= 26) {\n"
        "    %s = (unsigned char)(%s - 1 + 'a');\n"
        "  } else if (%s >= 'A' && %s <= 'Z') {\n"
        "    %s = (unsigned char)(%s + ('a' - 'A'));\n"
        "  }\n"
        "\n",
        names->tune_modifiers,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key,
        names->tune_normalized_key);
    if (tune_has_stepped_knob(needs))
        fprintf(f,
            "  if (%s & GLUT_ACTIVE_SHIFT)\n"
            "    %s *= "
                REPL_EXPORT_STRINGIFY(GLR_ADJUST_FINE_SCALE) ";\n"
            "  if (%s & GLUT_ACTIVE_CTRL)\n"
            "    %s *= "
                REPL_EXPORT_STRINGIFY(GLR_ADJUST_COARSE_SCALE) ";\n"
            "\n",
            names->tune_modifiers,
            names->tune_step_scale,
            names->tune_modifiers,
            names->tune_step_scale);
    for (int i = 0; i < needs->tune_count; i++) {
        const char *v = needs->tune_names[i];
        /* A `@bool` knob has two states, not a range: its first-column key
         * toggles it. The step scale (Shift/Ctrl) is meaningless here and
         * is deliberately not applied. */
        if (needs->tune_is_bool[i])
            fprintf(f,
                "  if (%s == '%c') %s = (%s > 0.5f) ? 0.0f : 1.0f;\n",
                names->tune_normalized_key, k_tune_up_keys[i], v, v);
        else
            fprintf(f,
                "  if (%s == '%c') %s += %s(%s) * %s;\n"
                "  if (%s == '%c') %s -= %s(%s) * %s;\n",
                names->tune_normalized_key, k_tune_up_keys[i],
                v, names->tuning_step, v, names->tune_step_scale,
                names->tune_normalized_key, k_tune_down_keys[i],
                v, names->tuning_step, v, names->tune_step_scale);
    }
}

static void emit_export_display_tail(FILE *f, const ExportNeeds *needs,
                                     const ReplExportLayout *layout,
                                     const ExportGeneratedNames *names) {
    int include_tess = needs ? needs->needs_tess : 0;
    int knobs = needs ? needs->tune_count : 0;

    if (knobs > 0)
        fprintf(f, "  %s();\n", names->draw_tuning_overlay);
    fprintf(f,
        "  glPopAttrib();\n"
        "  glutSwapBuffers();\n"
        "}\n"
        "\n"
        "/* Keep the projection matched to the current window size. */\n"
        "void reshape(int w, int h) {\n");
    if (knobs > 0)
        fprintf(f, "  %s = w;\n  %s = h;\n",
                names->tuning_window_width,
                names->tuning_window_height);
    fprintf(f,
        "  glViewport(0, 0, w, h);\n"
        "  glMatrixMode(GL_PROJECTION);\n"
        "  glLoadIdentity();\n");
    {
        const char *proj[REPL_EXPORT_PROJ_LINES];
        int pn = repl_export_reshape_projection_lines(proj);
        for (int j = 0; j < pn; j++)
            fprintf(f, "%s\n", proj[j]);
    }
    fprintf(f,
        "  glMatrixMode(GL_MODELVIEW);\n"
        "}\n"
        "\n"
        "/* Keyboard controls: Space toggles rotation; Esc exits. */\n"
        "void keyboard(unsigned char %s, int %s, int %s) {\n",
        names->keyboard_key,
        names->keyboard_mouse_x,
        names->keyboard_mouse_y);
    if (knobs > 0)
        emit_tune_keyboard_decls(f, needs, names);
    fprintf(f,
        "  (void)%s;\n"
        "  (void)%s;\n",
        names->keyboard_mouse_x,
        names->keyboard_mouse_y);
    if (knobs > 0)
        fprintf(f, "\n");
    if (knobs > 0)
        emit_tune_keyboard_handlers(f, needs, names);
    fprintf(f,
        "  if (%s == ' ')\n"
        "    g_rotating = !g_rotating;\n"
        "  if (%s == 27)\n"
        "    exit(0);\n"
        "}\n"
        "\n",
        names->keyboard_key,
        names->keyboard_key);
    fprintf(f,
        "static double %s(void) {\n"
        "  return (double)glutGet(GLUT_ELAPSED_TIME);\n"
        "}\n"
        "\n"
        "static int %s(double *next_deadline_ms) {\n"
        "  double now = %s();\n"
        "  int delay;\n"
        "\n"
        "  if (*next_deadline_ms == 0.0)\n"
        "    *next_deadline_ms = now;\n"
        "  *next_deadline_ms += 1000.0 / 60.0;\n"
        "  if (*next_deadline_ms < now)\n"
        "    *next_deadline_ms = now;\n"
        "\n"
        "  delay = (int)(*next_deadline_ms - now + 0.5);\n"
        "  if (delay < 1)\n"
        "    delay = 1;\n"
        "  return delay;\n"
        "}\n"
        "\n",
        names->timer_now_ms,
        names->timer_delay_ms,
        names->timer_now_ms);
    fprintf(f,
        "/* Advance animation at roughly 60 frames per second. */\n"
        "void %s(int value) {\n"
        "  static double next_deadline_ms = 0.0;\n"
        "  int delay;\n"
        "\n"
        "  (void)value;\n"
        "  /* Fixed-step time advance, matching the live REPL's\n"
        "   * repl_state_time_advance(0.01667) timer. Keeps tDelta = (t -\n"
        "   * tLast) * 10 constant across frames, independent of how long\n"
        "   * each render actually takes. */\n"
        "  t += 0.01667f;\n"
        "  if (g_rotating)\n"
        "    g_angle += 0.5f;\n"
        "  glutPostRedisplay();\n"
        "  delay = %s(&next_deadline_ms);\n"
        "  glutTimerFunc((unsigned int)delay, %s, 0);\n"
        "}\n"
        "\n"
        "/* One-time OpenGL state setup. */\n"
        "void init(void) {\n"
        "  glLineWidth(1.5f);\n",
        names->tick,
        names->timer_delay_ms,
        names->tick);
    emit_export_init_section_to_file(f, include_tess);

    /* Use the actual scene rect so the exported window preserves the REPL
     * viewport's aspect ratio and geometry is never clipped. Fall back to
     * 800x600 when dimensions aren't available (headless / demo export).
     * Read from the explicit ReplExportLayout struct rather than calling
     * ui_layout_scene_rect directly. */
    int sw = layout ? layout->render3d_w : 0;
    int sh = layout ? layout->render3d_h : 0;
    if (sw <= 0) sw = 800;
    if (sh <= 0) sh = 600;
    emit_footer_post_init(f, sw, sh, names->tick);
}

void emit_export_display(FILE *f, const ExportNeeds *needs,
                         const ReplExportLayout *layout,
                         const ExportGeneratedNames *names) {
    emit_export_display_begin(f, needs);
    emit_export_display_geometry(f, names);
    emit_export_display_tail(f, needs, layout, names);
}
