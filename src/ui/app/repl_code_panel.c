#include "ui/app/repl_code_panel.h"
#include "ui/app/repl_code_panel_statusbar.h"

#include "c_compat.h"
#include "editor/state.h"
#include "repl/command_spec.h"
#include "repl/attrib_bits.h"       /* REPL_ATTRIB_BIT_COUNT (per-bit gutter palette) */
#include "repl/program_query.h"
#include "repl/eval.h"
#include "repl/export.h"
#include "repl/state_views.h"
#include "support/cpuprof.h"
#include "ui/subsystems/color_picker.h"
#include "ui/app/numeric_swatch.h"
#include "ui/core/gl_2d.h"
#include "ui/app/layout.h"
#include "ui/app/menu_bar.h"
#include "ui/core/metrics.h"
#include "ui/app/panels.h"
#include "ui/app/scene_tabs.h"
#include "ui/core/text_layout.h"
#include "ui/core/text_panel.h"
#include "ui/core/theme.h"
#include "subsystems/edit_overlays/edit_overlays.h"
#include "subsystems/tutorial/tutorial_animation.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define UI_REPL_CODE_PANEL_MAX_STATIC_ROWS 256
#define UI_REPL_CODE_PANEL_MAX_ROWS \
    (MAX_EDITOR_COMMANDS + MAX_VIRTUAL_LINES + UI_REPL_CODE_PANEL_MAX_STATIC_ROWS + 2)
#define UI_REPL_CODE_PANEL_MAX_GENERATED_TEXT_ROWS 256
#define REPL_CODE_PANEL_FUNC_RETURN_PREFIX "void "

/* repl_code_panel_apply_fade_segments emits at most:
 *   - 1 settled-prefix segment
 *   - TUTORIAL_FADE_SETTLE_CHARS easing-tail per-char segments
 *   - 1 actively-revealing head segment
 *   - 1 not-yet-revealed tail segment
 * = TUTORIAL_FADE_SETTLE_CHARS + 3 segments.
 *
 * Bumping TUTORIAL_FADE_SETTLE_CHARS past
 * UI_TEXT_PANEL_MAX_COLOR_SEGMENTS - 3 would silently corrupt the fade
 * because the segment-capacity break in apply_fade_segments would drop
 * trailing chars. */
STATIC_ASSERT(TUTORIAL_FADE_SETTLE_CHARS + 3 <= UI_TEXT_PANEL_MAX_COLOR_SEGMENTS,
              "TUTORIAL_FADE_SETTLE_CHARS too large for color-segment budget");

static UiTextPanelRow g_repl_code_panel_rows[UI_REPL_CODE_PANEL_MAX_ROWS];
/* *2 allows room for concatenating virtual_line->text and ->aux. */
static char g_repl_code_panel_generated_text[UI_REPL_CODE_PANEL_MAX_GENERATED_TEXT_ROWS][MAX_LINE_LEN * 2];

/* Syntax-highlight palette: a deliberate per-category color scheme,
 * intentionally NOT theme tokens (theme.h bucket 3 - it is its own
 * domain palette and must stay legible/distinct independent of the UI
 * accent). k_syntax_shade below is the matching constant/var/literal
 * shade ramp; same rationale. */
static const struct { float r, g, b; } k_category_colors[CMD_CAT_COUNT] = {
    [CMD_CAT_DEFAULT]     = { 0.70f, 0.70f, 0.70f },
    [CMD_CAT_PRIMITIVE]   = { 0.85f, 0.45f, 0.85f },
    [CMD_CAT_VERTEX]      = { 0.40f, 0.90f, 0.40f },
    [CMD_CAT_NORMAL]      = { 0.40f, 0.80f, 0.95f },
    [CMD_CAT_COLOR]       = { 0.95f, 0.85f, 0.30f },
    [CMD_CAT_TRANSFORM]   = { 0.95f, 0.65f, 0.40f },
    [CMD_CAT_STATE]       = { 0.80f, 0.70f, 0.95f },
    [CMD_CAT_LOOP]        = { 0.95f, 0.60f, 0.30f },
    [CMD_CAT_FUNCTION]    = { 0.60f, 0.85f, 0.95f },
    [CMD_CAT_VARIABLE]    = { 0.55f, 0.80f, 0.95f },
    [CMD_CAT_CONDITIONAL] = { 0.95f, 0.75f, 0.50f },
    [CMD_CAT_COMMENT]     = { 0.45f, 0.50f, 0.45f },
    [CMD_CAT_GLUT_SHAPE]  = { 0.50f, 0.90f, 0.70f },
    [CMD_CAT_TESS_BLOCK]  = { 0.70f, 0.55f, 0.90f },
};

/* Per-attribute-bit colours for the glPushAttrib/glPopAttrib highlighting,
 * indexed by canonical bit index (repl_attrib_bit_entries order). Eleven
 * visually distinct hues used for both the mask-token spans on the push line
 * and the segmented left-edge gutter marker on each saved/reverted setter line.
 * GL_FOG_BIT is the one desaturated ("haze") entry, keeping it distinct from
 * the ten saturated hues around it. Kept clear of the reserved marker colours:
 * warning-red (0.95,0.35,0.30), replay-green (~0.20,0.90,0.30), and the violet
 * bracket match (0.80,0.70,0.95). */
static const struct { float r, g, b; } k_attrib_bit_colors[REPL_ATTRIB_BIT_COUNT] = {
    { 0.95f, 0.75f, 0.20f },  /* 0 GL_CURRENT_BIT        - gold      */
    { 0.55f, 0.80f, 0.30f },  /* 1 GL_POINT_BIT          - lime      */
    { 0.30f, 0.75f, 0.90f },  /* 2 GL_LINE_BIT           - sky       */
    { 0.90f, 0.55f, 0.30f },  /* 3 GL_POLYGON_BIT        - orange    */
    { 0.95f, 0.90f, 0.45f },  /* 4 GL_LIGHTING_BIT       - yellow    */
    { 0.72f, 0.75f, 0.78f },  /* 5 GL_FOG_BIT            - haze grey */
    { 0.45f, 0.60f, 0.95f },  /* 6 GL_DEPTH_BUFFER_BIT   - blue      */
    { 0.85f, 0.45f, 0.85f },  /* 7 GL_STENCIL_BUFFER_BIT - orchid    */
    { 0.60f, 0.45f, 0.90f },  /* 8 GL_TRANSFORM_BIT      - indigo    */
    { 0.40f, 0.85f, 0.70f },  /* 9 GL_ENABLE_BIT         - teal      */
    { 0.90f, 0.55f, 0.65f },  /* 10 GL_COLOR_BUFFER_BIT  - rose      */
};

typedef struct {
    const UiRenderSnapshot *snap;
    UiTextPanelSnapshot     text_snap;
    int                     row_count;
    int                     generated_count;
    int                     file_line;
    int                     highlight_normal_idx;
    int                     highlight_color_idx;
    int                     highlight_tutorial_insertion_idx;
    int                     highlight_matching_push_idx;
    int                     highlight_replay_call_site_idx;
    int                     highlight_replay_root_call_site_idx;
} ReplCodePanelBuilder;

static const char *repl_code_panel_display_text(const UiRenderSnapshot *snap, int line_idx);

static struct {
    const UiRenderSnapshot *snap;
    ReplCodePanelBuilder    builder;
    int                     valid;
} g_builder_cache;

static UiTextPanelColor repl_code_panel_rgb(float r, float g, float b) {
    UiTextPanelColor color = { r, g, b, 1.0f, 0 };
    return color;
}

static UiTextPanelColor repl_code_panel_rgba(float r, float g, float b, float a) {
    UiTextPanelColor color = { r, g, b, a, 1 };
    return color;
}

/* Line-range selection band (shift+up/down or shift+click selecting
 * whole rows). Deliberately the muted blue from the editor sub-palette
 * - distinct from text_panel.c's brighter k_clr_selection_band, which
 * marks the in-input character-range selection. The two highlights
 * coexist in the same panel; keeping them visually distinct makes
 * it clear which selection mode is active. */
static const float k_clr_line_selection_band[4] = { 0.20f, 0.30f, 0.50f, 0.55f };

static UiTextPanelColor repl_code_panel_scaled_alpha(UiTextPanelColor base,
                                                     float alpha) {
    float base_alpha = base.has_alpha ? base.a : 1.0f;
    base.a = base_alpha * alpha;
    base.has_alpha = 1;
    return base;
}

static const UiTransformer *repl_code_panel_find_color_transformer(
    const UiTransformerList *list, int line_idx) {
    if (!list)
        return NULL;
    for (int i = 0; i < list->count; i++) {
        const UiTransformer *transformer = &list->items[i];
        if (transformer->kind == TRANSFORMER_COLOR_PICKER &&
            transformer->line_idx == line_idx)
            return transformer;
    }
    return NULL;
}

static void repl_code_panel_category_rgb(CmdSyntaxCategory cat,
                                         float *r, float *g, float *b) {
    if (cat < 0 || cat >= CMD_CAT_COUNT)
        cat = CMD_CAT_DEFAULT;
    if (r) *r = k_category_colors[cat].r;
    if (g) *g = k_category_colors[cat].g;
    if (b) *b = k_category_colors[cat].b;
}

static UiTextPanelColor repl_code_panel_category_color(CmdType type) {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    repl_code_panel_category_rgb(repl_cmd_type_category(type), &r, &g, &b);
    return repl_code_panel_rgb(r, g, b);
}



static CodeLayout repl_code_panel_text_layout(const UiRenderSnapshot *snap,
                                              int panel_w, int first_x) {
    return code_layout_make(panel_w, first_x, FONT_W,
                            snap ? snap->code_panel.wrap_at_comma : 1);
}

static int repl_code_panel_row_count(const UiRenderSnapshot *snap,
                                     const char *text,
                                     int first_x, int panel_w) {
    CodeLayout layout = repl_code_panel_text_layout(snap, panel_w, first_x);
    return code_layout_row_count_for_text(text, &layout);
}

static int repl_code_panel_cursor_row(const UiRenderSnapshot *snap,
                                      const char *text,
                                      int first_x, int panel_w,
                                      int cursor, int *out_start,
                                      int *out_len, int *out_x) {
    CodeLayout layout = repl_code_panel_text_layout(snap, panel_w, first_x);
    return code_layout_cursor_row_for_text(text, &layout, cursor,
                                           out_start, out_len, out_x);
}

/* "Code focus" hides the derived C boilerplate stanzas (workspace header,
 * includes, render-state/camera/lights setup, init()/reshape() footer). The
 * display() framing is emitted separately in both modes. Gating the remaining
 * row-count and emit paths with this predicate keeps header_rows/footer_rows
 * consistent with what build_rows actually emits. */
static int repl_code_panel_chrome_visible(const UiRenderSnapshot *snap) {
    return !(snap && snap->code_panel.code_focus);
}

/* File-scope function headers are stored in the editable REPL spelling
 * (`func0() {`) but full-chrome mode projects the C return type the same way
 * it projects `void display(void) {`. A function definition left at or below
 * the body boundary stays inside display() and keeps its indented REPL
 * spelling; prefixing that row would render invalid-looking nested C. Keep
 * the synthetic prefix as row metadata so every path that crosses back into
 * editor coordinates can remove it explicitly. */
static int repl_code_panel_func_return_prefix_chars(
        const UiRenderSnapshot *snap, int line_idx) {
    if (!repl_code_panel_chrome_visible(snap) || !snap ||
        line_idx < 0 || line_idx >= snap->document_count ||
        line_idx >= snap->display_body_start)
        return 0;
    return snap->document_cmds[line_idx].valid &&
           snap->document_cmds[line_idx].type == CMD_FUNC_DEF
               ? (int)(sizeof(REPL_CODE_PANEL_FUNC_RETURN_PREFIX) - 1)
               : 0;
}

static const char *repl_code_panel_add_func_return_prefix(
        const UiRenderSnapshot *snap, int line_idx, const char *source,
        char *out, size_t out_sz) {
    if (!source)
        source = "";
    if (repl_code_panel_func_return_prefix_chars(snap, line_idx) == 0 ||
        !out || out_sz == 0)
        return source;
    snprintf(out, out_sz, "%s%s", REPL_CODE_PANEL_FUNC_RETURN_PREFIX, source);
    return out;
}

/* FILE-SCOPE chrome: everything that precedes the first document row -
 * workspace header, includes, the GL vector helpers, and the scratch
 * decoration line. The declarations and function definitions the document
 * opens with render after this and still ABOVE `void display(void) {`,
 * which is spliced in later at the display-body boundary. */
static int repl_code_panel_header_row_count(const UiRenderSnapshot *snap,
                                            int panel_w, int text_x) {
    if (!repl_code_panel_chrome_visible(snap))
        return 0;
    ReplImportExportView import_export = snap->import_export;
    int rows = 0;

    for (int i = 0; i < import_export.workspace_header_line_count; i++)
        rows += repl_code_panel_row_count(snap,
                                          import_export.workspace_header_lines[i],
                                          text_x, panel_w);
    for (int i = 0; g_header_pre[i]; i++) {
        if (repl_export_header_pre_line_visible(
                i, snap->math_collision_mask))
            rows += repl_code_panel_row_count(
                snap, g_header_pre[i], text_x, panel_w);
    }
    for (int i = 0; i < snap->gl_vector_helper_line_count; i++)
        rows += repl_code_panel_row_count(
            snap, snap->gl_vector_helper_lines[i], text_x, panel_w);
    rows += repl_code_panel_row_count(snap, REPL_CODE_PANEL_SCRATCH_DECL_LINE,
                                      text_x, panel_w);
    return rows;
}

/* Focus mode drops the C return type and parameter list from the display()
 * opener: what frames the user's code is the name and the brace. */
static const char *repl_code_panel_display_open_line(
        const UiRenderSnapshot *snap) {
    return repl_code_panel_chrome_visible(snap)
               ? g_display_header[0]
               : REPL_EXPORT_DISPLAY_OPEN_FOCUS_LINE;
}

/* Whether a blank row separates the file-scope prologue from the frame.
 * There is a prologue exactly when the boundary is past row 0. */
static int repl_code_panel_display_open_spacer(const UiRenderSnapshot *snap) {
    return snap && snap->display_body_start > 0;
}

/* DISPLAY-OPEN chrome, spliced at the display-body boundary: the
 * `void display(void) {` line plus the generated setup that runs before
 * the user's first body row.
 *
 * g_display_header[0] is the frame and is drawn unconditionally - code
 * focus hides what is INSIDE display(), never the line that says the body
 * is inside it. Everything after it is ordinary chrome. */
static int repl_code_panel_display_open_row_count(const UiRenderSnapshot *snap,
                                                  int panel_w, int text_x) {
    ReplImportExportView import_export = snap->import_export;
    int rows = 0;

    /* Blank spacer between the file-scope prologue and the frame, so the
     * last function's `}` does not butt up against `display() {`. Only
     * when there IS a prologue - a document that opens straight into body
     * code has nothing to separate from. */
    if (repl_code_panel_display_open_spacer(snap))
        rows += repl_code_panel_row_count(snap, "", text_x, panel_w);

    rows += repl_code_panel_row_count(
        snap, repl_code_panel_display_open_line(snap), text_x, panel_w);

    if (!repl_code_panel_chrome_visible(snap))
        return rows;

    for (int i = 1; g_display_header[i]; i++)
        rows += repl_code_panel_row_count(snap, g_display_header[i], text_x, panel_w);
    for (int i = 0; i < snap->lights_pre_camera_count; i++)
        rows += repl_code_panel_row_count(
            snap, snap->lights_pre_camera_lines[i], text_x, panel_w);
    /* The spin slot is empty in the panel's hook-less projection; an empty
     * slot draws no row, so it must not be counted as one either. */
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++)
        if (import_export.cam_lines[i][0])
            rows += repl_code_panel_row_count(snap,
                                              import_export.cam_lines[i],
                                              text_x, panel_w);
    for (int i = 0; i < snap->lights_display_count; i++)
        rows += repl_code_panel_row_count(snap, snap->lights_display_lines[i],
                                          text_x, panel_w);
    for (int i = 0; g_header_post[i]; i++)
        rows += repl_code_panel_row_count(snap, g_header_post[i], text_x, panel_w);
    return rows;
}

/* DISPLAY-CLOSE chrome, emitted after the trailing document row. The
 * closing `}` is the other half of the frame and is drawn unconditionally;
 * glPopAttrib/glutSwapBuffers above it are chrome. */
static int repl_code_panel_display_close_row_count(const UiRenderSnapshot *snap,
                                                   int panel_w, int text_x) {
    int rows = 0;

    if (repl_code_panel_chrome_visible(snap)) {
        for (int i = 0; g_display_footer[i]; i++)
            rows += repl_code_panel_row_count(snap, g_display_footer[i],
                                              text_x, panel_w);
        return rows;
    }
    return repl_code_panel_row_count(snap, REPL_EXPORT_DISPLAY_CLOSE_LINE,
                                     text_x, panel_w);
}

static int repl_code_panel_footer_row_count(const UiRenderSnapshot *snap,
                                            int panel_w, int text_x) {
    int rows = 0;

    if (!repl_code_panel_chrome_visible(snap))
        return 0;

    for (int i = 0; g_footer_pre_init[i]; i++) {
        if (strcmp(g_footer_pre_init[i],
                   REPL_EXPORT_RESHAPE_PROJ_SENTINEL) == 0) {
            /* Read the frame-frozen block from the snapshot - never
             * re-resolve live here, or this pass and the render pass
             * (opposite sides of render3d_draw_scene) could disagree
             * on row count across a 2D/3D transition. */
            for (int j = 0; j < snap->reshape_proj_count; j++)
                rows += repl_code_panel_row_count(snap, snap->reshape_proj_lines[j],
                                                  text_x, panel_w);
            continue;
        }
        rows += repl_code_panel_row_count(snap, g_footer_pre_init[i], text_x, panel_w);
    }
    for (int i = 0; i < snap->init_section_count; i++) {
        rows += repl_code_panel_row_count(snap, snap->init_section_lines[i], text_x, panel_w);
    }
    for (int i = 0; g_footer_post_init[i]; i++)
        rows += repl_code_panel_row_count(snap, g_footer_post_init[i], text_x, panel_w);
    return rows;
}

static int repl_code_panel_leading_ws_chars(const char *text) {
    int count = 0;

    while (text && text[count] && isspace((unsigned char)text[count]))
        count++;
    return count;
}

static int repl_code_panel_input_first_x(const UiRenderSnapshot *snap,
                                         int text_x) {
    return text_x + snap->active_indent_chars * FONT_W;
}

static int repl_code_panel_current_input_rows(const UiRenderSnapshot *snap,
                                              int panel_w, int text_x) {
    char decorated[MAX_LINE_LEN * 2];
    const char *text = repl_code_panel_add_func_return_prefix(
        snap,
        (!snap->editor_input.insert_mode ? snap->edit_line : -1),
        snap->editor_input.input, decorated, sizeof(decorated));
    return repl_code_panel_row_count(snap,
                                     text,
                                     repl_code_panel_input_first_x(snap, text_x),
                                     panel_w);
}

static int repl_code_panel_virtual_row_count_for_line(
    const UiRenderSnapshot *snap, int line_idx) {
    int count = 0;

    if (!snap || !snap->editor_virtual_lines)
        return 0;

    for (int i = 0; i < snap->editor_virtual_lines->count; i++) {
        if (snap->editor_virtual_lines->items[i].after_line_idx == line_idx)
            count++;
    }

    return count;
}

/* Shared doc-line math: these helpers turn the precomputed per-command
 * layout counts into either a prefix sum (rows before a command) or the
 * last visible row inside a command block (command rows + replay extras). */
static int repl_code_panel_rows_before_cmd(const int *cmd_main_rows,
                                           const int *replay_extra_rows,
                                           int cmd_limit) {
    int rows = 0;

    for (int i = 0; i < cmd_limit; i++) {
        rows += cmd_main_rows[i];
        rows += replay_extra_rows[i];
    }

    return rows;
}

static int repl_code_panel_last_row_offset_for_cmd(const int *cmd_main_rows,
                                                   const int *replay_extra_rows,
                                                   int cmd_idx) {
    int block_rows = cmd_main_rows[cmd_idx] + replay_extra_rows[cmd_idx];

    return block_rows > 0 ? block_rows - 1 : 0;
}

/* Doc-line lookup has several success exits (insert row, command row,
 * replay virtual row, trailing newline row). Funnel them through one
 * helper so each branch only states which logical row it resolved to. */
static int repl_code_panel_return_target_result(int *out_target,
                                                int *out_on_insert_line,
                                                int *out_row_offset,
                                                int target,
                                                int on_insert_line,
                                                int row_offset) {
    if (out_target)
        *out_target = target;
    if (out_on_insert_line)
        *out_on_insert_line = on_insert_line;
    if (out_row_offset)
        *out_row_offset = row_offset;
    return 1;
}

static int repl_code_panel_command_main_rows(const UiRenderSnapshot *snap,
                                             int cmd_idx, int panel_w,
                                             int text_x) {
    if (!snap->editor_input.insert_mode && cmd_idx == snap->edit_line) {
        return repl_code_panel_current_input_rows(snap, panel_w, text_x);
    }

    {
        char decorated[MAX_LINE_LEN * 2];
        const char *display_text = repl_code_panel_display_text(snap, cmd_idx);
        display_text = repl_code_panel_add_func_return_prefix(
            snap, cmd_idx, display_text, decorated, sizeof(decorated));
        return repl_code_panel_row_count(snap, display_text, text_x, panel_w);
    }
}

static void repl_code_panel_precompute_layout_rows(const UiRenderSnapshot *snap,
                                                   int panel_w, int text_x,
                                                   int *main_rows,
                                                   int *replay_extra_rows) {
    for (int i = 0; i < snap->document_count; i++) {
        if (main_rows)
            main_rows[i] = repl_code_panel_command_main_rows(snap, i, panel_w,
                                                             text_x);
        if (replay_extra_rows)
            replay_extra_rows[i] =
                repl_code_panel_virtual_row_count_for_line(snap, i);
    }
}

static int repl_code_panel_file_scope_slot_row_count(const UiRenderSnapshot *snap,
                                                     int panel_w, int text_x) {
    if (snap->tutorial.active)
        return 0;
    if (snap->editor_input.insert_mode &&
        snap->editor_input.insert_scope == EDITOR_INSERT_FILE_SCOPE) {
        return repl_code_panel_current_input_rows(snap, panel_w, text_x);
    }
    return 1;
}

static int repl_code_panel_insert_rows(const UiRenderSnapshot *snap,
                                       int panel_w, int text_x) {
    return repl_code_panel_current_input_rows(snap, panel_w, text_x);
}

static int repl_code_panel_trailing_row_count(const UiRenderSnapshot *snap,
                                        int panel_w, int text_x) {
    if (snap->edit_line == snap->document_count)
        return repl_code_panel_current_input_rows(snap, panel_w, text_x);
    return 1;
}

/* Rows drawn before document command `cmd_limit`: the file-scope chrome,
 * every earlier command's rows, and - once cmd_limit has reached the
 * display-body boundary - the file-scope slot and spliced display-open chrome. */
static int repl_code_panel_rows_before_cmd_in_layout(
        const UiReplCodePanelLayout *layout, int cmd_limit) {
    return layout->header_rows +
           repl_code_panel_rows_before_cmd(layout->cmd_main_rows,
                                           layout->replay_extra_rows,
                                           cmd_limit) +
           (cmd_limit >= layout->display_open_at
                ? (layout->file_scope_slot_rows + layout->display_open_rows)
                : 0);
}

static int repl_code_panel_cursor_doc_line_from_layout(
    const UiRenderSnapshot *snap,
    const UiReplCodePanelLayout *layout,
    int panel_w, int text_x) {
    if (snap->editor_input.insert_mode &&
        snap->editor_input.insert_scope == EDITOR_INSERT_FILE_SCOPE) {
        int prefix = (snap->display_body_start < snap->document_count)
                         ? snap->display_body_start : snap->document_count;
        return layout->header_rows +
               repl_code_panel_rows_before_cmd(layout->cmd_main_rows,
                                               layout->replay_extra_rows,
                                               prefix) +
               repl_code_panel_cursor_row(snap,
                                          snap->editor_input.input,
                                          repl_code_panel_input_first_x(snap, text_x),
                                          panel_w,
                                          snap->editor_input.cursor_pos,
                                          NULL, NULL, NULL);
    }

    int prefix = (snap->edit_line < snap->document_count)
                     ? snap->edit_line : snap->document_count;
    int display_prefix = repl_code_panel_func_return_prefix_chars(
        snap, !snap->editor_input.insert_mode ? snap->edit_line : -1);
    char decorated[MAX_LINE_LEN * 2];
    const char *input = repl_code_panel_add_func_return_prefix(
        snap, !snap->editor_input.insert_mode ? snap->edit_line : -1,
        snap->editor_input.input, decorated, sizeof(decorated));

    return repl_code_panel_rows_before_cmd_in_layout(layout, prefix) +
           repl_code_panel_cursor_row(snap,
                                      input,
                                      repl_code_panel_input_first_x(snap, text_x),
                                      panel_w,
                                      snap->editor_input.cursor_pos + display_prefix,
                                      NULL, NULL, NULL);
}

static int repl_code_panel_follow_doc_line_from_layout(
    const UiRenderSnapshot *snap,
    int cursor_doc_line, const UiReplCodePanelLayout *layout) {
    int src_line = snap->replay.src_line_idx;

    if (!(snap->replay.active &&
          src_line >= 0 && src_line < snap->document_count))
        return cursor_doc_line;

    return repl_code_panel_rows_before_cmd_in_layout(layout, src_line) +
           repl_code_panel_last_row_offset_for_cmd(layout->cmd_main_rows,
                                                   layout->replay_extra_rows,
                                                   src_line);
}

int ui_repl_code_panel_visible_lines_for_height(int cp_h, int top_chrome_h) {
    return ui_text_panel_visible_lines_for_height(
        cp_h, 22, top_chrome_h);
}

void ui_repl_code_panel_build_layout(const UiRenderSnapshot *snap,
                                     UiReplCodePanelLayout *layout,
                                     int panel_w, int text_x, int cp_h) {
    int total_lines;

    if (!snap || !layout)
        return;

    memset(layout, 0, sizeof(*layout));
    layout->panel_w = panel_w;
    layout->text_x = text_x;
    layout->cp_h = cp_h;
    layout->visible_lines = ui_repl_code_panel_visible_lines_for_height(
        cp_h, ui_scene_tabs_band_h(snap));
    layout->header_rows = repl_code_panel_header_row_count(snap, panel_w, text_x);
    layout->footer_rows = repl_code_panel_footer_row_count(snap, panel_w, text_x);
    layout->file_scope_slot_rows =
        repl_code_panel_file_scope_slot_row_count(snap, panel_w, text_x);
    layout->display_open_rows =
        repl_code_panel_display_open_row_count(snap, panel_w, text_x);
    layout->display_close_rows =
        repl_code_panel_display_close_row_count(snap, panel_w, text_x);
    /* Clamped: a boundary past the last command means the whole body is
     * empty, and the splice lands on the trailing row instead. */
    layout->display_open_at = snap->display_body_start;
    if (layout->display_open_at < 0)
        layout->display_open_at = 0;
    if (layout->display_open_at > snap->document_count)
        layout->display_open_at = snap->document_count;
    repl_code_panel_precompute_layout_rows(snap, panel_w, text_x,
                                           layout->cmd_main_rows,
                                           layout->replay_extra_rows);

    total_lines = layout->header_rows + layout->footer_rows +
                  layout->file_scope_slot_rows +
                  layout->display_open_rows + layout->display_close_rows +
                  repl_code_panel_trailing_row_count(snap, panel_w, text_x);
    for (int i = 0; i < snap->document_count; i++) {
        if (snap->editor_input.insert_mode &&
            snap->editor_input.insert_scope == EDITOR_INSERT_DOCUMENT &&
            i == snap->edit_line)
            total_lines += repl_code_panel_insert_rows(snap, panel_w, text_x);
        total_lines += layout->cmd_main_rows[i];
        total_lines += layout->replay_extra_rows[i];
    }
    layout->total_lines = total_lines;

    layout->cursor_doc_line = repl_code_panel_cursor_doc_line_from_layout(
        snap, layout, panel_w, text_x);
    layout->follow_doc_line = repl_code_panel_follow_doc_line_from_layout(
        snap, layout->cursor_doc_line, layout);
}

int ui_repl_code_panel_display_open_row(const UiReplCodePanelLayout *layout) {
    if (!layout)
        return 0;
    /* rows_before_cmd() at the boundary would already include the spliced
     * chrome; the frame starts just before it. */
    return layout->header_rows +
           repl_code_panel_rows_before_cmd(layout->cmd_main_rows,
                                           layout->replay_extra_rows,
                                           layout->display_open_at) +
           layout->file_scope_slot_rows;
}

int ui_repl_code_panel_rows_before_cmd(const UiReplCodePanelLayout *layout,
                                       int cmd_idx) {
    if (!layout || cmd_idx < 0)
        return 0;
    return repl_code_panel_rows_before_cmd_in_layout(layout, cmd_idx);
}

int ui_repl_code_panel_target_for_doc_line(const UiRenderSnapshot *snap,
                                            int doc_line,
                                            const UiReplCodePanelLayout *layout,
                                            int *out_target,
                                            int *out_on_insert_line,
                                            int *out_row_offset,
                                            EditorInsertScope *out_insert_scope) {
    int row;

    if (!snap || !layout)
        return 0;

    row = doc_line - layout->header_rows;
    if (row < 0)
        return 0;

    /* Consume rows in the same order the panel emits them after the
     * file-scope chrome: the file-scope insert slot, spliced display-open
     * chrome once the boundary is reached, then optional insert row,
     * command body rows, replay virtual rows, then the trailing newline/input slot. */
    for (int cmd_idx = 0; cmd_idx <= snap->document_count; cmd_idx++) {
        if (cmd_idx == layout->display_open_at) {
            if (layout->file_scope_slot_rows > 0) {
                if (row < layout->file_scope_slot_rows) {
                    if (out_insert_scope)
                        *out_insert_scope = EDITOR_INSERT_FILE_SCOPE;
                    return repl_code_panel_return_target_result(out_target,
                                                                out_on_insert_line,
                                                                out_row_offset,
                                                                snap->display_body_start, 1, row);
                }
                row -= layout->file_scope_slot_rows;
            }

            if (row < layout->display_open_rows)
                return 0;   /* generated chrome: not a document target */
            row -= layout->display_open_rows;
        }

        if (snap->editor_input.insert_mode &&
            snap->editor_input.insert_scope == EDITOR_INSERT_DOCUMENT &&
            cmd_idx == snap->edit_line) {
            int insert_rows = repl_code_panel_insert_rows(snap, layout->panel_w,
                                                          layout->text_x);
            if (row < insert_rows) {
                if (out_insert_scope)
                    *out_insert_scope = EDITOR_INSERT_DOCUMENT;
                return repl_code_panel_return_target_result(out_target,
                                                            out_on_insert_line,
                                                            out_row_offset,
                                                            -1, 1, row);
            }
            row -= insert_rows;
        }

        if (cmd_idx < snap->document_count) {
            int main_rows = layout->cmd_main_rows[cmd_idx];
            if (row < main_rows) {
                if (out_insert_scope)
                    *out_insert_scope = EDITOR_INSERT_DOCUMENT;
                return repl_code_panel_return_target_result(out_target,
                                                            out_on_insert_line,
                                                            out_row_offset,
                                                            cmd_idx, 0, row);
            }
            row -= main_rows;

            if (row < layout->replay_extra_rows[cmd_idx]) {
                if (out_insert_scope)
                    *out_insert_scope = EDITOR_INSERT_DOCUMENT;
                return repl_code_panel_return_target_result(out_target,
                                                            out_on_insert_line,
                                                            out_row_offset,
                                                            cmd_idx, 0, 0);
            }
            row -= layout->replay_extra_rows[cmd_idx];
        } else {
            int newline_rows = repl_code_panel_trailing_row_count(snap, layout->panel_w,
                                                            layout->text_x);
            if (row < newline_rows) {
                if (out_insert_scope)
                    *out_insert_scope = EDITOR_INSERT_DOCUMENT;
                return repl_code_panel_return_target_result(out_target,
                                                            out_on_insert_line,
                                                            out_row_offset,
                                                            snap->document_count,
                                                            0, row);
            }
            return 0;
        }
    }

    return 0;
}

static void repl_code_panel_find_highlight_rows(const UiRenderSnapshot *snap,
                                                int *out_normal_idx,
                                                int *out_color_idx,
                                                int *out_tutorial_insertion_idx,
                                                int *out_matching_push_idx,
                                                int *out_replay_call_site_idx,
                                                int *out_replay_root_call_site_idx) {
    int normal_idx = -1;
    int color_idx = -1;
    int tutorial_insertion_idx = -1;
    int matching_push_idx = -1;
    int replay_call_site_idx = -1;
    int replay_root_call_site_idx = -1;

    if (snap && snap->editor_highlights) {
        for (int i = 0; i < snap->editor_highlights->count; i++) {
            const UiHighlight *highlight = &snap->editor_highlights->items[i];
            if (highlight->kind == HIGHLIGHT_FEEDING_NORMAL)
                normal_idx = highlight->line_idx;
            else if (highlight->kind == HIGHLIGHT_FEEDING_COLOR)
                color_idx = highlight->line_idx;
            else if (highlight->kind == HIGHLIGHT_TUTORIAL_INSERTION)
                tutorial_insertion_idx = highlight->line_idx;
            else if (highlight->kind == HIGHLIGHT_MATCHING_PUSH_MATRIX)
                matching_push_idx = highlight->line_idx;
            else if (highlight->kind == HIGHLIGHT_REPLAY_CALL_SITE)
                replay_call_site_idx = highlight->line_idx;
            else if (highlight->kind == HIGHLIGHT_REPLAY_ROOT_CALL_SITE)
                replay_root_call_site_idx = highlight->line_idx;
        }
    }

    if (out_normal_idx) *out_normal_idx = normal_idx;
    if (out_color_idx) *out_color_idx = color_idx;
    if (out_tutorial_insertion_idx) *out_tutorial_insertion_idx = tutorial_insertion_idx;
    if (out_matching_push_idx) *out_matching_push_idx = matching_push_idx;
    if (out_replay_call_site_idx) *out_replay_call_site_idx = replay_call_site_idx;
    if (out_replay_root_call_site_idx)
        *out_replay_root_call_site_idx = replay_root_call_site_idx;
}

/* HIGHLIGHT_AFFECTING_TRANSFORM is multi-entry per frame, so we scan
 * the snapshot's highlight list per-line instead of pre-extracting a
 * single index. List size is bounded (MAX_HIGHLIGHTS=256) and only a
 * handful are typically transform-kind, so the cost is small. */
static int repl_code_panel_line_is_unbalanced(const UiRenderSnapshot *snap,
                                              int line_idx) {
    if (!snap || !snap->editor_highlights || line_idx < 0) return 0;
    for (int i = 0; i < snap->editor_highlights->count; i++) {
        const UiHighlight *h = &snap->editor_highlights->items[i];
        if (h->kind == HIGHLIGHT_UNBALANCED && h->line_idx == line_idx)
            return 1;
    }
    return 0;
}

static int repl_code_panel_line_is_affecting_transform(const UiRenderSnapshot *snap,
                                                       int line_idx) {
    if (!snap || !snap->editor_highlights || line_idx < 0) return 0;
    for (int i = 0; i < snap->editor_highlights->count; i++) {
        const UiHighlight *h = &snap->editor_highlights->items[i];
        if (h->kind == HIGHLIGHT_AFFECTING_TRANSFORM && h->line_idx == line_idx)
            return 1;
    }
    return 0;
}

/* OR of the bit-index masks (aux) over every HIGHLIGHT_ATTRIB_STATE entry on a
 * line - the set of attribute-bit colours the glPushAttrib gutter marker bands
 * for this saved/reverted setter line. 0 when the line carries no attrib
 * highlight. Multi-entry per line (a cell covered by several bits, or several
 * cells on one line), like the unbalanced scanner above. */
static unsigned repl_code_panel_line_attrib_bits(const UiRenderSnapshot *snap,
                                                 int line_idx) {
    unsigned mask = 0;
    if (!snap || !snap->editor_highlights || line_idx < 0) return 0;
    for (int i = 0; i < snap->editor_highlights->count; i++) {
        const UiHighlight *h = &snap->editor_highlights->items[i];
        if (h->kind == HIGHLIGHT_ATTRIB_STATE && h->line_idx == line_idx)
            mask |= (unsigned)h->aux;
    }
    return mask;
}

/* Collect up to UI_TEXT_PANEL_MAX_MARKER_BANDS caller-chain ramp colours for
 * `line_idx`. When a source line carries more than UI_TEXT_PANEL_MAX_MARKER_BANDS
 * (4) chain frames, retain its outermost band plus its three innermost bands
 * and drop only the middle bands from the gutter. Returns the number of bands
 * written to out_colors. */
static int repl_code_panel_line_replay_call_chain_colors(
    const UiRenderSnapshot *snap, int line_idx,
    UiTextPanelColor *out_colors, int max_out) {
    if (!snap || !snap->editor_highlights || line_idx < 0 || !out_colors || max_out <= 0)
        return 0;

    UiTextPanelColor temp_colors[MAX_HIGHLIGHTS];
    int match_count = 0;
    for (int i = 0; i < snap->editor_highlights->count; i++) {
        const UiHighlight *h = &snap->editor_highlights->items[i];
        if (h->kind == HIGHLIGHT_REPLAY_CALL_CHAIN && h->line_idx == line_idx) {
            if (match_count < MAX_HIGHLIGHTS) {
                float r = (float)((h->aux >> 16) & 0xFF) / 255.0f;
                float g = (float)((h->aux >> 8) & 0xFF) / 255.0f;
                float b = (float)(h->aux & 0xFF) / 255.0f;
                temp_colors[match_count++] = repl_code_panel_rgba(r, g, b, 0.90f);
            }
        }
    }
    if (match_count == 0)
        return 0;

    int cap = (max_out < UI_TEXT_PANEL_MAX_MARKER_BANDS) ? max_out : UI_TEXT_PANEL_MAX_MARKER_BANDS;
    if (match_count <= cap) {
        for (int i = 0; i < match_count; i++) {
            out_colors[i] = temp_colors[i];
        }
        return match_count;
    }

    out_colors[0] = temp_colors[0];
    int inner_count = cap - 1;
    for (int i = 0; i < inner_count; i++) {
        out_colors[1 + i] = temp_colors[match_count - inner_count + i];
    }
    return cap;
}

/* The 6-char aux column (vertex indices, the auto-normal tag) is always
 * reserved, and UI_TEXT_PANEL_CHROME_AUX_COL is always set to match - the
 * renderer draws the column at text_x - 6 * FONT_W, so reserving and
 * painting it must never disagree or the labels land on the line-number
 * gutter. It was previously conditional on a `show_vertex_indices` flag
 * that had no GlrConfigKey, no menu row and no binding: nothing outside
 * tests could turn it off, so the conditional layout only ever produced
 * duplicated width arithmetic in test helpers. */
int ui_repl_code_panel_compute_text_x(const UiRenderSnapshot *snap) {
    if (!snap)
        return 0;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = 6 * FONT_W;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    return idx_x + idx_col_w;
}

static int repl_code_panel_init_builder(ReplCodePanelBuilder *builder,
                                        const UiRenderSnapshot *snap) {
    int cp_x;
    int cp_y;
    int cp_w;
    int cp_h;
    const char *input_text;
    int input_prefix;

    if (!builder || !snap)
        return 0;

    memset(builder, 0, sizeof(*builder));
    builder->snap = snap;
    builder->file_line = 1;
    repl_code_panel_find_highlight_rows(snap,
                                        &builder->highlight_normal_idx,
                                        &builder->highlight_color_idx,
                                        &builder->highlight_tutorial_insertion_idx,
                                        &builder->highlight_matching_push_idx,
                                        &builder->highlight_replay_call_site_idx,
                                        &builder->highlight_replay_root_call_site_idx);

    ui_layout_code_panel_rect(&cp_x, &cp_y, &cp_w, &cp_h);
    if (cp_w <= 0 || cp_h <= 0)
        return 0;

    input_text = snap->editor_input.input ? snap->editor_input.input : "";
    input_prefix = repl_code_panel_func_return_prefix_chars(
        snap, !snap->editor_input.insert_mode ? snap->edit_line : -1);
    if (input_prefix > 0 &&
        builder->generated_count < UI_REPL_CODE_PANEL_MAX_GENERATED_TEXT_ROWS) {
        char *decorated =
            g_repl_code_panel_generated_text[builder->generated_count++];
        snprintf(decorated, MAX_LINE_LEN * 2, "%s%s",
                 REPL_CODE_PANEL_FUNC_RETURN_PREFIX, input_text);
        input_text = decorated;
    }

    builder->text_snap = (UiTextPanelSnapshot){
        .vp_w = snap->viewport.window_w,
        .vp_h = snap->viewport.window_h,
        .cp_x = cp_x,
        .cp_y = cp_y,
        .cp_w = cp_w,
        .cp_h = cp_h,
        .text_x = ui_repl_code_panel_compute_text_x(snap),
        .wrap_at_comma = snap->code_panel.wrap_at_comma,
        .comment_rule_ligature = 1,
        .paren_match = snap->code_panel.paren_match,
        .paren_scope = snap->code_panel.paren_scope,
        .top_chrome_h = ui_scene_tabs_band_h(snap),
        .statusbar_h = 22,
        .rows = g_repl_code_panel_rows,
        .row_count = 0,
        .scroll = snap->scroll.scroll,
        .scrollbar_drag = snap->code_panel.scrollbar_drag,
        .chrome_flags = UI_TEXT_PANEL_CHROME_STATUSBAR |
                        UI_TEXT_PANEL_CHROME_SCROLLBAR |
                        UI_TEXT_PANEL_CHROME_LINE_NUMS |
                        UI_TEXT_PANEL_CHROME_AUX_COL,
        .input = {
            .input = input_text,
            .input_len = snap->editor_input.input_len >= 0
                             ? snap->editor_input.input_len + input_prefix
                             : (int)strlen(input_text),
            .cursor = snap->editor_input.cursor_pos + input_prefix,
            .anchor = snap->editor_input.anchor_pos >= 0
                          ? snap->editor_input.anchor_pos + input_prefix
                          : -1,
            .ghost = snap->autocomplete.ghost,
            .hint = snap->autocomplete.hint,
            .cursor_visible = snap->cursor_blink.cursor_visible,
        },
        .search = {
            .active = snap->search.active,
            .query = snap->search.query,
            .query_len = snap->search.query_len,
            .hit_row = snap->search.hit_line_idx,
            .hit_char = snap->search.hit_char_idx,
            .whole_word = snap->search.whole_word,
        },
    };

    return 1;
}

static UiTextPanelRow *repl_code_panel_push_row(ReplCodePanelBuilder *builder) {
    UiTextPanelRow *row;

    if (!builder || builder->row_count >= UI_REPL_CODE_PANEL_MAX_ROWS)
        return NULL;

    row = &g_repl_code_panel_rows[builder->row_count++];
    memset(row, 0, sizeof(*row));
    row->source_line_idx = -1;
    row->hit_target_line_idx = -1;
    row->search_row_idx = -1;
    return row;
}

static char *repl_code_panel_next_generated_text(ReplCodePanelBuilder *builder) {
    if (!builder ||
        builder->generated_count >= UI_REPL_CODE_PANEL_MAX_GENERATED_TEXT_ROWS)
        return NULL;
    return g_repl_code_panel_generated_text[builder->generated_count++];
}

static const char *repl_code_panel_display_text(const UiRenderSnapshot *snap, int line_idx) {
    if (line_idx < 0)
        return "";
    for (int i = 0; i < snap->line_overrides.count; i++) {
        if (snap->line_overrides.items[i].line_idx == line_idx)
            return snap->line_overrides.items[i].text;
    }
    const char *display_text = editor_buffer_view_line(snap->editor_buffer, line_idx);
    return display_text ? display_text : "";
}

static int repl_code_panel_search_row_for_cmd(const UiRenderSnapshot *snap,
                                              int line_idx) {
    if (!snap || line_idx < 0 || line_idx >= snap->document_count)
        return -1;
    if (snap->editor_input.insert_mode && line_idx >= snap->edit_line)
        return line_idx + 1;
    return line_idx;
}

static int repl_code_panel_line_in_current_begin_block(const UiRenderSnapshot *snap,
                                                       int line_idx) {
    return snap &&
           snap->current_begin_block_valid &&
           line_idx >= snap->current_begin_block_start &&
           line_idx <= snap->current_begin_block_end;
}

static void repl_code_panel_set_vertex_label(UiTextPanelRow *row,
                                             const UiRenderSnapshot *snap,
                                             int line_idx,
                                             int is_vertex, int vnum,
                                             int primitive_vnums_exact) {
    if (!row || !snap || !is_vertex)
        return;
    if (!repl_code_panel_line_in_current_begin_block(snap, line_idx))
        return;

    snprintf(row->left_aux_label, sizeof(row->left_aux_label),
             primitive_vnums_exact ? "v%d" : "vn", vnum);
}

/* Auto-generated rows (synthesized normals) read as machine output, not as
 * something the user typed. `is_auto` already rides on the document command
 * the panel walks, so the whole signal is render-time: nothing is written
 * into the source text, which the autonormal pass rewrites wholesale on
 * every recompute.
 *
 * The label is the unambiguous channel; the dim is reinforcement. The aux
 * column is shared with the v0/v1 vertex indices, but an auto normal is
 * never a vertex row, so the two never contend for it. */
#define REPL_CODE_PANEL_AUTO_ROW_ALPHA 0.8f

/* The label is pure chrome - it names the mechanism once the reader has
 * learned the dim, and then repeats down the block - so it is tuned
 * independently of, and further back than, the row text it annotates. */
#define REPL_CODE_PANEL_AUTO_LABEL_ALPHA 0.30f

static int repl_code_panel_line_is_auto(const UiRenderSnapshot *snap,
                                        int line_idx) {
    const GLCmd *cmd;

    if (!snap || line_idx < 0 || line_idx >= snap->document_count)
        return 0;
    cmd = &snap->document_cmds[line_idx];
    return cmd->valid && cmd->is_auto;
}

static void repl_code_panel_set_auto_label(UiTextPanelRow *row,
                                           const UiRenderSnapshot *snap,
                                           int line_idx) {
    if (!row || !snap)
        return;
    if (!repl_code_panel_line_is_auto(snap, line_idx))
        return;

    snprintf(row->left_aux_label, sizeof(row->left_aux_label), "auto");
    row->left_aux_alpha = REPL_CODE_PANEL_AUTO_LABEL_ALPHA;
}

/* Runs after every segment pass: the syntax / trailing-comment / attrib-bit
 * passes write their own per-character colours, so dimming row->color alone
 * would leave a highlighted row at full brightness. */
static void repl_code_panel_apply_auto_dim(UiTextPanelRow *row,
                                           const UiRenderSnapshot *snap,
                                           int line_idx) {
    if (!row || !repl_code_panel_line_is_auto(snap, line_idx))
        return;

    row->color = repl_code_panel_scaled_alpha(row->color,
                                              REPL_CODE_PANEL_AUTO_ROW_ALPHA);
    for (int i = 0; i < row->color_segment_count; i++)
        row->color_segments[i].color = repl_code_panel_scaled_alpha(
            row->color_segments[i].color, REPL_CODE_PANEL_AUTO_ROW_ALPHA);
}

static void repl_code_panel_set_right_action(UiTextPanelRow *row,
                                             const UiRenderSnapshot *snap,
                                             int line_idx) {
    const UiTransformer *transformer;

    if (!row || !snap)
        return;

    transformer = repl_code_panel_find_color_transformer(snap->editor_transformers,
                                                         line_idx);
    if (!transformer)
        return;

    row->right_action.active = 1;
    row->right_action.color = transformer->state.color.has_alpha
        ? repl_code_panel_rgba(transformer->state.color.r,
                               transformer->state.color.g,
                               transformer->state.color.b,
                               transformer->state.color.a)
        : repl_code_panel_rgb(transformer->state.color.r,
                              transformer->state.color.g,
                              transformer->state.color.b);
    row->right_action.emphasized =
        snap->color_picker.active_line == transformer->line_idx;
}

static void repl_code_panel_apply_tutorial_insertion_marker(
    ReplCodePanelBuilder *builder, int target_line_idx, UiTextPanelRow *row) {
    if (!builder || !row || target_line_idx < 0)
        return;
    if (target_line_idx != builder->highlight_tutorial_insertion_idx)
        return;
    row->left_marker_active = 1;
    row->left_marker_color = repl_code_panel_rgba(0.95f, 0.45f, 0.85f, 0.90f);
}

typedef enum {
    MARKER_PRIORITY_NONE = 0,
    MARKER_PRIORITY_REPLAY,
    MARKER_PRIORITY_REPLAY_ROOT_CALL_SITE,
    MARKER_PRIORITY_REPLAY_CALL_SITE,
    MARKER_PRIORITY_REPLAY_CALL_CHAIN,
    MARKER_PRIORITY_MATCHING_PUSH,
    MARKER_PRIORITY_FEEDING_NORMAL,
    MARKER_PRIORITY_FEEDING_COLOR,
    /* Above the feeding markers: the affecting-transform set now resolves
     * through the flat program and can land on a line that also carries a
     * feeding-color/normal marker for a different cursor query, so it must
     * win rather than be masked (req 4). */
    MARKER_PRIORITY_AFFECTING_TRANSFORM,
    /* glPushAttrib per-bit saved/reverted setter marker. Above affecting-
     * transform (it is a distinct cursor query) and below unbalanced (a
     * structural error still wins the gutter). Draws a segmented multi-colour
     * band rather than a single colour. */
    MARKER_PRIORITY_ATTRIB_STATE,
    MARKER_PRIORITY_UNBALANCED,
    MARKER_PRIORITY_TUTORIAL_INSERTION
} MarkerPriority;

/* Decorate one code-panel row with its background band (replay PC /
 * line selection) and at most one left-edge marker. Every marker
 * source is tested independently against the snapshot, and ties
 * resolve through the MarkerPriority ladder above - a sequence of
 * "claim if higher priority" checks rather than nested exclusions, so
 * adding a marker kind is one enum entry plus one block here. */
static void repl_code_panel_apply_command_overlays(ReplCodePanelBuilder *builder,
                                                   int line_idx,
                                                   UiTextPanelRow *row) {
    if (!builder || !row || !builder->snap)
        return;

    if (builder->snap->replay.active &&
        builder->snap->replay.src_line_idx >= 0 &&
        line_idx == builder->snap->replay.src_line_idx) {
        row->background_active = 1;
        row->background_color = repl_code_panel_rgba(0.10f, 0.35f, 0.15f, 0.55f);
    }

    if (builder->snap->selection_active &&
        line_idx >= builder->snap->selection_lo &&
        line_idx <= builder->snap->selection_hi) {
        row->background_active = 1;
        row->background_color = repl_code_panel_rgba(
            k_clr_line_selection_band[0], k_clr_line_selection_band[1],
            k_clr_line_selection_band[2], k_clr_line_selection_band[3]);
    }

    MarkerPriority priority = MARKER_PRIORITY_NONE;
    UiTextPanelColor color = {0};

    if (builder->snap->replay.active &&
        builder->snap->replay.src_line_idx >= 0 &&
        line_idx == builder->snap->replay.src_line_idx) {
        priority = MARKER_PRIORITY_REPLAY;
        color = repl_code_panel_rgba(0.20f, 0.90f, 0.30f, 0.85f);
    }

    /* Replay call-site markers (cyan family) - the funcN(...) line(s) whose
     * expansion is currently executing. Distinct from the green PC marker so a
     * reused/recursive function shows which invocation is live. Immediate
     * caller (brighter) outranks the root caller of a nested chain (dimmer). */
    if (line_idx >= 0 && line_idx == builder->highlight_replay_root_call_site_idx) {
        if (MARKER_PRIORITY_REPLAY_ROOT_CALL_SITE > priority) {
            priority = MARKER_PRIORITY_REPLAY_ROOT_CALL_SITE;
            color = repl_code_panel_rgba(0.20f, 0.55f, 0.70f, 0.85f);
        }
    }

    if (line_idx >= 0 && line_idx == builder->highlight_replay_call_site_idx) {
        if (MARKER_PRIORITY_REPLAY_CALL_SITE > priority) {
            priority = MARKER_PRIORITY_REPLAY_CALL_SITE;
            color = repl_code_panel_rgba(0.30f, 0.85f, 0.95f, 0.90f);
        }
    }

    if (repl_code_panel_line_is_affecting_transform(builder->snap, line_idx)) {
        if (MARKER_PRIORITY_AFFECTING_TRANSFORM > priority) {
            priority = MARKER_PRIORITY_AFFECTING_TRANSFORM;
            /* Vivid amber - deliberately redder/more saturated than the pale
             * feeding-color yellow (0.95,0.85,0.30) and clear of the blue
             * feeding-normal marker, so the transform set reads distinctly. */
            color = repl_code_panel_rgba(1.0f, 0.50f, 0.10f, 0.92f);
        }
    }

    if (line_idx >= 0 && line_idx == builder->highlight_matching_push_idx) {
        if (MARKER_PRIORITY_MATCHING_PUSH > priority) {
            priority = MARKER_PRIORITY_MATCHING_PUSH;
            color = repl_code_panel_rgba(0.80f, 0.70f, 0.95f, 0.85f);
        }
    }

    if (line_idx == builder->highlight_normal_idx) {
        if (MARKER_PRIORITY_FEEDING_NORMAL > priority) {
            priority = MARKER_PRIORITY_FEEDING_NORMAL;
            color = repl_code_panel_rgba(0.40f, 0.80f, 0.95f, 0.85f);
        }
    } else if (line_idx == builder->highlight_color_idx) {
        if (MARKER_PRIORITY_FEEDING_COLOR > priority) {
            priority = MARKER_PRIORITY_FEEDING_COLOR;
            color = repl_code_panel_rgba(0.95f, 0.85f, 0.30f, 0.85f);
        }
    }

    unsigned attrib_bits = repl_code_panel_line_attrib_bits(builder->snap, line_idx);
    if (attrib_bits) {
        if (MARKER_PRIORITY_ATTRIB_STATE > priority) {
            priority = MARKER_PRIORITY_ATTRIB_STATE;
        }
    }

    UiTextPanelColor chain_colors[UI_TEXT_PANEL_MAX_MARKER_BANDS];
    int chain_band_count = repl_code_panel_line_replay_call_chain_colors(
        builder->snap, line_idx, chain_colors, UI_TEXT_PANEL_MAX_MARKER_BANDS);
    if (chain_band_count > 0) {
        if (MARKER_PRIORITY_REPLAY_CALL_CHAIN > priority) {
            priority = MARKER_PRIORITY_REPLAY_CALL_CHAIN;
        }
    }

    if (repl_code_panel_line_is_unbalanced(builder->snap, line_idx)) {
        if (MARKER_PRIORITY_UNBALANCED > priority) {
            priority = MARKER_PRIORITY_UNBALANCED;
            /* Warning red - distinct from the violet match / orange
             * affecting-transform / blue-yellow feeding markers. */
            color = repl_code_panel_rgba(0.95f, 0.35f, 0.30f, 0.95f);
        }
    }

    if (line_idx >= 0 && line_idx == builder->highlight_tutorial_insertion_idx) {
        if (MARKER_PRIORITY_TUTORIAL_INSERTION > priority) {
            priority = MARKER_PRIORITY_TUTORIAL_INSERTION;
            color = repl_code_panel_rgba(0.95f, 0.45f, 0.85f, 0.90f);
        }
    }

    if (priority == MARKER_PRIORITY_ATTRIB_STATE) {
        /* Segmented gutter marker: one band per covering attribute bit, in
         * canonical order, capped at the band limit. */
        int nb = 0;
        for (int i = 0;
             i < REPL_ATTRIB_BIT_COUNT && nb < UI_TEXT_PANEL_MAX_MARKER_BANDS;
             i++) {
            if (attrib_bits & (1u << i)) {
                row->left_marker_band_colors[nb++] = repl_code_panel_rgba(
                    k_attrib_bit_colors[i].r, k_attrib_bit_colors[i].g,
                    k_attrib_bit_colors[i].b, 0.95f);
            }
        }
        row->left_marker_band_count = nb;
        row->left_marker_active = 1;
    } else if (priority == MARKER_PRIORITY_REPLAY_CALL_CHAIN) {
        for (int i = 0; i < chain_band_count; i++) {
            row->left_marker_band_colors[i] = chain_colors[i];
        }
        row->left_marker_band_count = chain_band_count;
        row->left_marker_active = 1;
    } else if (priority > MARKER_PRIORITY_NONE) {
        row->left_marker_active = 1;
        row->left_marker_color = color;
    }
}

static int repl_code_panel_line_is_fading(const UiRenderSnapshot *snap, int line_idx) {
    return tutorial_fade_line_active(&snap->tutorial_fade, line_idx,
                                     snap->anim_time);
}

static void repl_code_panel_apply_fade_segments(const UiRenderSnapshot *snap,
                                                int line_idx, const char *text,
                                                UiTextPanelRow *row) {
    int line_len;
    int front;
    int settled_end;
    int i;
    float base_a;

    if (!row || !text)
        return;

    line_len = (int)strlen(text);
    if (line_len <= 0)
        return;

    front = tutorial_fade_front(&snap->tutorial_fade, line_idx, line_len,
                                snap->anim_time);
    if (front < 0)
        return;

    base_a = row->color.has_alpha ? row->color.a : 1.0f;

    row->color_segment_count = 0;

    /* Chars 0..settled_end-1 have completed the settle phase and sit
     * at the base color. Chars settled_end..front-1 are still easing
     * from bright white toward the base color and each get a
     * dedicated gradient segment. */
    settled_end = front - TUTORIAL_FADE_SETTLE_CHARS;
    if (settled_end < 0)
        settled_end = 0;

    if (settled_end > 0 &&
        row->color_segment_count < UI_TEXT_PANEL_MAX_COLOR_SEGMENTS) {
        row->color_segments[row->color_segment_count++] =
            (UiTextPanelColorSegment){
                .char_start = 0,
                .char_count = settled_end,
                .color = repl_code_panel_scaled_alpha(row->color, 1.0f),
            };
    }

    for (i = settled_end; i < front; i++) {
        float s;
        UiTextPanelColor c;
        if (row->color_segment_count >= UI_TEXT_PANEL_MAX_COLOR_SEGMENTS)
            break;
        s = tutorial_fade_settle(&snap->tutorial_fade, line_idx, i, line_len,
                                 snap->anim_time);
        c.r = 1.0f + (row->color.r - 1.0f) * s;
        c.g = 1.0f + (row->color.g - 1.0f) * s;
        c.b = 1.0f + (row->color.b - 1.0f) * s;
        c.a = base_a;
        c.has_alpha = 1;
        row->color_segments[row->color_segment_count++] =
            (UiTextPanelColorSegment){
                .char_start = i,
                .char_count = 1,
                .color = c,
            };
    }

    /* The actively-revealing head character: bright white, alpha
     * ramping 0 -> 1 across its slot. */
    if (front < line_len &&
        row->color_segment_count < UI_TEXT_PANEL_MAX_COLOR_SEGMENTS) {
        UiTextPanelColor highlight = { 1.0f, 1.0f, 1.0f, base_a, 1 };
        row->color_segments[row->color_segment_count++] =
            (UiTextPanelColorSegment){
                .char_start = front,
                .char_count = 1,
                .color = repl_code_panel_scaled_alpha(
                    highlight,
                    tutorial_fade_alpha(&snap->tutorial_fade, line_idx, front,
                                        line_len, snap->anim_time)),
            };
    }

    /* Not-yet-revealed tail. */
    if (front + 1 < line_len &&
        row->color_segment_count < UI_TEXT_PANEL_MAX_COLOR_SEGMENTS) {
        row->color_segments[row->color_segment_count++] =
            (UiTextPanelColorSegment){
                .char_start = front + 1,
                .char_count = line_len - (front + 1),
                .color = repl_code_panel_scaled_alpha(row->color, 0.0f),
            };
    }
}

/* -------------------------------------------------------------------------
 * Per-kind argument syntax coloring
 * ---------------------------------------------------------------------- */

/* Argument shades are derived from the command's class color *only* - no
 * independent per-kind hue (that produced cross-hue clashes / a rainbow).
 * The keyword keeps the full class color; the other kinds are separate
 * {brightness, saturation} tiers of that same hue, so the class stays
 * recognizable and a line never clashes with itself. Kinds differ only in
 * brightness and saturation, not hue. The constant tier sits just below the
 * keyword; variables and literals are boosted above 1.0 (clamped toward
 * white) so they read as the brightest tokens on the line. Tuned against the
 * (0.06,0.06,0.10) panel background and guarded by test_repl_code_panel_syntax's
 * contrast check. */
static const float k_syntax_shade[REPL_SYNTAX_KIND_COUNT][2] = {
    [REPL_SYNTAX_CONSTANT] = { 0.90f, 0.72f },  /* vivid-ish, slightly dim */
    [REPL_SYNTAX_VARIABLE] = { 1.38f, 1.52f },  /* boosted, brighter than keyword */
    [REPL_SYNTAX_LITERAL]  = { 2.85f, 2.97f },  /* most boosted + brightest */
};

static float repl_clamp01(float v) {
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

void ui_repl_code_panel_syntax_kind_rgb(UiSyntaxKind kind,
                                        CmdSyntaxCategory category,
                                        float out_rgb[3]) {
    float cr = 0.0f;
    float cg = 0.0f;
    float cb = 0.0f;
    float lum;
    float bright;
    float sat;

    if (!out_rgb)
        return;
    if (kind < 0 || kind >= REPL_SYNTAX_KIND_COUNT)
        kind = REPL_SYNTAX_VARIABLE;

    repl_code_panel_category_rgb(category, &cr, &cg, &cb);
    bright = k_syntax_shade[kind][0];
    sat = k_syntax_shade[kind][1];

    /* Saturation about the class color's own luminance keeps the hue fixed
     * (sat only moves the channels toward/away from that gray), then a
     * uniform brightness scale picks the tier. */
    lum = 0.30f * cr + 0.59f * cg + 0.11f * cb;
    out_rgb[0] = repl_clamp01((lum + (cr - lum) * sat) * bright);
    out_rgb[1] = repl_clamp01((lum + (cg - lum) * sat) * bright);
    out_rgb[2] = repl_clamp01((lum + (cb - lum) * sat) * bright);
}

static int repl_syntax_is_ident_start(int c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int repl_syntax_is_ident_char(int c) {
    return isalnum((unsigned char)c) || c == '_';
}

static int repl_syntax_is_c_keyword(const char *name) {
    static const char *const keywords[] = {
        "auto", "break", "case", "char", "const", "continue",
        "default", "defined", "do", "double", "else", "enum",
        "extern", "float", "for", "goto", "if", "inline", "int",
        "long", "register", "restrict", "return", "short", "signed",
        "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while",
        "define", "elif", "endif", "error", "ifdef", "ifndef",
        "include", "pragma", "undef",
        "GLenum", "GLboolean", "GLbitfield", "GLbyte", "GLshort",
        "GLint", "GLsizei", "GLubyte", "GLushort", "GLuint",
        "GLfloat", "GLclampf", "GLdouble", "GLclampd",
        "GLUtesselator", "size_t", "va_list",
        NULL
    };

    for (int i = 0; keywords[i]; i++)
        if (strcmp(name, keywords[i]) == 0)
            return 1;
    return 0;
}

static int repl_syntax_is_macro_constant(const char *name) {
    int has_alpha = 0;

    if (!name || !name[0])
        return 0;
    if (strcmp(name, "NULL") == 0 || strcmp(name, "M_PI") == 0)
        return 1;
    for (int i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (islower(c))
            return 0;
        if (isupper(c))
            has_alpha = 1;
        else if (!isdigit(c) && c != '_')
            return 0;
    }
    return has_alpha;
}

static int repl_code_panel_is_predef(const UiRenderSnapshot *snap, const char *name) {
    if (snap) {
        for (int v = 0; v < snap->variable_panel_vars.count; v++) {
            if (strcmp(snap->variable_panel_vars.vars[v].name, name) == 0)
                return 1;
        }
        return 0;
    }
    return repl_eval_find_predef_var_idx(name) >= 0;
}

int ui_repl_code_panel_classify_syntax(const UiRenderSnapshot *snap,
                                       const char *text,
                                       UiSyntaxSpan *out, int max_spans) {
    int n = 0;
    int i = 0;

    if (!text || !out || max_spans <= 0)
        return 0;

    while (text[i] && n < max_spans) {
        unsigned char c = (unsigned char)text[i];

        /* A top-level line comment ends syntax classification: the
         * trailing `// ...` is colored as a comment by
         * repl_code_panel_apply_trailing_comment_segment, and its words
         * must not be syntax-classified (e.g. a `max` in the comment
         * must not turn into a function color). Strings are consumed
         * atomically below, so a '//' reached here is outside any
         * string literal. */
        if (c == '/' && text[i + 1] == '/')
            break;

        /* Quoted string/character: the whole run (including quotes) is one
         * literal. Generated C adds character constants to the REPL's string
         * vocabulary, so recognize both delimiters here. */
        if (c == '"' || c == '\'') {
            int start = i;
            int quote = c;

            i++;
            while (text[i] && text[i] != quote) {
                if (text[i] == '\\' && text[i + 1])
                    i++;
                i++;
            }
            if (text[i] == quote)
                i++;
            out[n++] = (UiSyntaxSpan){ start, i - start, REPL_SYNTAX_LITERAL };
            continue;
        }

        /* Numeric literal: digits / '.' / exponent. A leading '.' counts
         * only when a digit follows (otherwise it is structural). */
        if (isdigit(c) ||
            (c == '.' && isdigit((unsigned char)text[i + 1]))) {
            int start = i;

            if (text[i] == '0' &&
                (text[i + 1] == 'x' || text[i + 1] == 'X')) {
                i += 2;
                while (isxdigit((unsigned char)text[i]))
                    i++;
            } else {
                while (text[i] &&
                       (isdigit((unsigned char)text[i]) || text[i] == '.'))
                    i++;
                if ((text[i] == 'e' || text[i] == 'E') &&
                    (isdigit((unsigned char)text[i + 1]) ||
                     ((text[i + 1] == '+' || text[i + 1] == '-') &&
                      isdigit((unsigned char)text[i + 2])))) {
                    i++;
                    if (text[i] == '+' || text[i] == '-')
                        i++;
                    while (isdigit((unsigned char)text[i]))
                        i++;
                }
            }
            while (text[i] == 'f' || text[i] == 'F' ||
                   text[i] == 'l' || text[i] == 'L' ||
                   text[i] == 'u' || text[i] == 'U')
                i++;
            out[n++] = (UiSyntaxSpan){ start, i - start, REPL_SYNTAX_LITERAL };
            continue;
        }

        /* Identifier. */
        if (repl_syntax_is_ident_start(c)) {
            int start = i;
            int len;
            int j;
            char name[64];

            while (text[i] && repl_syntax_is_ident_char(text[i]))
                i++;
            len = i - start;

            /* Function-call name (ident immediately followed by '(',
             * skipping spaces) keeps the class color - covers the command
             * keyword, math fns, funcN, and user aliases. */
            j = i;
            while (text[j] == ' ' || text[j] == '\t')
                j++;
            if (text[j] == '(')
                continue;

            if (len >= (int)sizeof(name))
                continue;  /* pathological; leave at class color */
            memcpy(name, text + start, (size_t)len);
            name[len] = '\0';

            if (strcmp(name, "PI") == 0 || strcmp(name, "TAU") == 0 ||
                strncmp(name, "GL_", 3) == 0 ||
                strncmp(name, "GLU_", 4) == 0 ||
                strncmp(name, "GLUT_", 5) == 0) {
                out[n++] = (UiSyntaxSpan){ start, len,
                                             REPL_SYNTAX_CONSTANT };
            } else if (strcmp(name, "t") == 0 ||
                       repl_eval_scratch_array_index(name) >= 0 ||
                       repl_code_panel_is_predef(snap, name)) {
                /* 't' is reserved but is the predefined animation var, so
                 * it must classify as a variable, not as structural. */
                out[n++] = (UiSyntaxSpan){ start, len,
                                             REPL_SYNTAX_VARIABLE };
            } else if (repl_syntax_is_macro_constant(name)) {
                out[n++] = (UiSyntaxSpan){ start, len,
                                             REPL_SYNTAX_CONSTANT };
            } else if (repl_eval_is_reserved_ident(name) ||
                       repl_syntax_is_c_keyword(name)) {
                /* REPL/C keyword, type, or math fn used bare - structural */
            } else {
                /* loop var, funcN param, or otherwise-unknown ident */
                out[n++] = (UiSyntaxSpan){ start, len,
                                             REPL_SYNTAX_VARIABLE };
            }
            continue;
        }

        i++;  /* whitespace / operator / punctuation -> class color */
    }

    return n;
}

static int repl_code_panel_first_call_name(const char *text,
                                           char out[64]) {
    int i = 0;

    if (!text || !out)
        return 0;
    out[0] = '\0';
    while (text[i]) {
        if (text[i] == '/' && (text[i + 1] == '/' || text[i + 1] == '*'))
            return 0;
        if (text[i] == '"' || text[i] == '\'') {
            int quote = text[i++];
            while (text[i] && text[i] != quote) {
                if (text[i] == '\\' && text[i + 1])
                    i++;
                i++;
            }
            if (text[i]) i++;
            continue;
        }
        if (repl_syntax_is_ident_start((unsigned char)text[i])) {
            int start = i;
            int len;
            int j;
            while (text[i] && repl_syntax_is_ident_char(text[i]))
                i++;
            len = i - start;
            j = i;
            while (text[j] == ' ' || text[j] == '\t')
                j++;
            if (text[j] == '(' && len < 64) {
                memcpy(out, text + start, (size_t)len);
                out[len] = '\0';
                return 1;
            }
            continue;
        }
        i++;
    }
    return 0;
}

static CmdSyntaxCategory repl_code_panel_call_category(const char *name) {
    const ReplEnumCommandSpec *enum_spec;
    const ReplStdCommandSpec *std_spec;

    for (enum_spec = repl_enum_command_specs(); enum_spec->name; enum_spec++)
        if (strcmp(name, enum_spec->name) == 0)
            return repl_cmd_type_category(enum_spec->type);
    for (std_spec = repl_std_command_specs(); std_spec->name; std_spec++)
        if (strcmp(name, std_spec->name) == 0)
            return repl_cmd_type_category(std_spec->type);

    if (strncmp(name, "glVertex", 8) == 0)
        return CMD_CAT_VERTEX;
    if (strncmp(name, "glNormal", 8) == 0)
        return CMD_CAT_NORMAL;
    if (strncmp(name, "glColor", 7) == 0 ||
        strncmp(name, "glMaterial", 10) == 0 ||
        strcmp(name, "glClearColor") == 0)
        return CMD_CAT_COLOR;
    if (strncmp(name, "glTranslate", 11) == 0 ||
        strncmp(name, "glRotate", 8) == 0 ||
        strncmp(name, "glScale", 7) == 0 ||
        strcmp(name, "glLoadIdentity") == 0 ||
        strcmp(name, "glPushMatrix") == 0 ||
        strcmp(name, "glPopMatrix") == 0 ||
        strcmp(name, "glOrtho") == 0 ||
        strcmp(name, "gluPerspective") == 0 ||
        strcmp(name, "gluLookAt") == 0)
        return CMD_CAT_TRANSFORM;
    if (strcmp(name, "glBegin") == 0 || strcmp(name, "glEnd") == 0)
        return CMD_CAT_PRIMITIVE;
    if (strncmp(name, "gluTess", 7) == 0)
        return CMD_CAT_TESS_BLOCK;
    if (strncmp(name, "glutSolid", 9) == 0 ||
        strncmp(name, "glutWire", 8) == 0)
        return CMD_CAT_GLUT_SHAPE;
    if (strncmp(name, "gl", 2) == 0 || strncmp(name, "glu", 3) == 0)
        return CMD_CAT_STATE;
    return CMD_CAT_FUNCTION;
}

CmdSyntaxCategory ui_repl_code_panel_generated_category(const char *text) {
    const char *p = text ? text : "";
    char call[64];

    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == '/' && (p[1] == '/' || p[1] == '*'))
        return CMD_CAT_COMMENT;
    if (*p == '#') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "if", 2) == 0 || strncmp(p, "else", 4) == 0 ||
            strncmp(p, "endif", 5) == 0)
            return CMD_CAT_CONDITIONAL;
        return CMD_CAT_STATE;
    }
    if ((strncmp(p, "if", 2) == 0 && !repl_syntax_is_ident_char(p[2])) ||
        (strncmp(p, "else", 4) == 0 && !repl_syntax_is_ident_char(p[4])) ||
        (strncmp(p, "switch", 6) == 0 && !repl_syntax_is_ident_char(p[6])))
        return CMD_CAT_CONDITIONAL;
    if ((strncmp(p, "for", 3) == 0 && !repl_syntax_is_ident_char(p[3])) ||
        (strncmp(p, "while", 5) == 0 && !repl_syntax_is_ident_char(p[5])) ||
        (strncmp(p, "do", 2) == 0 && !repl_syntax_is_ident_char(p[2])))
        return CMD_CAT_LOOP;
    if (strncmp(p, "return", 6) == 0 && !repl_syntax_is_ident_char(p[6]))
        return CMD_CAT_FUNCTION;
    if (repl_code_panel_first_call_name(p, call))
        return repl_code_panel_call_category(call);
    if (strncmp(p, "static", 6) == 0 || strncmp(p, "const", 5) == 0 ||
        strncmp(p, "float", 5) == 0 || strncmp(p, "double", 6) == 0 ||
        strncmp(p, "int", 3) == 0 || strncmp(p, "char", 4) == 0 ||
        strncmp(p, "GL", 2) == 0 || strchr(p, '=') != NULL)
        return CMD_CAT_VARIABLE;
    return CMD_CAT_DEFAULT;
}

/* mode: 0 = off (no spans), 1 = on, 2 = on + drop-shadow constants. */
static void repl_code_panel_apply_syntax_segments_for_category(
    const UiRenderSnapshot *snap,
    const char *text,
    CmdSyntaxCategory cat,
    int mode,
    UiTextPanelRow *row) {
    UiSyntaxSpan spans[UI_TEXT_PANEL_MAX_COLOR_SEGMENTS];
    int count;

    if (!row || !text || !text[0] || mode <= 0)
        return;

    if (cat == CMD_CAT_COMMENT)
        return;  /* whole comment line keeps the comment color */

    count = ui_repl_code_panel_classify_syntax(
        snap, text, spans, UI_TEXT_PANEL_MAX_COLOR_SEGMENTS);
    if (count <= 0)
        return;

    row->color_segment_count = 0;
    for (int i = 0;
         i < count &&
         row->color_segment_count < UI_TEXT_PANEL_MAX_COLOR_SEGMENTS;
         i++) {
        float rgb[3];

        ui_repl_code_panel_syntax_kind_rgb(spans[i].kind, cat, rgb);
        row->color_segments[row->color_segment_count++] =
            (UiTextPanelColorSegment){
                .char_start = spans[i].start,
                .char_count = spans[i].len,
                .color = repl_code_panel_rgb(rgb[0], rgb[1], rgb[2]),
                /* On+Shadow mode drop-shadows constants only. */
                .shadow = (mode == 2 &&
                           spans[i].kind == REPL_SYNTAX_CONSTANT),
            };
    }
}

static void repl_code_panel_apply_syntax_segments(const UiRenderSnapshot *snap,
                                                  const char *text,
                                                  CmdType type,
                                                  int mode,
                                                  UiTextPanelRow *row) {
    repl_code_panel_apply_syntax_segments_for_category(
        snap, text, repl_cmd_type_category(type), mode, row);
}

/* Color a command line's trailing `// ...` comment with the comment
 * category color (matching standalone comment lines) instead of letting
 * it inherit the command's syntax color. Appended after any syntax spans
 * - which stop at the '//' - so the segments stay ordered and disjoint;
 * the renderer fills gaps with row->color. Works in both syntax-highlight
 * modes: ON (code spans precede it) and OFF (no spans, so the code prefix
 * renders as the gap in row->color). Fades are handled separately and
 * never reach here. */
static void repl_code_panel_apply_trailing_comment_segment(const char *text,
                                                           UiTextPanelRow *row) {
    const char *cmt;
    float r, g, b;

    if (!row || !text)
        return;
    cmt = repl_line_trailing_comment(text);
    if (!cmt || !cmt[0])
        return;
    if (row->color_segment_count >= UI_TEXT_PANEL_MAX_COLOR_SEGMENTS)
        return;  /* dense line already at the span cap; degrade gracefully */

    repl_code_panel_category_rgb(CMD_CAT_COMMENT, &r, &g, &b);
    row->color_segments[row->color_segment_count++] =
        (UiTextPanelColorSegment){
            .char_start = (int)(cmt - text),
            .char_count = (int)strlen(cmt),
            .color = repl_code_panel_rgb(r, g, b),
            .shadow = 0,
        };
}

static float repl_code_panel_clamp_color_component(float value) {
    if (value < 0.0f)
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

/* Expanded replay appends an evaluated glColor/gluColor call after the source
 * row. Tint just that generated suffix with the RGB state it establishes, so
 * expressions changing color over time are visible at a glance. Alpha stays
 * opaque here: applying glColor4f's alpha to the glyphs would make a perfectly
 * valid zero-alpha annotation disappear and defeat the debugging aid. */
static void repl_code_panel_apply_replay_color_comment_segment(
    const UiRenderSnapshot *snap, CmdType type, const char *text,
    UiTextPanelRow *row) {
    const char *annotation = NULL;
    const char *scan;
    float r, g, b, a;
    int consumed = 0;
    int matched = 0;

    if (!snap || !text || !row || !snap->replay.active ||
        snap->replay.expand_args != REPLAY_EXPAND_EXPANDED ||
        !repl_cmd_sets_current_color(type))
        return;

    /* The source may already have its own trailing comment. Replay's
     * generated annotation is always the final // suffix. */
    scan = text;
    while ((scan = strstr(scan, "//")) != NULL) {
        annotation = scan;
        scan += 2;
    }
    if (!annotation)
        return;

    switch (type) {
    case CMD_COLOR3F:
        matched = sscanf(annotation, "// glColor3f(%f, %f, %f);%n",
                         &r, &g, &b, &consumed);
        break;
    case CMD_COLOR4F:
        matched = sscanf(annotation, "// glColor4f(%f, %f, %f, %f);%n",
                         &r, &g, &b, &a, &consumed);
        break;
    case CMD_TESS_COLOR:
        matched = sscanf(annotation, "// gluColor(%f, %f, %f, %f);%n",
                         &r, &g, &b, &a, &consumed);
        break;
    default:
        return;
    }
    if (matched != (type == CMD_COLOR3F ? 3 : 4) ||
        consumed <= 0 || annotation[consumed] != '\0' ||
        row->color_segment_count >= UI_TEXT_PANEL_MAX_COLOR_SEGMENTS)
        return;

    row->color_segments[row->color_segment_count++] =
        (UiTextPanelColorSegment){
            .char_start = (int)(annotation - text),
            .char_count = consumed,
            .color = repl_code_panel_rgb(
                repl_code_panel_clamp_color_component(r),
                repl_code_panel_clamp_color_component(g),
                repl_code_panel_clamp_color_component(b)),
            .shadow = 0,
        };
}

static void repl_code_panel_append_attrib_bit_token_segment(
    UiTextPanelRow *row, const ReplAttribTokenSpan *span) {
    int idx;

    if (!row || !span || row->color_segment_count >= UI_TEXT_PANEL_MAX_COLOR_SEGMENTS)
        return;
    idx = span->bit_idx;
    if (idx < 0 || idx >= REPL_ATTRIB_BIT_COUNT)
        return;
    row->color_segments[row->color_segment_count++] =
        (UiTextPanelColorSegment){
            .char_start = span->char_start,
            .char_count = span->char_end - span->char_start,
            .color = repl_code_panel_rgb(k_attrib_bit_colors[idx].r,
                                         k_attrib_bit_colors[idx].g,
                                         k_attrib_bit_colors[idx].b),
            .shadow = 0,
        };
}

static int repl_code_panel_has_attrib_bit_token_highlight(
    const UiRenderSnapshot *snap, int line_idx, int bit_idx) {
    if (!snap || !snap->editor_highlights)
        return 0;
    for (int i = 0; i < snap->editor_highlights->count; i++) {
        const UiHighlight *h = &snap->editor_highlights->items[i];
        if (h->kind == HIGHLIGHT_ATTRIB_BIT_TOKEN &&
            h->line_idx == line_idx && h->aux == bit_idx)
            return 1;
    }
    return 0;
}

/* On a highlighted committed glPushAttrib row, scan the exact display text
 * for token spans and retain the controller's parsed-mask gate. The scan is
 * deliberately against `text`, rather than trusting stored char ranges, so a
 * display-text substitution cannot move a token's per-bit color. */
static void repl_code_panel_apply_attrib_bit_token_segments(
    const UiRenderSnapshot *snap, int line_idx, const char *text,
    UiTextPanelRow *row) {
    ReplAttribTokenSpan spans[REPL_ATTRIB_BIT_COUNT];
    int n;

    if (!snap || !row || !text)
        return;
    n = repl_attrib_bit_token_spans(text, spans, REPL_ATTRIB_BIT_COUNT);
    for (int i = 0; i < n; i++) {
        if (repl_code_panel_has_attrib_bit_token_highlight(
                snap, line_idx, spans[i].bit_idx))
            repl_code_panel_append_attrib_bit_token_segment(row, &spans[i]);
    }
}

/* The active input row is intentionally narrower than committed-row syntax:
 * only a live glPushAttrib(...) call gets per-bit color, and every supported
 * token currently typed inside its first (...) is shown. The spans come
 * straight from the live input text, so mid-edit changes need no indentation
 * offset or committed-buffer coordinate translation. */
static void repl_code_panel_apply_input_attrib_bit_token_segments(
    const char *text, UiTextPanelRow *row) {
    ReplAttribTokenSpan spans[REPL_ATTRIB_BIT_COUNT];
    int n;

    if (!text || !row)
        return;
    n = repl_attrib_bit_token_spans(text, spans, REPL_ATTRIB_BIT_COUNT);
    for (int i = 0; i < n; i++)
        repl_code_panel_append_attrib_bit_token_segment(row, &spans[i]);
}

static int repl_code_panel_input_is_push_attrib(const char *text) {
    const char *name = "glPushAttrib";
    int name_len = (int)strlen(name);

    if (!text)
        return 0;
    while (isspace((unsigned char)*text))
        text++;
    if (strncmp(text, name, (size_t)name_len) != 0)
        return 0;
    text += name_len;
    while (isspace((unsigned char)*text))
        text++;
    return *text == '(';
}

/* Generated C is informative but not editable. Preserve the normal syntax
 * hues, then pull every color toward its own luminance (lower saturation) and
 * scale it down (lower brightness). Applying this after token classification
 * keeps the same visual grammar as user code without competing with it. */
static UiTextPanelColor repl_code_panel_muted_generated_color(
    UiTextPanelColor color) {
    const float saturation = 0.52f;
    const float brightness = 0.72f;
    float lum = 0.30f * color.r + 0.59f * color.g + 0.11f * color.b;

    color.r = repl_clamp01((lum + (color.r - lum) * saturation) * brightness);
    color.g = repl_clamp01((lum + (color.g - lum) * saturation) * brightness);
    color.b = repl_clamp01((lum + (color.b - lum) * saturation) * brightness);
    return color;
}

static void repl_code_panel_mute_generated_row(UiTextPanelRow *row) {
    if (!row)
        return;
    row->color = repl_code_panel_muted_generated_color(row->color);
    for (int i = 0; i < row->color_segment_count; i++) {
        row->color_segments[i].color = repl_code_panel_muted_generated_color(
            row->color_segments[i].color);
        /* The generated layer is deliberately subordinate even in the
         * user's "On + Shadow" syntax mode. */
        row->color_segments[i].shadow = 0;
    }
}

static void repl_code_panel_add_static_row(ReplCodePanelBuilder *builder,
                                           const char *text) {
    UiTextPanelRow *row = repl_code_panel_push_row(builder);
    CmdSyntaxCategory cat;
    float r, g, b;

    if (!row)
        return;

    row->text = text ? text : "";
    row->kind = UI_TEXT_PANEL_ROW_STATIC;
    row->left_gutter_label = builder->file_line++;
    cat = ui_repl_code_panel_generated_category(row->text);
    repl_code_panel_category_rgb(cat, &r, &g, &b);
    row->color = repl_code_panel_rgb(r, g, b);
    row->hit_eligible = 0;
    repl_code_panel_apply_syntax_segments_for_category(
        builder->snap, row->text, cat,
        builder->snap->code_panel.syntax_highlight, row);
    repl_code_panel_apply_trailing_comment_segment(row->text, row);
    repl_code_panel_mute_generated_row(row);
}

static void repl_code_panel_add_input_row(ReplCodePanelBuilder *builder,
                                          int source_line_idx,
                                          int hit_target_line_idx,
                                          int indent_chars,
                                          int search_row_idx,
                                          const char *aux_label) {
    UiTextPanelRow *row = repl_code_panel_push_row(builder);

    if (!row)
        return;

    row->text = "";
    row->kind = UI_TEXT_PANEL_ROW_INPUT;
    row->left_gutter_label = builder->file_line++;
    row->source_line_idx = source_line_idx;
    row->hit_target_line_idx = hit_target_line_idx;
    row->search_row_idx = search_row_idx;
    row->indent_chars = indent_chars;
    row->display_prefix_chars =
        repl_code_panel_func_return_prefix_chars(builder->snap,
                                                 source_line_idx);
    row->hit_eligible = 1;
    if (aux_label && aux_label[0])
        snprintf(row->left_aux_label, sizeof(row->left_aux_label), "%s", aux_label);
    repl_code_panel_apply_tutorial_insertion_marker(builder, hit_target_line_idx,
                                                    row);

    if (source_line_idx >= 0) {
        repl_code_panel_apply_command_overlays(builder, source_line_idx, row);
        repl_code_panel_set_right_action(row, builder->snap, source_line_idx);
    }
    if (repl_code_panel_input_is_push_attrib(builder->snap->editor_input.input)) {
        row->color = ui_text_panel_input_text_color();
        repl_code_panel_apply_input_attrib_bit_token_segments(
            builder->snap->editor_input.input, row);
    }
}

static void repl_code_panel_add_placeholder_row(ReplCodePanelBuilder *builder,
                                                int hit_target_line_idx,
                                                int indent_chars) {
    UiTextPanelRow *row = repl_code_panel_push_row(builder);

    if (!row)
        return;

    row->text = "";
    row->kind = UI_TEXT_PANEL_ROW_PLACEHOLDER;
    row->left_gutter_label = builder->file_line++;
    row->source_line_idx = hit_target_line_idx;
    row->hit_target_line_idx = hit_target_line_idx;
    row->indent_chars = indent_chars;
    row->hit_eligible = 1;
    row->color = repl_code_panel_rgb(0.28f, 0.28f, 0.35f);
    repl_code_panel_apply_tutorial_insertion_marker(builder, hit_target_line_idx,
                                                    row);
}

static void repl_code_panel_add_command_row(ReplCodePanelBuilder *builder,
                                            int line_idx,
                                            int is_vertex,
                                            int vnum,
                                            int primitive_vnums_exact) {
    UiTextPanelRow *row = repl_code_panel_push_row(builder);
    const char *display_text;
    char *decorated;

    if (!row)
        return;

    display_text = repl_code_panel_display_text(builder->snap, line_idx);
    if (repl_code_panel_func_return_prefix_chars(builder->snap, line_idx) > 0) {
        decorated = repl_code_panel_next_generated_text(builder);
        if (decorated)
            display_text = repl_code_panel_add_func_return_prefix(
                builder->snap, line_idx, display_text, decorated,
                MAX_LINE_LEN * 2);
    }
    row->text = display_text;
    row->kind = UI_TEXT_PANEL_ROW_TEXT;
    row->left_gutter_label = builder->file_line++;
    row->source_line_idx = line_idx;
    row->search_row_idx = repl_code_panel_search_row_for_cmd(builder->snap,
                                                             line_idx);
    row->display_prefix_chars =
        repl_code_panel_func_return_prefix_chars(builder->snap, line_idx);
    row->color = repl_code_panel_category_color(
        builder->snap->document_cmds[line_idx].type);
    row->hit_eligible = 1;
    repl_code_panel_set_vertex_label(row, builder->snap, line_idx, is_vertex, vnum,
                                     primitive_vnums_exact);
    repl_code_panel_set_auto_label(row, builder->snap, line_idx);
    repl_code_panel_set_right_action(row, builder->snap, line_idx);
    repl_code_panel_apply_command_overlays(builder, line_idx, row);
    if (repl_code_panel_line_is_fading(builder->snap, line_idx)) {
        repl_code_panel_apply_fade_segments(builder->snap, line_idx, display_text, row);
    } else {
        repl_code_panel_apply_syntax_segments(
            builder->snap,
            display_text,
            builder->snap->document_cmds[line_idx].type,
            builder->snap->code_panel.syntax_highlight, row);
        /* A command line's trailing `// ...` reads as a comment, not as
         * part of the command's syntax color. (The fade path is for
         * whole-line instruction comments, so it is left untouched.) */
        repl_code_panel_apply_trailing_comment_segment(display_text, row);
        repl_code_panel_apply_replay_color_comment_segment(
            builder->snap, builder->snap->document_cmds[line_idx].type,
            display_text, row);
        /* Cursor-on-push/pop: colour the glPushAttrib line's mask tokens.
         * Last so the opaque per-bit spans win over the syntax colour. */
        repl_code_panel_apply_attrib_bit_token_segments(builder->snap, line_idx,
                                                        display_text, row);
    }
    repl_code_panel_apply_auto_dim(row, builder->snap, line_idx);
}

static const char k_replay_path_prefix[] = "|-> ";
static const char k_replay_path_sep[] = " > ";
static const char k_replay_path_incomplete[] = "[incomplete]";

/* Displayable chars in UiVirtualLine.text (the array includes the NUL). */
#define REPLAY_PATH_TEXT_MAX       (MAX_VIRTUAL_LINE_TEXT - 1)
/* Room for the prefix so one fragment always fits with "|-> ". */
#define REPLAY_PATH_FRAG_MAX       (REPLAY_PATH_TEXT_MAX - \
                                    (int)(sizeof(k_replay_path_prefix) - 1))
#define REPLAY_PATH_LOOP_FRAG_LEN  (REPL_PREDEF_NAME_MAX + 24)
#define REPLAY_PATH_RUNG_FRAG_LEN  (REPLAY_PATH_FRAG_MAX + 1)
#define REPLAY_PATH_KEEP_OUTER     1
#define REPLAY_PATH_KEEP_INNER     2

/* Append src if the whole token fits. Returns 1 on success. */
static int replay_path_append(char *buf, int buf_sz, int *pos, const char *src) {
    int n;

    if (!buf || buf_sz <= 0 || !pos || *pos < 0)
        return 0;
    if (!src)
        src = "";
    n = (int)strlen(src);
    if (*pos + n >= buf_sz)
        return 0;
    memcpy(buf + *pos, src, (size_t)n);
    *pos += n;
    buf[*pos] = '\0';
    return 1;
}

static void replay_path_format_loop(const ReplReplayPathLoop *loop,
                                    char *buf, int buf_sz) {
    snprintf(buf, (size_t)buf_sz, "%s=%g",
             loop->name[0] ? loop->name : "?", (double)loop->value);
}

/* Format one number-plus-optional-comma into num. Always a complete token. */
static void replay_path_format_arg(char *num, int num_sz, int with_comma,
                                   float value) {
    snprintf(num, (size_t)num_sz, "%s%g", with_comma ? ", " : "",
             (double)value);
}

/* arg_head < 0: all args that fit. arg_head == 0 && arg_tail == 0: (...).
 * Otherwise keep the first arg_head and last arg_tail, with "..." between.
 * Unindexed rungs (args_available == 0) always render as name(...).
 * Never writes a partial number. */
static void replay_path_format_rung(const ReplReplayPathRung *rung,
                                    char *buf, int buf_sz,
                                    int arg_head, int arg_tail) {
    char num[32];
    int pos = 0;
    int n;
    int a;
    int hide_args;

    if (!buf || buf_sz <= 0)
        return;
    buf[0] = '\0';
    if (buf_sz > REPLAY_PATH_RUNG_FRAG_LEN)
        buf_sz = REPLAY_PATH_RUNG_FRAG_LEN;

    replay_path_append(buf, buf_sz, &pos,
                       rung->func_name[0] ? rung->func_name : "?");
    replay_path_append(buf, buf_sz, &pos, "(");

    n = rung->arg_count;
    if (n < 0)
        n = 0;
    hide_args = !rung->args_available || (arg_head == 0 && arg_tail == 0);
    if (hide_args) {
        replay_path_append(buf, buf_sz, &pos, "...");
        replay_path_append(buf, buf_sz, &pos, ")");
        return;
    }
    if (arg_head < 0 || arg_head + arg_tail >= n) {
        for (a = 0; a < n; a++) {
            int last = (a + 1 == n);
            int reserve;

            replay_path_format_arg(num, (int)sizeof(num), a > 0,
                                   rung->args[a]);
            /* Keep room for ')' if this is the last arg, or ', ...)' if
             * more remain, so a truncated list never ends mid-number. */
            reserve = last ? 1 : (int)sizeof(", ...)") - 1;
            if (pos + (int)strlen(num) + reserve >= buf_sz) {
                if (a > 0)
                    replay_path_append(buf, buf_sz, &pos, ", ");
                replay_path_append(buf, buf_sz, &pos, "...");
                break;
            }
            replay_path_append(buf, buf_sz, &pos, num);
        }
    } else {
        for (a = 0; a < arg_head && a < n; a++) {
            replay_path_format_arg(num, (int)sizeof(num), a > 0,
                                   rung->args[a]);
            if (pos + (int)strlen(num) + 1 >= buf_sz)
                break;
            replay_path_append(buf, buf_sz, &pos, num);
        }
        if (arg_head > 0)
            replay_path_append(buf, buf_sz - 1, &pos, ", ");
        replay_path_append(buf, buf_sz - 1, &pos, "...");
        for (a = n - arg_tail; a < n; a++) {
            if (a < arg_head)
                continue;
            replay_path_format_arg(num, (int)sizeof(num), 1, rung->args[a]);
            if (pos + (int)strlen(num) + 1 >= buf_sz)
                break;
            replay_path_append(buf, buf_sz, &pos, num);
        }
    }
    replay_path_append(buf, buf_sz, &pos, ")");
}

static int replay_path_joined_len(const char *const *parts, int nparts) {
    int used = (int)(sizeof(k_replay_path_prefix) - 1);
    int i;

    for (i = 0; i < nparts; i++) {
        if (i)
            used += (int)(sizeof(k_replay_path_sep) - 1);
        used += (int)strlen(parts[i] ? parts[i] : "");
    }
    return used;
}

/* Join whole parts only. A part that does not fit is replaced by "..."
 * so the result never ends mid-token. */
static int replay_path_join(char *out, int out_sz,
                            const char *const *parts, int nparts) {
    int used = 0;
    int i;

    if (!out || out_sz <= 0)
        return 0;
    out[0] = '\0';
    if (!replay_path_append(out, out_sz, &used, k_replay_path_prefix))
        return 0;
    for (i = 0; i < nparts; i++) {
        const char *sep = i ? k_replay_path_sep : "";
        const char *p = parts[i] ? parts[i] : "";
        int need = (int)strlen(sep) + (int)strlen(p);

        if (used + need >= out_sz) {
            replay_path_append(out, out_sz, &used, "...");
            break;
        }
        replay_path_append(out, out_sz, &used, sep);
        replay_path_append(out, out_sz, &used, p);
    }
    return used;
}

static int replay_path_emit_loop_span(const char **loop_frags, int nloops,
                                      int head, int tail,
                                      const char **parts, int nparts,
                                      char *ellipsis, int ellipsis_sz) {
    int i;
    int omitted;

    if (head < 0 || head + tail >= nloops) {
        for (i = 0; i < nloops; i++)
            parts[nparts++] = loop_frags[i];
        return nparts;
    }
    for (i = 0; i < head; i++)
        parts[nparts++] = loop_frags[i];
    omitted = nloops - head - tail;
    if (omitted > 0 && ellipsis && ellipsis_sz > 0) {
        snprintf(ellipsis, (size_t)ellipsis_sz, "... %d loops ...", omitted);
        parts[nparts++] = ellipsis;
    }
    for (i = nloops - tail; i < nloops; i++) {
        if (i < head)
            continue;
        parts[nparts++] = loop_frags[i];
    }
    return nparts;
}

int ui_repl_code_panel_format_replay_path(const ReplReplayPathSnapshot *path,
                                          char *out, int out_sz) {
    char loop_frags[MAX_EXPR_VARS][REPLAY_PATH_LOOP_FRAG_LEN];
    char rung_frags[REPL_REPLAY_PATH_RUNG_MAX][REPLAY_PATH_RUNG_FRAG_LEN];
    const char *loop_ptrs[MAX_EXPR_VARS];
    char frame_ellipsis[sizeof("... ") + 10 + sizeof(" frames ...")];
    char loop_ellipsis[sizeof("... ") + 10 + sizeof(" loops ...")];
    const char *parts[MAX_EXPR_VARS + REPL_REPLAY_PATH_RUNG_MAX + 3];
    int nloops = 0;
    int nrungs = 0;
    int nparts;
    int i;
    int budget;
    int arg_head;
    int arg_tail;
    int loop_head;
    int loop_tail;
    int rung_outer;
    int rung_inner;
    int pass;

    if (!out || out_sz <= 0)
        return 0;
    out[0] = '\0';
    if (!path || !path->valid)
        return 0;

    for (i = 0; i < path->loop_count && i < MAX_EXPR_VARS; i++) {
        replay_path_format_loop(&path->loops[i], loop_frags[i],
                                (int)sizeof(loop_frags[i]));
        loop_ptrs[i] = loop_frags[i];
        nloops++;
    }
    nrungs = path->rung_count;
    if (nrungs > REPL_REPLAY_PATH_RUNG_MAX)
        nrungs = REPL_REPLAY_PATH_RUNG_MAX;
    if (nloops == 0 && nrungs == 0 && !path->overflow)
        return 0;

    budget = out_sz - 1;
    if (budget > REPLAY_PATH_TEXT_MAX)
        budget = REPLAY_PATH_TEXT_MAX;
    if (budget < 0)
        budget = 0;

    /* Tightening passes: full path, then frame-middle, loop-middle,
     * drop loops, shrink args, func(...), outer+inner, innermost. */
    for (pass = 0; pass < 8; pass++) {
        switch (pass) {
        case 0:
            loop_head = -1; loop_tail = 0;
            rung_outer = nrungs; rung_inner = 0;
            arg_head = -1; arg_tail = 0;
            break;
        case 1:
            loop_head = -1; loop_tail = 0;
            rung_outer = REPLAY_PATH_KEEP_OUTER;
            rung_inner = REPLAY_PATH_KEEP_INNER;
            arg_head = -1; arg_tail = 0;
            break;
        case 2:
            loop_head = 1; loop_tail = 1;
            rung_outer = REPLAY_PATH_KEEP_OUTER;
            rung_inner = REPLAY_PATH_KEEP_INNER;
            arg_head = -1; arg_tail = 0;
            break;
        case 3:
            loop_head = 0; loop_tail = 0;
            rung_outer = REPLAY_PATH_KEEP_OUTER;
            rung_inner = REPLAY_PATH_KEEP_INNER;
            arg_head = -1; arg_tail = 0;
            break;
        case 4:
            loop_head = 0; loop_tail = 0;
            rung_outer = REPLAY_PATH_KEEP_OUTER;
            rung_inner = REPLAY_PATH_KEEP_INNER;
            arg_head = 1; arg_tail = 1;
            break;
        case 5:
            loop_head = 0; loop_tail = 0;
            rung_outer = REPLAY_PATH_KEEP_OUTER;
            rung_inner = REPLAY_PATH_KEEP_INNER;
            arg_head = 0; arg_tail = 0;
            break;
        case 6:
            loop_head = 0; loop_tail = 0;
            rung_outer = REPLAY_PATH_KEEP_OUTER;
            rung_inner = 1;
            arg_head = 0; arg_tail = 0;
            break;
        default:
            loop_head = 0; loop_tail = 0;
            rung_outer = 0; rung_inner = 1;
            arg_head = 0; arg_tail = 0;
            break;
        }

        for (i = 0; i < nrungs; i++)
            replay_path_format_rung(&path->rungs[i], rung_frags[i],
                                    (int)sizeof(rung_frags[i]),
                                    arg_head, arg_tail);

        nparts = 0;
        if (path->overflow)
            parts[nparts++] = k_replay_path_incomplete;
        nparts = replay_path_emit_loop_span(loop_ptrs, nloops,
                                            loop_head, loop_tail,
                                            parts, nparts,
                                            loop_ellipsis,
                                            (int)sizeof(loop_ellipsis));
        if (rung_outer < 0 || rung_outer + rung_inner >= nrungs) {
            for (i = 0; i < nrungs; i++)
                parts[nparts++] = rung_frags[i];
        } else {
            int omitted = nrungs - rung_outer - rung_inner;

            for (i = 0; i < rung_outer; i++)
                parts[nparts++] = rung_frags[i];
            if (omitted > 0) {
                snprintf(frame_ellipsis, sizeof(frame_ellipsis),
                         "... %d frames ...", omitted);
                parts[nparts++] = frame_ellipsis;
            }
            for (i = nrungs - rung_inner; i < nrungs; i++) {
                if (i < rung_outer)
                    continue;
                parts[nparts++] = rung_frags[i];
            }
        }

        if (nparts == 0)
            continue;
        if (replay_path_joined_len(parts, nparts) <= budget || pass == 7)
            return replay_path_join(out, budget + 1, parts, nparts);
    }
    return 0;
}

static void repl_code_panel_add_virtual_rows(ReplCodePanelBuilder *builder,
                                             int after_line_idx) {
    const UiVirtualLineList *virtual_lines;

    if (!builder || !builder->snap)
        return;

    virtual_lines = builder->snap->editor_virtual_lines;
    if (!virtual_lines)
        return;

    for (int i = 0; i < virtual_lines->count; i++) {
        const UiVirtualLine *virtual_line = &virtual_lines->items[i];
        UiTextPanelRow *row;
        char *text;
        int main_len;
        int aux_len;

        if (virtual_line->after_line_idx != after_line_idx)
            continue;

        row = repl_code_panel_push_row(builder);
        text = repl_code_panel_next_generated_text(builder);
        if (!row || !text)
            return;

        if (virtual_line->style == VIRTUAL_STYLE_REPLAY_PATH) {
            ui_repl_code_panel_format_replay_path(&builder->snap->replay_path,
                                                  text, MAX_LINE_LEN * 2);
        } else {
            snprintf(text, MAX_LINE_LEN * 2, "%s%s", virtual_line->text,
                     virtual_line->aux);
        }
        row->text = text;
        row->kind = UI_TEXT_PANEL_ROW_VIRTUAL;
        row->hit_target_line_idx = after_line_idx;
        row->hit_eligible = after_line_idx >= 0;

        if (virtual_line->style == VIRTUAL_STYLE_REPLAY_PATH) {
            row->background_active = 1;
            row->background_color = repl_code_panel_rgba(0.28f, 0.20f, 0.08f, 0.35f);
            row->color = repl_code_panel_rgb(0.85f, 0.74f, 0.42f);
        } else if (virtual_line->style == VIRTUAL_STYLE_REPLAY_SUBST) {
            row->background_active = 1;
            row->background_color = repl_code_panel_rgba(0.10f, 0.25f, 0.15f, 0.35f);
            row->color = repl_code_panel_rgb(0.50f, 0.75f, 0.50f);
        } else {
            row->background_active = 1;
            row->background_color = repl_code_panel_rgba(0.15f, 0.15f, 0.25f, 0.35f);
            row->color = repl_code_panel_rgb(0.50f, 0.60f, 0.80f);
        }

        main_len = (int)strlen(virtual_line->text);
        aux_len = (int)strlen(virtual_line->aux);
        if (aux_len > 0) {
            row->color_segments[0] = (UiTextPanelColorSegment){
                .char_start = 0,
                .char_count = main_len,
                .color = row->color,
            };
            row->color_segments[1] = (UiTextPanelColorSegment){
                .char_start = main_len,
                .char_count = aux_len,
                .color = repl_code_panel_rgb(0.40f, 0.55f, 0.40f),
            };
            row->color_segment_count = 2;
        }
    }
}

/* One-pass row emission carries a small source-order state machine for
 * vertex numbering and "exact vs maybe-loop-expanded" labels. Keep it
 * local to the builder path so the begin/end ordering stays obvious. */
typedef struct {
    int vnum;
    int loop_depth;
    int tess_depth;
    int in_tess_poly;
    int primitive_vnums_exact;
} ReplCodePanelWalkState;

static void repl_code_panel_add_static_null_terminated_lines(
    ReplCodePanelBuilder *builder,
    const char *const *lines) {
    if (!builder || !lines)
        return;

    for (int i = 0; lines[i]; i++)
        repl_code_panel_add_static_row(builder, lines[i]);
}

static void repl_code_panel_add_header_pre_lines(
    ReplCodePanelBuilder *builder) {
    if (!builder || !builder->snap)
        return;

    for (int i = 0; g_header_pre[i]; i++) {
        if (repl_export_header_pre_line_visible(
                i, builder->snap->math_collision_mask))
            repl_code_panel_add_static_row(builder, g_header_pre[i]);
    }
}

static void repl_code_panel_add_static_buffer_lines(
    ReplCodePanelBuilder *builder,
    int count,
    size_t line_size,
    const char (*lines)[line_size]) {
    if (!builder || !lines)
        return;

    for (int i = 0; i < count; i++)
        repl_code_panel_add_static_row(builder, lines[i]);
}

static void repl_code_panel_add_header_rows(ReplCodePanelBuilder *builder) {
    const UiRenderSnapshot *snap;

    if (!builder || !builder->snap)
        return;

    snap = builder->snap;
    if (!repl_code_panel_chrome_visible(snap))
        return;

    repl_code_panel_add_static_buffer_lines(
        builder,
        snap->import_export.workspace_header_line_count,
        sizeof(snap->import_export.workspace_header_lines[0]),
        snap->import_export.workspace_header_lines);
    repl_code_panel_add_header_pre_lines(builder);
    repl_code_panel_add_static_buffer_lines(
        builder,
        snap->gl_vector_helper_line_count,
        sizeof(snap->gl_vector_helper_lines[0]),
        snap->gl_vector_helper_lines);

    /* Scratch decoration row: panel-only (the exporter emits the arrays as
     * file-scope statics on demand instead). It decorates the global
     * declarations, which now sit above display() themselves, so it
     * belongs with the file-scope chrome rather than below the frame. */
    repl_code_panel_add_static_row(
        builder, REPL_CODE_PANEL_SCRATCH_DECL_LINE);
}

/* Emitted at the display-body boundary - see
 * repl_code_panel_display_open_row_count for what is frame and what is
 * chrome. Must stay in lockstep with that counter. */
static void repl_code_panel_add_display_open_rows(
    ReplCodePanelBuilder *builder) {
    const UiRenderSnapshot *snap;

    if (!builder || !builder->snap)
        return;
    snap = builder->snap;

    if (repl_code_panel_display_open_spacer(snap))
        repl_code_panel_add_static_row(builder, "");
    repl_code_panel_add_static_row(builder,
                                   repl_code_panel_display_open_line(snap));
    if (!repl_code_panel_chrome_visible(snap))
        return;

    for (int i = 1; g_display_header[i]; i++)
        repl_code_panel_add_static_row(builder, g_display_header[i]);
    repl_code_panel_add_static_buffer_lines(
        builder,
        snap->lights_pre_camera_count,
        sizeof(snap->lights_pre_camera_lines[0]),
        snap->lights_pre_camera_lines);
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++) {
        if (snap->import_export.cam_lines[i][0])
            repl_code_panel_add_static_row(builder,
                                           snap->import_export.cam_lines[i]);
    }
    repl_code_panel_add_static_buffer_lines(
        builder,
        snap->lights_display_count,
        sizeof(snap->lights_display_lines[0]),
        snap->lights_display_lines);
    repl_code_panel_add_static_null_terminated_lines(
        builder, g_header_post);
}

/* Emitted after the trailing document row. Mirrors
 * repl_code_panel_display_close_row_count. */
static void repl_code_panel_add_display_close_rows(
    ReplCodePanelBuilder *builder) {
    if (!builder || !builder->snap)
        return;

    if (!repl_code_panel_chrome_visible(builder->snap)) {
        repl_code_panel_add_static_row(builder,
                                       REPL_EXPORT_DISPLAY_CLOSE_LINE);
        return;
    }
    repl_code_panel_add_static_null_terminated_lines(builder,
                                                     g_display_footer);
}

static void repl_code_panel_add_footer_rows(ReplCodePanelBuilder *builder) {
    const UiRenderSnapshot *snap;

    if (!builder || !builder->snap)
        return;

    snap = builder->snap;
    if (!repl_code_panel_chrome_visible(snap))
        return;

    for (int i = 0; g_footer_pre_init[i]; i++) {
        if (strcmp(g_footer_pre_init[i],
                   REPL_EXPORT_RESHAPE_PROJ_SENTINEL) == 0) {
            /* Frame-frozen in the snapshot by the controller; its
             * storage outlives this render, so add_static_row may hold
             * the pointer directly (like the literal g_footer lines).
             * Same block the row-count pass read, so they agree. */
            repl_code_panel_add_static_buffer_lines(
                builder,
                snap->reshape_proj_count,
                sizeof(snap->reshape_proj_lines[0]),
                snap->reshape_proj_lines);
            continue;
        }
        repl_code_panel_add_static_row(builder, g_footer_pre_init[i]);
    }

    repl_code_panel_add_static_buffer_lines(
        builder,
        snap->init_section_count,
        sizeof(snap->init_section_lines[0]),
        snap->init_section_lines);
    repl_code_panel_add_static_null_terminated_lines(
        builder, g_footer_post_init);
}

static void repl_code_panel_begin_walk_line(ReplCodePanelWalkState *state,
                                            const GLCmd *cmd) {
    if (!state || !cmd || !cmd->valid)
        return;

    /* Opening a primitive/tess polygon resets numbering before the row
     * is emitted so the current source line and the following vertices
     * already see the new numbering regime. */
    if (cmd->type == CMD_BEGIN) {
        state->vnum = 0;
        state->primitive_vnums_exact = (state->loop_depth == 0);
    } else if (cmd->type == CMD_TESS_BEGIN_POLYGON) {
        state->vnum = 0;
        state->in_tess_poly = 1;
        state->tess_depth = 1;
        state->primitive_vnums_exact = (state->loop_depth == 0);
    }
}

static void repl_code_panel_vertex_aux_label(const UiRenderSnapshot *snap,
                                             const ReplCodePanelWalkState *state,
                                             int line_idx,
                                             int is_vertex,
                                             char out_label[8]) {
    if (!out_label)
        return;

    out_label[0] = '\0';
    if (!snap || !state || !is_vertex)
        return;
    if (!repl_code_panel_line_in_current_begin_block(snap, line_idx))
        return;

    snprintf(out_label, 8,
             state->primitive_vnums_exact ? "v%d" : "vn",
             state->vnum);
}

static void repl_code_panel_end_walk_line(ReplCodePanelWalkState *state,
                                          const GLCmd *cmd,
                                          int is_vertex) {
    if (!state || !cmd)
        return;

    if (is_vertex)
        state->vnum++;
    if (!cmd->valid)
        return;

    /* Closing/unwinding commands advance after the row is emitted: the
     * line itself still belongs to the old context, and only later rows
     * should see the unwound state. */
    switch (cmd->type) {
    case CMD_FOR_BEGIN:
        state->loop_depth++;
        state->primitive_vnums_exact = 0;
        break;
    case CMD_FOR_END:
        if (state->loop_depth > 0)
            state->loop_depth--;
        break;
    case CMD_END:
        state->primitive_vnums_exact = 1;
        break;
    case CMD_TESS_BEGIN_CONTOUR:
        if (state->in_tess_poly)
            state->tess_depth++;
        break;
    case CMD_TESS_END:
        if (state->in_tess_poly) {
            if (state->tess_depth > 0)
                state->tess_depth--;
            if (state->tess_depth == 0) {
                state->in_tess_poly = 0;
                state->primitive_vnums_exact = 1;
            }
        }
        break;
    default:
        break;
    }
}

static void repl_code_panel_add_file_scope_slot(ReplCodePanelBuilder *builder) {
    const UiRenderSnapshot *snap;

    if (!builder || !builder->snap)
        return;
    snap = builder->snap;
    if (snap->tutorial.active)
        return;

    if (snap->editor_input.insert_mode &&
        snap->editor_input.insert_scope == EDITOR_INSERT_FILE_SCOPE) {
        repl_code_panel_add_input_row(builder, -1, snap->display_body_start,
                                      0, snap->display_body_start, NULL);
    } else {
        UiTextPanelRow *row = repl_code_panel_push_row(builder);
        if (!row)
            return;
        row->text = "+ float or function";
        row->kind = UI_TEXT_PANEL_ROW_VIRTUAL;
        row->left_gutter_label = 0;
        row->source_line_idx = -1;
        row->hit_target_line_idx = snap->display_body_start;
        row->indent_chars = 0;
        row->hit_eligible = 1;
        row->color = repl_code_panel_rgb(0.28f, 0.28f, 0.35f);
    }
}

static void repl_code_panel_add_rows_for_line(ReplCodePanelBuilder *builder,
                                              ReplCodePanelWalkState *state,
                                              int line_idx) {
    const UiRenderSnapshot *snap;
    const GLCmd *cmd;
    int is_edit;
    int is_vertex;
    char aux_label[8];

    if (!builder || !builder->snap || !state)
        return;

    snap = builder->snap;
    cmd = &snap->document_cmds[line_idx];

    if (snap->editor_input.insert_mode &&
        snap->editor_input.insert_scope == EDITOR_INSERT_DOCUMENT &&
        line_idx == snap->edit_line) {
        repl_code_panel_add_input_row(builder, -1, snap->edit_line,
                                      snap->active_indent_chars, line_idx,
                                      NULL);
    }

    repl_code_panel_begin_walk_line(state, cmd);

    is_edit = (!snap->editor_input.insert_mode && line_idx == snap->edit_line);
    is_vertex = cmd->valid && repl_cmd_emits_vertex(cmd->type);
    repl_code_panel_vertex_aux_label(snap, state, line_idx, is_vertex, aux_label);
    /* Parking the cursor on an auto row keeps the label (it is still an
     * auto row until the edit commits); the dim is deliberately not applied
     * to the live input row, which stays at full brightness. */
    if (!aux_label[0] && repl_code_panel_line_is_auto(snap, line_idx))
        snprintf(aux_label, sizeof(aux_label), "auto");

    if (is_edit) {
        repl_code_panel_add_input_row(builder, line_idx, -1,
                                      snap->active_indent_chars, line_idx,
                                      aux_label[0] ? aux_label : NULL);
    } else {
        repl_code_panel_add_command_row(builder, line_idx, is_vertex,
                                        state->vnum,
                                        state->primitive_vnums_exact);
    }

    repl_code_panel_add_virtual_rows(builder, line_idx);
    repl_code_panel_end_walk_line(state, cmd, is_vertex);
}

static void repl_code_panel_add_trailing_document_row(
    ReplCodePanelBuilder *builder) {
    const UiRenderSnapshot *snap;

    if (!builder || !builder->snap)
        return;

    snap = builder->snap;
    if (snap->edit_line == snap->document_count) {
        repl_code_panel_add_input_row(builder, -1, snap->document_count,
                                      snap->active_indent_chars,
                                      snap->document_count, NULL);
        return;
    }

    repl_code_panel_add_placeholder_row(builder, snap->document_count,
                                        snap->trailing_indent_chars);
}

static void repl_code_panel_build_rows(ReplCodePanelBuilder *builder) {
    const UiRenderSnapshot *snap;
    ReplCodePanelWalkState walk = { .primitive_vnums_exact = 1 };

    if (!builder || !builder->snap)
        return;

    snap = builder->snap;

    repl_code_panel_add_header_rows(builder);

    {
        int at = snap->display_body_start;
        if (at < 0) at = 0;
        if (at > snap->document_count) at = snap->document_count;

        for (int i = 0; i < snap->document_count; i++) {
            /* Before add_rows_for_line, so the file-scope slot and
             * display() open chrome draw in sequence before body commands. */
            if (i == at) {
                repl_code_panel_add_file_scope_slot(builder);
                repl_code_panel_add_display_open_rows(builder);
            }
            repl_code_panel_add_rows_for_line(builder, &walk, i);
        }
        /* A declarations-and-functions-only document: the whole body is
         * empty, so the frame opens after the last command and before the
         * trailing input/placeholder row. */
        if (at >= snap->document_count) {
            repl_code_panel_add_file_scope_slot(builder);
            repl_code_panel_add_display_open_rows(builder);
        }
    }

    repl_code_panel_add_trailing_document_row(builder);
    repl_code_panel_add_display_close_rows(builder);
    repl_code_panel_add_footer_rows(builder);

    builder->text_snap.row_count = builder->row_count;
}


void ui_repl_code_panel_render_with_chrome(const UiRenderSnapshot *snap,
                               UiCodePanelOutput *out) {
    ReplCodePanelBuilder builder;
    UiTextPanelOutput text_out;

    if (out) {
        out->cursor_px = 0;
        out->cursor_py = 0;
        out->cursor_valid = 0;
    }
    if (!snap || !repl_code_panel_init_builder(&builder, snap))
        return;

    glViewport(0, 0, snap->viewport.window_w, snap->viewport.window_h);

    prof_begin(PROF_CODE_PANEL_ROWS);
    repl_code_panel_build_rows(&builder);
    prof_end(PROF_CODE_PANEL_ROWS);

    g_builder_cache.snap    = snap;
    g_builder_cache.builder = builder;
    g_builder_cache.builder.text_snap.rows = g_repl_code_panel_rows;
    g_builder_cache.valid   = 1;

    memset(&text_out, 0, sizeof(text_out));
    prof_begin(PROF_CODE_PANEL_TEXT);
    ui_text_panel_render(&builder.text_snap, &text_out);
    prof_end(PROF_CODE_PANEL_TEXT);

    if (out) {
        out->cursor_px = text_out.cursor_px;
        out->cursor_py = text_out.cursor_py;
        out->cursor_valid = text_out.cursor_valid;
    }

    prof_begin(PROF_CODE_PANEL_OVERLAYS);
    gl2d_begin(snap->viewport.window_w, snap->viewport.window_h);
    /* Draw the scene tab strip before the menu bar so the closed menu row
     * stays the topmost chrome; the example dropdown is a later controller
     * overlay pass (glr_ctrl.c) and still overpaints both. */
    prof_begin(PROF_CODE_PANEL_OVERLAY_TABS);
    ui_scene_tabs_render(snap);
    prof_end(PROF_CODE_PANEL_OVERLAY_TABS);

    prof_begin(PROF_CODE_PANEL_OVERLAY_MENU);
    ui_menu_bar_render(snap);
    prof_end(PROF_CODE_PANEL_OVERLAY_MENU);

    prof_begin(PROF_CODE_PANEL_OVERLAY_SEARCH);
    ui_menu_bar_render_search_overlay(snap);
    prof_end(PROF_CODE_PANEL_OVERLAY_SEARCH);

    prof_begin(PROF_CODE_PANEL_OVERLAY_STATUS);
    repl_code_panel_statusbar_draw(snap, &text_out.statusbar_slot);
    prof_end(PROF_CODE_PANEL_OVERLAY_STATUS);

    prof_begin(PROF_CODE_PANEL_OVERLAY_PICKER);
    ui_color_picker_render(&snap->color_picker,
                           snap->viewport.window_w,
                           snap->viewport.window_h);
    prof_end(PROF_CODE_PANEL_OVERLAY_PICKER);

    prof_begin(PROF_CODE_PANEL_OVERLAY_SWATCH);
    ui_numeric_swatch_render(snap);
    prof_end(PROF_CODE_PANEL_OVERLAY_SWATCH);
    gl2d_end();
    prof_end(PROF_CODE_PANEL_OVERLAYS);
}

static UiHit repl_code_panel_rewrite_hit(const ReplCodePanelBuilder *builder,
                                         int mx, UiHit hit) {
    const UiTextPanelRow *row;
    int swatch_x;
    int swatch_w;

    if (!builder)
        return hit;
    if (hit.kind == UI_HIT_NONE || hit.kind == UI_HIT_PANEL_DIVIDER)
        return hit;
    if (hit.cmd_idx < 0 || hit.cmd_idx >= builder->row_count)
        return hit;

    row = &builder->text_snap.rows[hit.cmd_idx];
    if (row->kind == UI_TEXT_PANEL_ROW_VIRTUAL) {
        if (hit.line_idx < 0 && row->hit_target_line_idx >= 0)
            hit.line_idx = row->hit_target_line_idx;
        hit.char_idx = -1;
        if (row->hit_target_line_idx == builder->snap->display_body_start &&
            row->text && strcmp(row->text, "+ float or function") == 0) {
            hit.kind = UI_HIT_CODE_FILE_SCOPE_INSERT;
        }
    } else if (row->kind == UI_TEXT_PANEL_ROW_INPUT &&
               builder->snap->editor_input.insert_mode &&
               builder->snap->editor_input.insert_scope == EDITOR_INSERT_FILE_SCOPE &&
               row->hit_target_line_idx == builder->snap->display_body_start) {
        hit.kind = UI_HIT_CODE_FILE_SCOPE_INSERT;
        hit.line_idx = builder->snap->display_body_start;
        if (hit.char_idx < 0)
            hit.char_idx = 0;
    } else if ((row->kind == UI_TEXT_PANEL_ROW_TEXT ||
                row->kind == UI_TEXT_PANEL_ROW_INPUT) &&
               hit.kind == UI_HIT_CODE_TEXT &&
               hit.char_idx >= 0) {
        if (row->kind == UI_TEXT_PANEL_ROW_TEXT)
            hit.char_idx -= repl_code_panel_leading_ws_chars(row->text);
        hit.char_idx -= row->display_prefix_chars;
        if (hit.char_idx < 0)
            hit.char_idx = 0;
    }

    if (hit.kind != UI_HIT_CODE_TEXT || hit.visual_row != 0 ||
        !row->right_action.active)
        return hit;

    if (!ui_text_panel_right_action_rect(&builder->text_snap, 0, &swatch_x, NULL, &swatch_w))
        return hit;
    if (mx < swatch_x || mx >= swatch_x + swatch_w)
        return hit;

    hit.kind = UI_HIT_INLINE_COLOR_SWATCH;
    hit.line_idx = row->source_line_idx >= 0
        ? row->source_line_idx
        : row->hit_target_line_idx;
    hit.cmd_idx = hit.line_idx;
    hit.char_idx = -1;
    return hit;
}

static UiHit repl_code_panel_make_local_hit(
    const UiTextPanelSnapshot *text_snap, int mx, int gl_y, int kind) {
    UiHit hit = ui_hit_none();

    hit.kind = kind;
    hit.local_x = (float)(mx - text_snap->cp_x);
    hit.local_y = (float)(gl_y - text_snap->cp_y);
    return hit;
}


static int repl_code_panel_point_in_rect(int px, int py,
                                         int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw &&
           py >= ry && py < ry + rh;
}

UiHit ui_repl_code_panel_hit_test(const UiRenderSnapshot *snap,
                                  int mx, int my) {
    ReplCodePanelBuilder builder;
    UiHit hit;
    int gl_y;

    if (!snap)
        return ui_hit_none();

    if (g_builder_cache.valid && g_builder_cache.snap == snap) {
        builder = g_builder_cache.builder;
        builder.text_snap.rows = g_repl_code_panel_rows;
    } else {
        if (!repl_code_panel_init_builder(&builder, snap))
            return ui_hit_none();
        repl_code_panel_build_rows(&builder);
    }

    gl_y = builder.text_snap.vp_h - my;

    /* Statusbar strip vs. resize divider. In the TOP layout (the default)
     * the divider runs along the panel's bottom edge - exactly where the
     * strip sits - and in LEFT the full-width strip crosses the divider
     * column, so the two overlap and the order between them decides who
     * gets the grab band's panel-interior half. Three tiers:
     *
     *  1. An actionable keycap (undo/redo/copy/cut/paste/trash/focus/help)
     *     keeps every pixel of its drawn box. Their boxes start exactly
     *     UI_PANEL_DIVIDER_GRAB_PX in from the strip edge, so tier 2 would
     *     otherwise shave off their bottom row.
     *  2. The divider, ahead of the inert strip. Classified after it, the
     *     band's inner half belonged to the statusbar and the drag only
     *     started on the scene-side pixels - while the hover cursor, derived
     *     straight from divider geometry, promised the whole band.
     *  3. The rest of the strip, which consumes its clicks as chrome.
     *
     * The strip still has to beat the text rows below (tier 3 included):
     * with enough document/header rows the text-panel walk maps this strip
     * to its last visible row and steals toolbar clicks. */
    {
        int in_statusbar =
            repl_code_panel_point_in_rect(mx, gl_y,
                                          builder.text_snap.cp_x,
                                          builder.text_snap.cp_y,
                                          builder.text_snap.cp_w,
                                          builder.text_snap.cp_h) &&
            gl_y > builder.text_snap.cp_y &&
            gl_y < builder.text_snap.cp_y + STATUSBAR_H;
        int statusbar_kind = in_statusbar
            ? repl_code_panel_statusbar_hit_kind(snap, &builder.text_snap,
                                                 mx, gl_y)
            : UI_HIT_NONE;

        if (in_statusbar && statusbar_kind != UI_HIT_CODE_PANEL_CHROME)
            return repl_code_panel_make_local_hit(&builder.text_snap, mx,
                                                  gl_y, statusbar_kind);

        if (ui_text_panel_point_on_divider(&builder.text_snap, mx, gl_y)) {
            UiHit divider_hit = ui_hit_none();
            divider_hit.kind = UI_HIT_PANEL_DIVIDER;
            divider_hit.local_x = (float)mx;
            divider_hit.local_y = (float)gl_y;
            return divider_hit;
        }

        if (in_statusbar)
            return repl_code_panel_make_local_hit(&builder.text_snap, mx,
                                                  gl_y, statusbar_kind);
    }

    hit = ui_text_panel_hit_test(&builder.text_snap, mx, my);
    if (hit.kind != UI_HIT_NONE)
        return repl_code_panel_rewrite_hit(&builder, mx, hit);

    if (repl_code_panel_point_in_rect(mx, gl_y,
                                      builder.text_snap.cp_x,
                                      builder.text_snap.cp_y,
                                      builder.text_snap.cp_w,
                                      builder.text_snap.cp_h)) {
        return repl_code_panel_make_local_hit(
            &builder.text_snap, mx, gl_y,
            mx < builder.text_snap.cp_x + builder.text_snap.text_x
                ? UI_HIT_CODE_GUTTER
                : UI_HIT_CODE_TEXT);
    }
    return hit;
}

int ui_repl_code_panel_scrollbar_scroll_at(const UiRenderSnapshot *snap,
                                           int my, int grab_dy) {
    ReplCodePanelBuilder builder;
    int gl_y;

    if (!snap)
        return -1;

    if (g_builder_cache.valid && g_builder_cache.snap == snap) {
        builder = g_builder_cache.builder;
        builder.text_snap.rows = g_repl_code_panel_rows;
    } else {
        if (!repl_code_panel_init_builder(&builder, snap))
            return -1;
        repl_code_panel_build_rows(&builder);
    }

    gl_y = builder.text_snap.vp_h - my;
    return ui_text_panel_scroll_for_thumb_top(&builder.text_snap,
                                              gl_y + grab_dy);
}

int ui_repl_code_panel_source_line_point(const UiRenderSnapshot *snap,
                                         int source_line_idx,
                                         int *out_mx,
                                         int *out_my) {
    ReplCodePanelBuilder builder;
    int i;
    int py;

    if (!snap || source_line_idx < 0 ||
        !repl_code_panel_init_builder(&builder, snap))
        return 0;
    repl_code_panel_build_rows(&builder);

    for (i = 0; i < builder.row_count; i++) {
        const UiTextPanelRow *row = &builder.text_snap.rows[i];
        if (row->kind == UI_TEXT_PANEL_ROW_VIRTUAL &&
            row->text && strcmp(row->text, "+ float or function") == 0)
            continue;
        if (row->kind == UI_TEXT_PANEL_ROW_INPUT &&
            snap->editor_input.insert_mode &&
            snap->editor_input.insert_scope == EDITOR_INSERT_FILE_SCOPE)
            continue;

        int target = row->source_line_idx >= 0
                         ? row->source_line_idx
                         : row->hit_target_line_idx;

        if (!row->hit_eligible || target != source_line_idx)
            continue;
        if (!ui_text_panel_row_y(&builder.text_snap, i, &py))
            continue;

        if (out_mx)
            *out_mx = builder.text_snap.cp_x + builder.text_snap.text_x +
                      FONT_W;
        if (out_my)
            *out_my = builder.text_snap.vp_h - py;
        return 1;
    }
    return 0;
}

int ui_repl_code_panel_input_row_y(const UiRenderSnapshot *snap,
                                   float *out_py) {
    ReplCodePanelBuilder builder;
    int i, py;
    if (!snap || !out_py || !repl_code_panel_init_builder(&builder, snap))
        return 0;
    repl_code_panel_build_rows(&builder);
    for (i = 0; i < builder.row_count; i++) {
        if (builder.text_snap.rows[i].kind == UI_TEXT_PANEL_ROW_INPUT) {
            if (ui_text_panel_input_row_y(&builder.text_snap, i, &py)) {
                *out_py = (float)py;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

int ui_repl_code_panel_input_row_has_color_swatch(
    const UiRenderSnapshot *snap) {
    ReplCodePanelBuilder builder;
    int i;
    if (!snap || !repl_code_panel_init_builder(&builder, snap))
        return 0;
    repl_code_panel_build_rows(&builder);
    for (i = 0; i < builder.row_count; i++) {
        if (builder.text_snap.rows[i].kind == UI_TEXT_PANEL_ROW_INPUT)
            return builder.text_snap.rows[i].right_action.active != 0;
    }
    return 0;
}

/* Test-only: clear the render->hit-test row builder cache. Lets a test
 * exercise the cache from a known-empty initial state so a regression
 * that fails to refresh the cache becomes observable. */
void ui_repl_code_panel_invalidate_row_cache_for_test(void) {
    g_builder_cache.valid = 0;
    g_builder_cache.snap  = NULL;
}

/* Test-only: after a render or hit-test populates the row builder,
 * return the left-marker color for the row representing `source_line_idx`.
 * Returns 1 if a row was found and `out_rgba`/`out_active` were written;
 * 0 if no matching row exists. Used by the marker-priority cascade
 * regression test in tests/test_ui.c. */
/* Resolve the gutter label (the number the code panel draws in its left
 * margin) for each of `count` document line indices, writing -1 where no row
 * represents that line.
 *
 * This exists because the gutter label is NOT the document index: it counts
 * every emitted row, so with code focus off the derived-C chrome rows push
 * every user line's label up by the size of that boilerplate. Anything
 * quoting a line number to the user - the OpenGL-state popup's source column
 * - has to name the row the way the gutter names it, or the two disagree on
 * screen (measured at 100 apart on a three-line scene).
 *
 * Reading the label back off the built rows, rather than recomputing the
 * offset, is deliberate: a parallel counter would have to track chrome
 * emission, wrapping, and the placeholder rows that can consume a label
 * mid-document, and would drift the first time one of those changed.
 *
 * Batched because a cache miss rebuilds every row; callers resolving a whole
 * report must not pay that per row. */
void ui_repl_code_panel_gutter_labels_for_lines(const UiRenderSnapshot *snap,
                                                const int *source_lines,
                                                int *out_labels,
                                                int count) {
    ReplCodePanelBuilder builder;
    const UiTextPanelRow *rows;
    int row_count;
    int i, r;

    if (!out_labels || count <= 0)
        return;
    for (i = 0; i < count; i++)
        out_labels[i] = -1;
    if (!snap || !source_lines)
        return;

    if (g_builder_cache.valid && g_builder_cache.snap == snap) {
        builder = g_builder_cache.builder;
    } else {
        if (!repl_code_panel_init_builder(&builder, snap))
            return;
        repl_code_panel_build_rows(&builder);
    }
    rows = g_repl_code_panel_rows;
    row_count = builder.text_snap.row_count;
    if (row_count > UI_REPL_CODE_PANEL_MAX_ROWS)
        row_count = UI_REPL_CODE_PANEL_MAX_ROWS;

    for (r = 0; r < row_count; r++) {
        const UiTextPanelRow *row = &rows[r];
        if (row->source_line_idx < 0)
            continue;
        if (row->kind != UI_TEXT_PANEL_ROW_TEXT &&
            row->kind != UI_TEXT_PANEL_ROW_INPUT &&
            row->kind != UI_TEXT_PANEL_ROW_PLACEHOLDER)
            continue;
        for (i = 0; i < count; i++) {
            if (out_labels[i] < 0 && source_lines[i] == row->source_line_idx)
                out_labels[i] = row->left_gutter_label;
        }
    }
}

int ui_repl_code_panel_row_marker_for_test(int source_line_idx,
                                           int *out_active,
                                           float out_rgba[4]) {
    if (!g_builder_cache.valid)
        return 0;
    for (int i = 0; i < g_builder_cache.builder.text_snap.row_count; i++) {
        const UiTextPanelRow *row = &g_repl_code_panel_rows[i];
        if (row->source_line_idx != source_line_idx)
            continue;
        if (row->kind != UI_TEXT_PANEL_ROW_TEXT &&
            row->kind != UI_TEXT_PANEL_ROW_INPUT)
            continue;
        if (out_active)
            *out_active = row->left_marker_active;
        if (out_rgba) {
            out_rgba[0] = row->left_marker_color.r;
            out_rgba[1] = row->left_marker_color.g;
            out_rgba[2] = row->left_marker_color.b;
            out_rgba[3] = row->left_marker_color.a;
        }
        return 1;
    }
    return 0;
}

int ui_repl_code_panel_row_marker_bands_for_test(int source_line_idx,
                                                 int *out_active,
                                                 int *out_band_count,
                                                 float out_band_rgba[][4],
                                                 int max_bands) {
    if (!g_builder_cache.valid)
        return 0;
    for (int i = 0; i < g_builder_cache.builder.text_snap.row_count; i++) {
        const UiTextPanelRow *row = &g_repl_code_panel_rows[i];
        if (row->source_line_idx != source_line_idx)
            continue;
        if (row->kind != UI_TEXT_PANEL_ROW_TEXT &&
            row->kind != UI_TEXT_PANEL_ROW_INPUT)
            continue;
        if (out_active)
            *out_active = row->left_marker_active;
        if (out_band_count)
            *out_band_count = row->left_marker_band_count;
        if (out_band_rgba && max_bands > 0) {
            int n = row->left_marker_band_count;
            if (n > max_bands)
                n = max_bands;
            for (int b = 0; b < n; b++) {
                out_band_rgba[b][0] = row->left_marker_band_colors[b].r;
                out_band_rgba[b][1] = row->left_marker_band_colors[b].g;
                out_band_rgba[b][2] = row->left_marker_band_colors[b].b;
                out_band_rgba[b][3] = row->left_marker_band_colors[b].has_alpha
                    ? row->left_marker_band_colors[b].a : 1.0f;
            }
        }
        return 1;
    }
    return 0;
}

int ui_repl_code_panel_row_alphas_for_test(int source_line_idx,
                                           float *out_text_alpha,
                                           float *out_aux_alpha) {
    if (out_text_alpha) *out_text_alpha = 1.0f;
    if (out_aux_alpha)  *out_aux_alpha = 1.0f;
    if (!g_builder_cache.valid)
        return 0;
    for (int i = 0; i < g_builder_cache.builder.text_snap.row_count; i++) {
        const UiTextPanelRow *row = &g_repl_code_panel_rows[i];
        if (row->source_line_idx != source_line_idx)
            continue;
        if (row->kind != UI_TEXT_PANEL_ROW_TEXT &&
            row->kind != UI_TEXT_PANEL_ROW_INPUT)
            continue;
        if (out_text_alpha)
            *out_text_alpha = row->color.has_alpha ? row->color.a : 1.0f;
        /* Mirrors text_panel_draw_left_aux's own "0 means opaque" reading. */
        if (out_aux_alpha)
            *out_aux_alpha = row->left_aux_alpha > 0.0f
                ? row->left_aux_alpha : 1.0f;
        return 1;
    }
    return 0;
}

int ui_repl_code_panel_row_aux_label_for_test(int source_line_idx,
                                              char out_label[8]) {
    if (out_label)
        out_label[0] = '\0';
    if (!g_builder_cache.valid)
        return 0;
    for (int i = 0; i < g_builder_cache.builder.text_snap.row_count; i++) {
        const UiTextPanelRow *row = &g_repl_code_panel_rows[i];
        if (row->source_line_idx != source_line_idx)
            continue;
        if (row->kind != UI_TEXT_PANEL_ROW_TEXT &&
            row->kind != UI_TEXT_PANEL_ROW_INPUT)
            continue;
        if (out_label)
            snprintf(out_label, 8, "%s", row->left_aux_label);
        return 1;
    }
    return 0;
}

int ui_repl_code_panel_generated_row_style_for_test(
        const UiRenderSnapshot *snap, const char *needle,
        float out_rgb[3], int *out_segment_count) {
    ReplCodePanelBuilder builder;

    if (!snap || !needle || !needle[0] ||
        !repl_code_panel_init_builder(&builder, snap))
        return 0;
    repl_code_panel_build_rows(&builder);
    for (int i = 0; i < builder.text_snap.row_count; i++) {
        const UiTextPanelRow *row = &builder.text_snap.rows[i];
        if (row->kind != UI_TEXT_PANEL_ROW_STATIC ||
            !strstr(row->text, needle))
            continue;
        if (out_rgb) {
            out_rgb[0] = row->color.r;
            out_rgb[1] = row->color.g;
            out_rgb[2] = row->color.b;
        }
        if (out_segment_count)
            *out_segment_count = row->color_segment_count;
        return 1;
    }
    return 0;
}

int ui_repl_code_panel_row_color_segments_for_test(
        const UiRenderSnapshot *snap, int source_line_idx,
        UiTextPanelColorSegment *out, int max_out) {
    ReplCodePanelBuilder builder;

    if (!snap || !repl_code_panel_init_builder(&builder, snap))
        return -1;
    repl_code_panel_build_rows(&builder);
    for (int i = 0; i < builder.text_snap.row_count; i++) {
        const UiTextPanelRow *row = &builder.text_snap.rows[i];
        int n;
        if (row->source_line_idx != source_line_idx)
            continue;
        if (row->kind != UI_TEXT_PANEL_ROW_TEXT &&
            row->kind != UI_TEXT_PANEL_ROW_INPUT)
            continue;
        n = row->color_segment_count;
        if (out && max_out > 0) {
            int copy = n < max_out ? n : max_out;
            for (int j = 0; j < copy; j++)
                out[j] = row->color_segments[j];
        }
        return n;
    }
    return -1;
}

int ui_repl_code_panel_row_text_for_test(const UiRenderSnapshot *snap,
                                         int source_line_idx,
                                         char *out, int out_sz,
                                         int *out_display_prefix_chars) {
    ReplCodePanelBuilder builder;

    if (out && out_sz > 0)
        out[0] = '\0';
    if (out_display_prefix_chars)
        *out_display_prefix_chars = 0;
    if (!snap || !repl_code_panel_init_builder(&builder, snap))
        return 0;
    repl_code_panel_build_rows(&builder);
    for (int i = 0; i < builder.text_snap.row_count; i++) {
        const UiTextPanelRow *row = &builder.text_snap.rows[i];
        const char *text;

        if (row->source_line_idx != source_line_idx ||
            (row->kind != UI_TEXT_PANEL_ROW_TEXT &&
             row->kind != UI_TEXT_PANEL_ROW_INPUT))
            continue;
        text = row->kind == UI_TEXT_PANEL_ROW_INPUT
                   ? builder.text_snap.input.input : row->text;
        if (out && out_sz > 0)
            snprintf(out, (size_t)out_sz, "%s", text ? text : "");
        if (out_display_prefix_chars)
            *out_display_prefix_chars = row->display_prefix_chars;
        return 1;
    }
    return 0;
}
