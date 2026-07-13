#include "ui/app/repl_code_panel.h"

#include "c_compat.h"
#include "editor/state.h"
#include "repl/command_spec.h"
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
#include "subsystems/tutorial/tutorial_animation.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define UI_REPL_CODE_PANEL_MAX_STATIC_ROWS 256
#define UI_REPL_CODE_PANEL_MAX_ROWS \
    (MAX_EDITOR_COMMANDS + MAX_VIRTUAL_LINES + UI_REPL_CODE_PANEL_MAX_STATIC_ROWS + 2)
#define UI_REPL_CODE_PANEL_MAX_GENERATED_TEXT_ROWS 256

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
    [CMD_CAT_LABEL]       = { 0.85f, 0.55f, 0.85f },
    [CMD_CAT_COMMENT]     = { 0.45f, 0.50f, 0.45f },
    [CMD_CAT_GLUT_SHAPE]  = { 0.50f, 0.90f, 0.70f },
    [CMD_CAT_TESS_BLOCK]  = { 0.70f, 0.55f, 0.90f },
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
 * — distinct from text_panel.c's brighter k_clr_selection_band, which
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

/* "Code focus" hides the derived C boilerplate stanzas (workspace
 * header, includes, display() framing, render-state/camera/lights setup,
 * init()/reshape() footer). Gating both the row-count and the emit paths
 * with this single predicate keeps layout->header_rows/footer_rows
 * consistent with what build_rows actually emits. */
static int repl_code_panel_chrome_visible(const UiRenderSnapshot *snap) {
    return !(snap && snap->code_panel.code_focus);
}

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
    for (int i = 0; g_header_pre[i]; i++)
        rows += repl_code_panel_row_count(snap, g_header_pre[i], text_x, panel_w);
    for (int i = 0; g_display_header[i]; i++)
        rows += repl_code_panel_row_count(snap, g_display_header[i], text_x, panel_w);
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        rows += repl_code_panel_row_count(snap,
                                          import_export.render_state_lines[i],
                                          text_x, panel_w);
    for (int i = 0; i < snap->lights_pre_camera_count; i++)
        rows += repl_code_panel_row_count(
            snap, snap->lights_pre_camera_lines[i], text_x, panel_w);
    if (import_export.camera_comment_line[0])
        rows += repl_code_panel_row_count(snap,
                                          import_export.camera_comment_line,
                                          text_x, panel_w);
    for (int i = 0; i < REPL_EXPORT_CAMERA_LINES; i++)
        rows += repl_code_panel_row_count(snap,
                                          import_export.cam_lines[i],
                                          text_x, panel_w);
    {
        for (int i = 0; i < snap->lights_display_count; i++) {
            rows += repl_code_panel_row_count(snap, snap->lights_display_lines[i], text_x, panel_w);
        }
    }
    for (int i = 0; g_header_post[i]; i++)
        rows += repl_code_panel_row_count(snap, g_header_post[i], text_x, panel_w);
    rows += repl_code_panel_row_count(snap, REPL_CODE_PANEL_SCRATCH_DECL_LINE,
                                      text_x, panel_w);
    return rows;
}

static int repl_code_panel_footer_row_count(const UiRenderSnapshot *snap,
                                            int panel_w, int text_x) {
    int rows = 0;

    if (!repl_code_panel_chrome_visible(snap))
        return 0;

    for (int i = 0; g_footer_pre_init[i]; i++) {
        if (strcmp(g_footer_pre_init[i],
                   REPL_EXPORT_RESHAPE_PROJ_SENTINEL) == 0) {
            /* Read the frame-frozen block from the snapshot — never
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
    return repl_code_panel_row_count(snap,
                                     snap->editor_input.input,
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
        const char *display_text = repl_code_panel_display_text(snap, cmd_idx);
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

static int repl_code_panel_cursor_doc_line_from_layout(
    const UiRenderSnapshot *snap,
    int header_rows, const int *cmd_main_rows, const int *replay_extra_rows,
    int panel_w, int text_x) {
    /* The three former branches (insert mode / in-range edit line /
     * out-of-range fallback) all summed the same prefix and then added
     * the identical cursor-row term. The prefix length is the same in
     * every case: min(edit_line, document_count) — insert mode clamps
     * to both bounds; non-insert uses edit_line when it is
     * < document_count, otherwise document_count, which is that same
     * min. So the dispatch was redundant. */
    int prefix = (snap->edit_line < snap->document_count)
                     ? snap->edit_line : snap->document_count;

    return header_rows +
           repl_code_panel_rows_before_cmd(cmd_main_rows, replay_extra_rows,
                                           prefix) +
           repl_code_panel_cursor_row(snap,
                                      snap->editor_input.input,
                                      repl_code_panel_input_first_x(snap, text_x),
                                      panel_w,
                                      snap->editor_input.cursor_pos,
                                      NULL, NULL, NULL);
}

static int repl_code_panel_follow_doc_line_from_layout(
    const UiRenderSnapshot *snap,
    int cursor_doc_line, int header_rows, const int *cmd_main_rows,
    const int *replay_extra_rows) {
    int src_line = snap->replay.src_line_idx;

    if (!(snap->replay.active &&
          src_line >= 0 && src_line < snap->document_count))
        return cursor_doc_line;

    return header_rows +
           repl_code_panel_rows_before_cmd(cmd_main_rows, replay_extra_rows,
                                           src_line) +
           repl_code_panel_last_row_offset_for_cmd(cmd_main_rows,
                                                   replay_extra_rows,
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
    repl_code_panel_precompute_layout_rows(snap, panel_w, text_x,
                                           layout->cmd_main_rows,
                                           layout->replay_extra_rows);

    total_lines = layout->header_rows + layout->footer_rows +
                  repl_code_panel_trailing_row_count(snap, panel_w, text_x);
    for (int i = 0; i < snap->document_count; i++) {
        if (snap->editor_input.insert_mode && i == snap->edit_line)
            total_lines += repl_code_panel_insert_rows(snap, panel_w, text_x);
        total_lines += layout->cmd_main_rows[i];
        total_lines += layout->replay_extra_rows[i];
    }
    layout->total_lines = total_lines;

    layout->cursor_doc_line = repl_code_panel_cursor_doc_line_from_layout(
        snap,
        layout->header_rows, layout->cmd_main_rows, layout->replay_extra_rows,
        panel_w, text_x);
    layout->follow_doc_line = repl_code_panel_follow_doc_line_from_layout(
        snap,
        layout->cursor_doc_line, layout->header_rows, layout->cmd_main_rows,
        layout->replay_extra_rows);
}

int ui_repl_code_panel_target_for_doc_line(const UiRenderSnapshot *snap,
                                           int doc_line,
                                           const UiReplCodePanelLayout *layout,
                                           int *out_target,
                                           int *out_on_insert_line,
                                           int *out_row_offset) {
    int row;

    if (!snap || !layout)
        return 0;

    row = doc_line - layout->header_rows;
    if (row < 0)
        return 0;

    /* Consume rows in the same order the panel emits them after the
     * header chrome: optional insert row, command body rows, replay
     * virtual rows, then the trailing newline/input slot. */
    for (int cmd_idx = 0; cmd_idx <= snap->document_count; cmd_idx++) {
        if (snap->editor_input.insert_mode && cmd_idx == snap->edit_line) {
            int insert_rows = repl_code_panel_insert_rows(snap, layout->panel_w,
                                                          layout->text_x);
            if (row < insert_rows)
                return repl_code_panel_return_target_result(out_target,
                                                            out_on_insert_line,
                                                            out_row_offset,
                                                            -1, 1, row);
            row -= insert_rows;
        }

        if (cmd_idx < snap->document_count) {
            int main_rows = layout->cmd_main_rows[cmd_idx];
            if (row < main_rows)
                return repl_code_panel_return_target_result(out_target,
                                                            out_on_insert_line,
                                                            out_row_offset,
                                                            cmd_idx, 0, row);
            row -= main_rows;

            if (row < layout->replay_extra_rows[cmd_idx])
                return repl_code_panel_return_target_result(out_target,
                                                            out_on_insert_line,
                                                            out_row_offset,
                                                            cmd_idx, 0, 0);
            row -= layout->replay_extra_rows[cmd_idx];
        } else {
            int newline_rows = repl_code_panel_trailing_row_count(snap, layout->panel_w,
                                                            layout->text_x);
            if (row < newline_rows)
                return repl_code_panel_return_target_result(out_target,
                                                            out_on_insert_line,
                                                            out_row_offset,
                                                            snap->document_count,
                                                            0, row);
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

int ui_repl_code_panel_compute_text_x(const UiRenderSnapshot *snap) {
    if (!snap)
        return 0;
    int linenum_w = 4 * FONT_W;
    int idx_col_w = snap->code_panel.show_vertex_indices ? (6 * FONT_W) : 0;
    int idx_x = CODE_MARGIN_X + linenum_w + FONT_W;
    return idx_x + idx_col_w;
}

static int repl_code_panel_init_builder(ReplCodePanelBuilder *builder,
                                        const UiRenderSnapshot *snap) {
    int cp_x;
    int cp_y;
    int cp_w;
    int cp_h;

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

    builder->text_snap = (UiTextPanelSnapshot){
        .vp_w = snap->viewport.window_w,
        .vp_h = snap->viewport.window_h,
        .cp_x = cp_x,
        .cp_y = cp_y,
        .cp_w = cp_w,
        .cp_h = cp_h,
        .text_x = ui_repl_code_panel_compute_text_x(snap),
        .wrap_at_comma = snap->code_panel.wrap_at_comma,
        .paren_match = snap->code_panel.paren_match,
        .paren_scope = snap->code_panel.paren_scope,
        .top_chrome_h = ui_scene_tabs_band_h(snap),
        .statusbar_h = 22,
        .rows = g_repl_code_panel_rows,
        .row_count = 0,
        .scroll = snap->scroll.scroll,
        .chrome_flags = UI_TEXT_PANEL_CHROME_STATUSBAR |
                        UI_TEXT_PANEL_CHROME_SCROLLBAR |
                        UI_TEXT_PANEL_CHROME_LINE_NUMS |
                        (snap->code_panel.show_vertex_indices
                            ? UI_TEXT_PANEL_CHROME_AUX_COL
                            : 0),
        .input = {
            .input = snap->editor_input.input ? snap->editor_input.input : "",
            .input_len = snap->editor_input.input_len,
            .cursor = snap->editor_input.cursor_pos,
            .anchor = snap->editor_input.anchor_pos,
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
    if (!row || !snap || !snap->code_panel.show_vertex_indices || !is_vertex)
        return;
    if (!repl_code_panel_line_in_current_begin_block(snap, line_idx))
        return;

    snprintf(row->left_aux_label, sizeof(row->left_aux_label),
             primitive_vnums_exact ? "v%d" : "vn", vnum);
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
    MARKER_PRIORITY_MATCHING_PUSH,
    MARKER_PRIORITY_FEEDING_NORMAL,
    MARKER_PRIORITY_FEEDING_COLOR,
    /* Above the feeding markers: the affecting-transform set now resolves
     * through the flat program and can land on a line that also carries a
     * feeding-color/normal marker for a different cursor query, so it must
     * win rather than be masked (req 4). */
    MARKER_PRIORITY_AFFECTING_TRANSFORM,
    MARKER_PRIORITY_UNBALANCED,
    MARKER_PRIORITY_TUTORIAL_INSERTION
} MarkerPriority;

/* Decorate one code-panel row with its background band (replay PC /
 * line selection) and at most one left-edge marker. Every marker
 * source is tested independently against the snapshot, and ties
 * resolve through the MarkerPriority ladder above — a sequence of
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

    /* Replay call-site markers (cyan family) — the funcN(...) line(s) whose
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
            /* Vivid amber — deliberately redder/more saturated than the pale
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

    if (repl_code_panel_line_is_unbalanced(builder->snap, line_idx)) {
        if (MARKER_PRIORITY_UNBALANCED > priority) {
            priority = MARKER_PRIORITY_UNBALANCED;
            /* Warning red — distinct from the violet match / orange
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

    if (priority > MARKER_PRIORITY_NONE) {
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
     * ramping 0 → 1 across its slot. */
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

/* Argument shades are derived from the command's class color *only* — no
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
             * skipping spaces) keeps the class color — covers the command
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
                /* REPL/C keyword, type, or math fn used bare — structural */
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
 * — which stop at the '//' — so the segments stay ordered and disjoint;
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
    row->hit_eligible = 1;
    if (aux_label && aux_label[0])
        snprintf(row->left_aux_label, sizeof(row->left_aux_label), "%s", aux_label);
    repl_code_panel_apply_tutorial_insertion_marker(builder, hit_target_line_idx,
                                                    row);

    if (source_line_idx >= 0) {
        repl_code_panel_apply_command_overlays(builder, source_line_idx, row);
        repl_code_panel_set_right_action(row, builder->snap, source_line_idx);
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

    if (!row)
        return;

    display_text = repl_code_panel_display_text(builder->snap, line_idx);
    row->text = display_text;
    row->kind = UI_TEXT_PANEL_ROW_TEXT;
    row->left_gutter_label = builder->file_line++;
    row->source_line_idx = line_idx;
    row->search_row_idx = repl_code_panel_search_row_for_cmd(builder->snap,
                                                             line_idx);
    row->color = repl_code_panel_category_color(
        builder->snap->document_cmds[line_idx].type);
    row->hit_eligible = 1;
    repl_code_panel_set_vertex_label(row, builder->snap, line_idx, is_vertex, vnum,
                                     primitive_vnums_exact);
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
    }
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

        snprintf(text, MAX_LINE_LEN * 2, "%s%s", virtual_line->text, virtual_line->aux);
        row->text = text;
        row->kind = UI_TEXT_PANEL_ROW_VIRTUAL;
        row->hit_target_line_idx = after_line_idx;
        row->hit_eligible = after_line_idx >= 0;

        if (virtual_line->style == VIRTUAL_STYLE_REPLAY_SUBST) {
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

static void repl_code_panel_add_static_buffer_lines(
    ReplCodePanelBuilder *builder,
    int count,
    size_t line_size,
    const char lines[count][line_size]) {
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
    repl_code_panel_add_static_null_terminated_lines(
        builder, g_header_pre);
    repl_code_panel_add_static_null_terminated_lines(
        builder, g_display_header);

    repl_code_panel_add_static_buffer_lines(
        builder,
        RENDER_STATE_LINE_COUNT,
        sizeof(snap->import_export.render_state_lines[0]),
        snap->import_export.render_state_lines);
    repl_code_panel_add_static_buffer_lines(
        builder,
        snap->lights_pre_camera_count,
        sizeof(snap->lights_pre_camera_lines[0]),
        snap->lights_pre_camera_lines);
    if (snap->import_export.camera_comment_line[0]) {
        repl_code_panel_add_static_row(
            builder,
            snap->import_export.camera_comment_line);
    }
    repl_code_panel_add_static_buffer_lines(
        builder,
        REPL_EXPORT_CAMERA_LINES,
        sizeof(snap->import_export.cam_lines[0]),
        snap->import_export.cam_lines);
    repl_code_panel_add_static_buffer_lines(
        builder,
        snap->lights_display_count,
        sizeof(snap->lights_display_lines[0]),
        snap->lights_display_lines);
    repl_code_panel_add_static_null_terminated_lines(
        builder, g_header_post);

    /* Scratch decoration row: panel-only (the exporter emits the arrays as
     * file-scope statics on demand instead). Keep it adjacent to the user
     * source so its role is clear. */
    repl_code_panel_add_static_row(
        builder, REPL_CODE_PANEL_SCRATCH_DECL_LINE);
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
    if (!snap || !state || !snap->code_panel.show_vertex_indices || !is_vertex)
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

    if (snap->editor_input.insert_mode && line_idx == snap->edit_line) {
        repl_code_panel_add_input_row(builder, -1, snap->edit_line,
                                      snap->active_indent_chars, line_idx,
                                      NULL);
    }

    repl_code_panel_begin_walk_line(state, cmd);

    is_edit = (!snap->editor_input.insert_mode && line_idx == snap->edit_line);
    is_vertex = cmd->valid && repl_cmd_emits_vertex(cmd->type);
    repl_code_panel_vertex_aux_label(snap, state, line_idx, is_vertex, aux_label);

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

    for (int i = 0; i < snap->document_count; i++)
        repl_code_panel_add_rows_for_line(builder, &walk, i);

    repl_code_panel_add_trailing_document_row(builder);
    repl_code_panel_add_footer_rows(builder);

    builder->text_snap.row_count = builder->row_count;
}

static void repl_code_panel_statusbar_sep(int *tx, int sy, int sh) {
    *tx += 8;
    ui_clr(UI_TOK_DIVIDER);
    glBegin(GL_LINES);
    glVertex2f((float)*tx, (float)(sy + 4));
    glVertex2f((float)*tx, (float)(sy + sh - 4));
    glEnd();
    *tx += 8;
}

/* Right-aligned statusbar cluster: compact editor action chips
 * (undo/redo/copy/cut/clear), a "[focus] focus" keycap+label, and
 * the existing "[F1] help" keycap+label. The geometry is derived once
 * here so the renderer and the hit-test agree on the clickable chip
 * boxes (window / GL coords, bottom-left origin), with no arithmetic
 * duplicated across the two passes. */
static const char *k_statusbar_help_kbd  = "F1";
static const char *k_statusbar_help_lbl  = "help";
static const char *k_statusbar_focus_lbl = "focus";

/* The focus keycap reads "^⇧F". '^' and 'F' are font glyphs; the ⇧
 * shift symbol has no bitmap-font glyph so it is line-drawn into the
 * middle cell. Width is therefore a fixed 3-cell count rather than a
 * strlen (a UTF-8 "⇧" would mis-measure). */
#define STATUSBAR_FOCUS_KBD_CELLS 3

/* The left-aligned status segments ("N/M cmds", "Ln …", optional
 * "AA …"), built once so the renderer (drawing) and the hints
 * (collision test) share one width. `right_edge` is the window-x just
 * past the last left glyph. STATUSBAR_SEP_W mirrors the two +8 nudges
 * in repl_code_panel_statusbar_sep. */
#define STATUSBAR_SEP_W 16

typedef struct {
    char cmds[48];
    char cost[32];
    char line[64];
    char aa[32];
    char unbal[32];
    int  cmds_w;
    int  cost_w;
    int  line_w;
    int  aa_w;
    int  unbal_w;
    int  has_cost;
    int  has_aa;
    int  has_unbal;
    int  right_edge;
} ReplStatusbarLeft;

static ReplStatusbarLeft repl_code_panel_statusbar_left(
        const UiRenderSnapshot *snap, int sx) {
    ReplStatusbarLeft L;
    int tx = sx + CODE_MARGIN_X;
    int edit_line = snap->edit_line;

    snprintf(L.cmds, sizeof L.cmds, "%d/%d cmds",
             snap->flat_program_count, MAX_FLAT_COMMANDS);
    L.cmds_w = (int)strlen(L.cmds) * FONT_SMALL_W;
    tx += L.cmds_w;
    tx += STATUSBAR_SEP_W;

    if (snap->editor_input.insert_mode)
        snprintf(L.line, sizeof L.line, "Ln %d [INSERT]", edit_line + 1);
    else if (snap->in_begin_block)
        snprintf(L.line, sizeof L.line, "Ln %d  %s",
                 edit_line + 1, repl_mode_name(snap->current_begin_mode));
    else
        snprintf(L.line, sizeof L.line, "Ln %d", edit_line + 1);
    L.line_w = (int)strlen(L.line) * FONT_SMALL_W;
    tx += L.line_w;

    L.has_aa = snap->render.use_accum ? 1 : 0;
    if (L.has_aa) {
        tx += STATUSBAR_SEP_W;
        if (snap->render.accum_effect != RENDER3D_ACCUM_EFFECT_OFF &&
            snap->render.accum_passes > 1)
            snprintf(L.aa, sizeof L.aa, "%s %dx",
                     RENDER3D_ACCUM_EFFECT_IS_BLUR(snap->render.accum_effect)
                         ? "Blur" : "AA",
                     snap->render.accum_passes);
        else
            snprintf(L.aa, sizeof L.aa, "AA off");
        L.aa_w = (int)strlen(L.aa) * FONT_SMALL_W;
        tx += L.aa_w;
    } else {
        L.aa[0] = '\0';
        L.aa_w = 0;
    }

    L.has_unbal = snap->unbalanced_count > 0;
    if (L.has_unbal) {
        tx += STATUSBAR_SEP_W;
        snprintf(L.unbal, sizeof L.unbal, "%d unbalanced", snap->unbalanced_count);
        L.unbal_w = (int)strlen(L.unbal) * FONT_SMALL_W;
        tx += L.unbal_w;
    } else {
        L.unbal[0] = '\0';
        L.unbal_w = 0;
    }

    /* Cursor budget readout ("fn cmds 2480", "scope cmds 230"): how
     * much of the flat budget the cursor's scope spends. Kept LAST in
     * the left cluster — it appears/disappears and changes width as
     * the cursor moves, so anything placed after it would jitter. The
     * controller leaves the label empty when there's nothing worth
     * showing (plain lines costing <= 1, comments, empty buffer). */
    L.has_cost = snap->cursor_cost_label[0] != '\0';
    if (L.has_cost) {
        tx += STATUSBAR_SEP_W;
        snprintf(L.cost, sizeof L.cost, "%s cmds %d",
                 snap->cursor_cost_label, snap->cursor_cost_count);
        L.cost_w = (int)strlen(L.cost) * FONT_SMALL_W;
        tx += L.cost_w;
    } else {
        L.cost[0] = '\0';
        L.cost_w = 0;
    }

    L.right_edge = tx;
    return L;
}

typedef struct {
    int text_y;
    int help_kx, help_lbl_x, help_kw;
    int focus_kx, focus_lbl_x, focus_kw;
    int trash_kx, trash_kw;
    int copy_kx, copy_kw;
    int cut_kx, cut_kw;
    int undo_kx, undo_kw;
    int redo_kx, redo_kw;
    int ky, kh;                 /* keycap box y / h (shared) */
    int trash_visible;          /* 0 when it would collide with left text */
    int copy_visible;
    int cut_visible;
    int undo_visible;
    int redo_visible;
    int focus_visible;          /* 0 when it would collide with left text */
    int help_visible;
} ReplStatusbarHints;

static ReplStatusbarHints repl_code_panel_statusbar_hints(
        const UiRenderSnapshot *snap, int sx, int sy, int sw, int sh) {
    ReplStatusbarHints h;
    int help_lbl_w  = (int)strlen(k_statusbar_help_lbl)  * FONT_SMALL_W;
    int focus_lbl_w = (int)strlen(k_statusbar_focus_lbl) * FONT_SMALL_W;
    int left_end    = repl_code_panel_statusbar_left(snap, sx).right_edge;
    int gap         = FONT_SMALL_W;   /* one cell of breathing room */

    h.text_y     = sy + (sh - FONT_SMALL_H) / 2 + 1;
    h.ky         = sy + 3;
    h.kh         = sh - 6;

    h.help_kw    = (int)strlen(k_statusbar_help_kbd) * FONT_SMALL_W + 10;
    h.help_lbl_x = sx + sw - CODE_MARGIN_X - help_lbl_w;
    h.help_kx    = h.help_lbl_x - h.help_kw - 6;

    h.focus_kw    = STATUSBAR_FOCUS_KBD_CELLS * FONT_SMALL_W + 10;
    h.focus_lbl_x = h.help_kx - 12 - focus_lbl_w;
    h.focus_kx    = h.focus_lbl_x - h.focus_kw - 6;
    h.trash_kw    = h.kh + 4;
    h.trash_kx    = h.focus_kx - 12 - h.trash_kw;
    h.cut_kw      = h.kh + 4;
    h.cut_kx      = h.trash_kx - 6 - h.cut_kw;
    h.copy_kw     = h.kh + 4;
    h.copy_kx     = h.cut_kx - 4 - h.copy_kw;
    h.redo_kw     = h.kh + 4;
    h.redo_kx     = h.copy_kx - 6 - h.redo_kw;
    h.undo_kw     = h.kh + 4;
    h.undo_kx     = h.redo_kx - 4 - h.undo_kw;

    /* The right cluster is always drawn from the right edge; the left
     * text from the left. On narrow/default panels they collide, so
     * suppress whichever right chip the left text would reach. Keyboard
     * shortcuts still work when a chip is hidden. */
    h.help_visible  = (h.help_kx  >= left_end + gap);
    h.focus_visible = (h.focus_kx >= left_end + gap);
    h.trash_visible = (h.trash_kx >= left_end + gap);
    h.cut_visible   = (h.cut_kx   >= left_end + gap);
    h.copy_visible  = (h.copy_kx  >= left_end + gap);
    h.redo_visible  = (h.redo_kx  >= left_end + gap);
    h.undo_visible  = (h.undo_kx  >= left_end + gap);
    return h;
}

static int repl_code_panel_point_in_rect(int px, int py,
                                         int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw &&
           py >= ry && py < ry + rh;
}

/* A sunken keycap chip (box + divider border) using theme tokens. The
 * caller draws the centred key glyphs afterwards with its own color so
 * the focus chip can tint them by ON/OFF state. */
static void repl_code_panel_draw_keycap(int kx, int ky, int kw, int kh) {
    ui_clr(UI_TOK_SUNKEN);
    glRectf((float)kx, (float)ky, (float)(kx + kw), (float)(ky + kh));
    ui_clr(UI_TOK_DIVIDER);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)kx, (float)ky);
    glVertex2f((float)(kx + kw), (float)ky);
    glVertex2f((float)(kx + kw), (float)(ky + kh));
    glVertex2f((float)kx, (float)(ky + kh));
    glEnd();
}

/* The ⇧ shift symbol as an 8x13 1bpp bitmap, drawn with glBitmap at
 * raster (cx, gy) using the same cell metrics and 2px descent origin
 * as GLUT_BITMAP_8_BY_13 so it sits on the same baseline as the
 * adjacent '^' and 'F' font glyphs. Caller sets the color. */
static void repl_code_panel_draw_shift_glyph(int cx, int gy) {
    /* Rows are bottom-to-top (glBitmap scan order); bit 0x80 = leftmost
     * pixel. Rows 0-2 are blank descent; an *outlined* up-arrow
     * occupies rows 3-12 (hollow triangle head over a hollow stem). */
    static const GLubyte shift_bits[13] = {
        0x00, 0x00, 0x00,  /* rows 0-2   (baseline / descent)        */
        0x3C,              /* row 3   stem foot   ..####..           */
        0x24,              /* row 6               ..#..#..           */
        0x24,              /* row 7               ..#..#..           */
        0xE7,              /* row 8   shoulders   ###..###           */
        0x81,              /* row 9   head edge   #......#           */
        0x42,              /* row 10              .#....#.           */
        0x24,              /* row 11              ..#..#..           */
        0x18               /* row 12  apex        ...##...           */
    };
    GLint prev_align = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glRasterPos2f((float)cx, (float)gy);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);   /* rows are 1 byte each */
    glBitmap(8, 13, 0.0f, 2.0f, 0.0f, 0.0f, shift_bits);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
}

/* Draw the focus keycap glyphs "^⇧F" across three FONT_SMALL cells
 * starting at (gx, gy). Color is set by the caller. */
static void repl_code_panel_draw_focus_kbd(int gx, int gy) {
    char ch[2] = { '^', 0 };
    gl2d_draw_string((float)gx, (float)gy, ch, FONT_SMALL);
    repl_code_panel_draw_shift_glyph(gx + FONT_SMALL_W, gy);
    ch[0] = 'F';
    gl2d_draw_string((float)(gx + 2 * FONT_SMALL_W), (float)gy, ch, FONT_SMALL);
}

#define TRASH_ICON_W 13
#define TRASH_ICON_H 12
#define ACTION_ICON_W 13
#define ACTION_ICON_H 12

/* Centre an icon_w x icon_h 1bpp glyph in the (kx,ky,kw,kh) keycap box
 * and draw it with glBitmap. Shared by the trash / undo / redo / copy /
 * cut statusbar chips so each glyph is just a bit table. */
static void repl_code_panel_draw_bitmap_icon(int kx, int ky, int kw, int kh,
                                             int icon_w, int icon_h,
                                             const GLubyte *bits) {
    GLint prev_align = 4;
    int rx = kx + (kw - icon_w) / 2;
    int ry = ky + (kh - icon_h) / 2;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glRasterPos2f((float)rx, (float)ry);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBitmap(icon_w, icon_h, 0.0f, 0.0f, 0.0f, 0.0f, bits);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
}

static void repl_code_panel_draw_trash_icon(int kx, int ky, int kw, int kh) {
    /* 13×12  1bpp trash-can glyph, drawn with glBitmap so it matches
     * the crisp pixel-art style of the ⇧ shift glyph and the bitmap
     * font.  Rows are bottom-to-top (glBitmap scan order); bit 0x80
     * of byte 0 = leftmost pixel (col 0).  The three interior slats
     * are inset a pixel from both rims so they read as ribs inside
     * the can, not bars fused to it.
     *
     *  row 11:  ....#####....   handle tab  (cols 4-8)
     *  row 10:  .###########.   lid         (cols 1-11, overhangs body)
     *  row 9:   .............   lid / body gap
     *  row 8:   ..#########..   body top    (cols 2-10)
     *  row 7:   ..#.......#..   walls only (slat inset)
     *  row 6:   ..#.#.#.#.#..   walls + 3 interior slats
     *  row 5:   ..#.#.#.#.#..
     *  row 4:   ..#.#.#.#.#..
     *  row 3:   ..#.#.#.#.#..
     *  row 2:   ..#.#.#.#.#..
     *  row 1:   ..#.......#..   walls only (slat inset)
     *  row 0:   ..#########..   body bottom (cols 2-10)               */
    static const GLubyte trash_bits[TRASH_ICON_H * 2] = {
        0x3F, 0xE0,  /* row 0  body bottom  ..#########..            */
        0x20, 0x20,  /* row 1  walls        ..#.......#..            */
        0x2A, 0xA0,  /* row 2  slats        ..#.#.#.#.#..            */
        0x2A, 0xA0,  /* row 3  slats        ..#.#.#.#.#..            */
        0x2A, 0xA0,  /* row 4  slats        ..#.#.#.#.#..            */
        0x2A, 0xA0,  /* row 5  slats        ..#.#.#.#.#..            */
        0x2A, 0xA0,  /* row 6  slats        ..#.#.#.#.#..            */
        0x20, 0x20,  /* row 7  walls        ..#.......#..            */
        0x3F, 0xE0,  /* row 8  body top     ..#########..            */
        0x00, 0x00,  /* row 9  lid gap                               */
        0x7F, 0xF0,  /* row 10 lid          .###########.            */
        0x0F, 0x80   /* row 11 handle       ....#####....            */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     TRASH_ICON_W, TRASH_ICON_H, trash_bits);
}

static void repl_code_panel_draw_undo_icon(int kx, int ky, int kw, int kh) {
    /* 13x12 1bpp "return hook" glyph: a riser down the right side that
     * bends into a leftward-pointing arrowhead, i.e. the same shape as
     * the redo icon mirrored left-right.
     *
     *  row 11:  .........##..   riser
     *  row 10:  .........##..
     *  row 9:   .........##..
     *  row 8:   .........##..
     *  row 7:   ....#....##..   arrowhead tip taper
     *  row 6:   ...##....##..
     *  row 5:   .##########..   shaft + arrowhead point
     *  row 4:   .##########..
     *  row 3:   ...##........   arrowhead tip taper
     *  row 2:   ....#........
     *  row 1:   .............
     *  row 0:   .............                                        */
    static const GLubyte undo_bits[ACTION_ICON_H * 2] = {
        0x00, 0x00,  /* row 0  */
        0x00, 0x00,  /* row 1  */
        0x08, 0x00,  /* row 2  */
        0x18, 0x00,  /* row 3  */
        0x7F, 0xE0,  /* row 4  */
        0x7F, 0xE0,  /* row 5  */
        0x18, 0x60,  /* row 6  */
        0x08, 0x60,  /* row 7  */
        0x00, 0x60,  /* row 8  */
        0x00, 0x60,  /* row 9  */
        0x00, 0x60,  /* row 10 */
        0x00, 0x60   /* row 11 */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, undo_bits);
}

static void repl_code_panel_draw_redo_icon(int kx, int ky, int kw, int kh) {
    /* Mirror image of the undo glyph: riser down the left side bending
     * into a rightward-pointing arrowhead.
     *
     *  row 11:  ..##.........
     *  row 10:  ..##.........
     *  row 9:   ..##.........
     *  row 8:   ..##.........
     *  row 7:   ..##....#....
     *  row 6:   ..##....##...
     *  row 5:   ..##########.
     *  row 4:   ..##########.
     *  row 3:   ........##...
     *  row 2:   ........#....
     *  row 1:   .............
     *  row 0:   .............                                        */
    static const GLubyte redo_bits[ACTION_ICON_H * 2] = {
        0x00, 0x00,  /* row 0  */
        0x00, 0x00,  /* row 1  */
        0x00, 0x80,  /* row 2  */
        0x00, 0xC0,  /* row 3  */
        0x3F, 0xF0,  /* row 4  */
        0x3F, 0xF0,  /* row 5  */
        0x30, 0xC0,  /* row 6  */
        0x30, 0x80,  /* row 7  */
        0x30, 0x00,  /* row 8  */
        0x30, 0x00,  /* row 9  */
        0x30, 0x00,  /* row 10 */
        0x30, 0x00   /* row 11 */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, redo_bits);
}

static void repl_code_panel_draw_copy_icon(int kx, int ky, int kw, int kh) {
    /* Two stacked portrait pages: the front sheet (bottom-left, 7x9
     * outline) occludes the back sheet, whose top-left corner and
     * right/bottom edges peek out behind it with a 1px gap all round
     * so the outlines never fuse.
     *
     *  row 11:  .....#######.   back page top
     *  row 10:  .....#.....#.
     *  row 9:   ...........#.
     *  row 8:   .#######...#.   front page top
     *  row 7:   .#.....#...#.
     *  row 6:   .#.....#...#.
     *  row 5:   .#.....#...#.
     *  row 4:   .#.....#...#.
     *  row 3:   .#.....#.###.   back page bottom (visible part)
     *  row 2:   .#.....#.....
     *  row 1:   .#.....#.....
     *  row 0:   .#######.....   front page bottom                    */
    static const GLubyte copy_bits[ACTION_ICON_H * 2] = {
        0x7F, 0x00,  /* row 0  */
        0x41, 0x00,  /* row 1  */
        0x41, 0x00,  /* row 2  */
        0x41, 0x70,  /* row 3  */
        0x41, 0x10,  /* row 4  */
        0x41, 0x10,  /* row 5  */
        0x41, 0x10,  /* row 6  */
        0x41, 0x10,  /* row 7  */
        0x7F, 0x10,  /* row 8  */
        0x00, 0x10,  /* row 9  */
        0x04, 0x10,  /* row 10 */
        0x07, 0xF0   /* row 11 */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, copy_bits);
}

static void repl_code_panel_draw_cut_icon(int kx, int ky, int kw, int kh) {
    /* Scissors: two 2px blades crossing in an X, tapering to pointed
     * tips at the top; handles diverge below the pivot into open
     * finger-loop rings that connect to the handle ends.
     *
     *  row 11:  .#.........#.   blade tips (pointed)
     *  row 10:  ..##.....##..
     *  row 9:   ...##...##...
     *  row 8:   ....##.##....
     *  row 7:   .....###.....
     *  row 6:   ......#......   pivot
     *  row 5:   .....#.#.....
     *  row 4:   ....#...#....   handles
     *  row 3:   ..###...###..   ring tops (join handles at cols 4 / 8)
     *  row 2:   .#...#.#...#.
     *  row 1:   .#...#.#...#.
     *  row 0:   ..###...###..   ring bottoms                          */
    static const GLubyte cut_bits[ACTION_ICON_H * 2] = {
        0x38, 0xE0,  /* row 0  */
        0x45, 0x10,  /* row 1  */
        0x45, 0x10,  /* row 2  */
        0x38, 0xE0,  /* row 3  */
        0x08, 0x80,  /* row 4  */
        0x05, 0x00,  /* row 5  */
        0x02, 0x00,  /* row 6  */
        0x07, 0x00,  /* row 7  */
        0x0D, 0x80,  /* row 8  */
        0x18, 0xC0,  /* row 9  */
        0x30, 0x60,  /* row 10 */
        0x40, 0x10   /* row 11 */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, cut_bits);
}

static void repl_code_panel_draw_statusbar(const UiRenderSnapshot *snap,
                                           const UiTextPanelRect *slot) {
    int sy;
    int sh;
    int cp_x;
    int cp_w;

    if (!snap || !slot || slot->h <= 0)
        return;

    sy = slot->y;
    sh = slot->h;
    cp_x = slot->x;
    cp_w = slot->w;

    glPushAttrib(GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Hard backstop: clip every statusbar primitive to the panel slot.
     * The left status text ("N/M cmds", "Ln …", "AA …") is laid out
     * left-to-right with no width clamp, so on a narrow split-layout
     * panel it would otherwise bleed past the panel's right edge onto
     * the 3D scene. gl2d uses gluOrtho2D(0,w,0,h) over the full-window
     * viewport, so slot coords map 1:1 to scissor window coords.
     * GL_CURRENT_BIT does not save scissor state, so it is disabled
     * explicitly below before the color picker (same gl2d pass) draws. */
    if (cp_w > 0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(cp_x, sy, cp_w, sh);
    }

    ui_clr_a(UI_TOK_SURFACE, 0.98f);
    glRectf((float)cp_x, (float)sy,
            (float)(cp_x + cp_w), (float)(sy + sh));
    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);   /* deliberate hard bottom edge */
    glBegin(GL_LINES);
    glVertex2f((float)cp_x, (float)(sy + sh));
    glVertex2f((float)(cp_x + cp_w), (float)(sy + sh));
    glEnd();

    {
        ReplStatusbarHints h =
            repl_code_panel_statusbar_hints(snap, cp_x, sy, cp_w, sh);
        ReplStatusbarLeft L = repl_code_panel_statusbar_left(snap, cp_x);
        int text_y = h.text_y;
        int tx = cp_x + CODE_MARGIN_X;

        ui_clr(UI_TOK_TEXT_PRIMARY);
        gl2d_draw_string((float)tx, (float)text_y, L.cmds, FONT_SMALL);
        tx += L.cmds_w;

        repl_code_panel_statusbar_sep(&tx, sy, sh);

        ui_clr(UI_TOK_TEXT_MUTED);
        gl2d_draw_string((float)tx, (float)text_y, L.line, FONT_SMALL);
        tx += L.line_w;

        if (L.has_aa) {
            repl_code_panel_statusbar_sep(&tx, sy, sh);
            ui_clr(UI_TOK_TEXT_MUTED);
            gl2d_draw_string((float)tx, (float)text_y, L.aa, FONT_SMALL);
            tx += L.aa_w;
        }

        if (L.has_unbal) {
            repl_code_panel_statusbar_sep(&tx, sy, sh);
            ui_clr(UI_TOK_STATUS_WARN);
            gl2d_draw_string((float)tx, (float)text_y, L.unbal, FONT_SMALL);
            tx += L.unbal_w;
        }

        if (L.has_cost) {
            repl_code_panel_statusbar_sep(&tx, sy, sh);
            ui_clr(UI_TOK_ACCENT);
            gl2d_draw_string((float)tx, (float)text_y, L.cost, FONT_SMALL);
            tx += L.cost_w;
        }

        /* Right cluster, drawn from the right edge. Each chip is
         * suppressed when the left status text would collide with it;
         * keyboard shortcuts still work when a chip is hidden. */
        if (h.undo_visible) {
            repl_code_panel_draw_keycap(h.undo_kx, h.ky, h.undo_kw, h.kh);
            ui_clr(snap->can_undo ? UI_TOK_TEXT_PRIMARY : UI_TOK_TEXT_MUTED);
            repl_code_panel_draw_undo_icon(h.undo_kx, h.ky,
                                           h.undo_kw, h.kh);
        }

        if (h.redo_visible) {
            repl_code_panel_draw_keycap(h.redo_kx, h.ky, h.redo_kw, h.kh);
            ui_clr(snap->can_redo ? UI_TOK_TEXT_PRIMARY : UI_TOK_TEXT_MUTED);
            repl_code_panel_draw_redo_icon(h.redo_kx, h.ky,
                                           h.redo_kw, h.kh);
        }

        if (h.copy_visible) {
            repl_code_panel_draw_keycap(h.copy_kx, h.ky, h.copy_kw, h.kh);
            ui_clr(UI_TOK_TEXT_PRIMARY);
            repl_code_panel_draw_copy_icon(h.copy_kx, h.ky,
                                           h.copy_kw, h.kh);
        }

        if (h.cut_visible) {
            repl_code_panel_draw_keycap(h.cut_kx, h.ky, h.cut_kw, h.kh);
            ui_clr(UI_TOK_TEXT_PRIMARY);
            repl_code_panel_draw_cut_icon(h.cut_kx, h.ky,
                                          h.cut_kw, h.kh);
        }

        if (h.trash_visible) {
            repl_code_panel_draw_keycap(h.trash_kx, h.ky,
                                        h.trash_kw, h.kh);
            ui_clr(UI_TOK_STATUS_ERR);
            repl_code_panel_draw_trash_icon(h.trash_kx, h.ky,
                                            h.trash_kw, h.kh);
        }

        if (h.focus_visible) {
            /* Keycap glyphs use the "F1"-keycap color (TEXT_PRIMARY)
             * so the two chips read identically; ON/OFF state is
             * carried by the "focus" label (accent on / muted off).
             * The keycap box is the UI_HIT_CODE_FOCUS_TOGGLE target,
             * hit-tested from this same hints geometry. */
            UiThemeToken focus_tok = snap->code_panel.code_focus
                ? UI_TOK_ACCENT : UI_TOK_TEXT_MUTED;
            repl_code_panel_draw_keycap(h.focus_kx, h.ky, h.focus_kw, h.kh);
            ui_clr(UI_TOK_TEXT_PRIMARY);
            repl_code_panel_draw_focus_kbd(h.focus_kx + 5, h.ky + 2);
            ui_clr(focus_tok);
            gl2d_draw_string((float)h.focus_lbl_x, (float)text_y,
                             k_statusbar_focus_lbl, FONT_SMALL);
        }

        if (h.help_visible) {
            /* Same scheme as the focus chip: keycap glyphs stay
             * TEXT_PRIMARY; the "help" label carries the active state
             * (accent while the overlay is open, muted otherwise). */
            UiThemeToken help_tok = snap->help.visible
                ? UI_TOK_ACCENT : UI_TOK_TEXT_MUTED;
            repl_code_panel_draw_keycap(h.help_kx, h.ky, h.help_kw, h.kh);
            ui_clr(UI_TOK_TEXT_PRIMARY);
            gl2d_draw_string((float)(h.help_kx + 5), (float)(h.ky + 2),
                             k_statusbar_help_kbd, FONT_SMALL);
            ui_clr(help_tok);
            gl2d_draw_string((float)h.help_lbl_x, (float)text_y,
                             k_statusbar_help_lbl, FONT_SMALL);
        }
    }

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glPopAttrib();
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
     * overlay pass (glr_ctrl.c) and still overpaints both (implemented
     * per plan §4). */
    ui_scene_tabs_render(snap);
    ui_menu_bar_render(snap);
    ui_menu_bar_render_search_overlay(snap);
    repl_code_panel_draw_statusbar(snap, &text_out.statusbar_slot);
    ui_color_picker_render(&snap->color_picker,
                           snap->viewport.window_w,
                           snap->viewport.window_h);
    ui_numeric_swatch_render(snap);
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
    } else if (row->kind == UI_TEXT_PANEL_ROW_TEXT &&
               hit.kind == UI_HIT_CODE_TEXT &&
               hit.char_idx >= 0) {
        hit.char_idx -= repl_code_panel_leading_ws_chars(row->text);
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

/* Keep statusbar hit geometry derived from the same hints struct the
 * renderer uses so hidden chips stay unclickable and visible chips
 * match their drawn keycap boxes exactly. */
static int repl_code_panel_statusbar_hit_kind(
    const UiRenderSnapshot *snap,
    const UiTextPanelSnapshot *text_snap,
    int mx,
    int gl_y) {
    ReplStatusbarHints hints = repl_code_panel_statusbar_hints(
        snap, text_snap->cp_x, text_snap->cp_y, text_snap->cp_w, STATUSBAR_H);

    if (hints.trash_visible &&
        repl_code_panel_point_in_rect(mx, gl_y,
                                      hints.trash_kx, hints.ky,
                                      hints.trash_kw, hints.kh))
        return UI_HIT_CODE_CLEAR_ALL;

    if (hints.cut_visible &&
        repl_code_panel_point_in_rect(mx, gl_y,
                                      hints.cut_kx, hints.ky,
                                      hints.cut_kw, hints.kh))
        return UI_HIT_CODE_CUT;

    if (hints.copy_visible &&
        repl_code_panel_point_in_rect(mx, gl_y,
                                      hints.copy_kx, hints.ky,
                                      hints.copy_kw, hints.kh))
        return UI_HIT_CODE_COPY;

    if (hints.redo_visible &&
        repl_code_panel_point_in_rect(mx, gl_y,
                                      hints.redo_kx, hints.ky,
                                      hints.redo_kw, hints.kh))
        return UI_HIT_CODE_REDO;

    if (hints.undo_visible &&
        repl_code_panel_point_in_rect(mx, gl_y,
                                      hints.undo_kx, hints.ky,
                                      hints.undo_kw, hints.kh))
        return UI_HIT_CODE_UNDO;

    if (hints.focus_visible &&
        repl_code_panel_point_in_rect(mx, gl_y,
                                      hints.focus_kx, hints.ky,
                                      hints.focus_kw, hints.kh))
        return UI_HIT_CODE_FOCUS_TOGGLE;

    if (hints.help_visible &&
        repl_code_panel_point_in_rect(mx, gl_y,
                                      hints.help_kx, hints.ky,
                                      hints.help_kw, hints.kh))
        return UI_HIT_HELP_TOGGLE;

    return UI_HIT_CODE_PANEL_CHROME;
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
    hit = ui_text_panel_hit_test(&builder.text_snap, mx, my);
    if (hit.kind != UI_HIT_NONE)
        return repl_code_panel_rewrite_hit(&builder, mx, hit);

    gl_y = builder.text_snap.vp_h - my;
    if (repl_code_panel_point_in_rect(mx, gl_y,
                                      builder.text_snap.cp_x,
                                      builder.text_snap.cp_y,
                                      builder.text_snap.cp_w,
                                      builder.text_snap.cp_h)) {
        if (gl_y < builder.text_snap.cp_y + STATUSBAR_H) {
            return repl_code_panel_make_local_hit(
                &builder.text_snap, mx, gl_y,
                repl_code_panel_statusbar_hit_kind(snap, &builder.text_snap,
                                                   mx, gl_y));
        }
        return repl_code_panel_make_local_hit(
            &builder.text_snap, mx, gl_y,
            mx < builder.text_snap.cp_x + builder.text_snap.text_x
                ? UI_HIT_CODE_GUTTER
                : UI_HIT_CODE_TEXT);
    }
    return hit;
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

/* Test-only: clear the render→hit-test row builder cache. Lets a test
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
