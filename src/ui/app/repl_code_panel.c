#include "ui/app/repl_code_panel.h"

#include "c_compat.h"
#include "editor/state.h"
#include "repl/command_spec.h"
#include "repl/core.h"
#include "repl/eval.h"
#include "repl/export.h"
#include "repl/state_views.h"
#include "ui/app/color_picker.h"
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
#include "subsystems/tutorial/tutorial.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define UI_REPL_CODE_PANEL_MAX_STATIC_ROWS 256
#define UI_REPL_CODE_PANEL_MAX_ROWS \
    (MAX_COMMANDS + MAX_VIRTUAL_LINES + UI_REPL_CODE_PANEL_MAX_STATIC_ROWS + 2)
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
static char g_repl_code_panel_generated_text[UI_REPL_CODE_PANEL_MAX_GENERATED_TEXT_ROWS][MAX_LINE_LEN];

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

/* Derived-C boilerplate sub-palette. Deliberately outside the theme
 * tokens (this is the syntax / generated-code color domain, like
 * k_category_colors / k_syntax_shade above): CHROME is the dim
 * gray-blue of the static header/footer chrome lines; STATE is the
 * slightly brighter tint of the synthesized render-state / camera /
 * lights lines. Each expands to an r,g,b argument triple. */
#define REPL_CODE_PANEL_CHROME_RGB 0.38f, 0.38f, 0.42f
#define REPL_CODE_PANEL_STATE_RGB  0.50f, 0.45f, 0.55f

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
    rows += repl_code_panel_row_count(snap, REPL_CODE_PANEL_SCRATCH_DECL_LINE,
                                      text_x, panel_w);
    for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++)
        rows += repl_code_panel_row_count(snap,
                                          import_export.render_state_lines[i],
                                          text_x, panel_w);
    for (int i = 0; i < CAM_LINE_COUNT; i++)
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
    return rows;
}

static int repl_code_panel_footer_row_count(const UiRenderSnapshot *snap,
                                            int panel_w, int text_x) {
    int rows = 0;
    char line[MAX_LINE_LEN];

    if (!repl_code_panel_chrome_visible(snap))
        return 0;

    for (int i = 0; g_footer_pre_init[i]; i++) {
        if (strcmp(g_footer_pre_init[i],
                   REPL_EXPORT_RESHAPE_PROJ_SENTINEL) == 0) {
            /* Read the frame-frozen block from the snapshot — never
             * re-resolve live here, or this pass and the render pass
             * (opposite sides of scene_render_3d_scene) could disagree
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

static int repl_code_panel_command_main_rows(const UiRenderSnapshot *snap,
                                             int cmd_idx, int panel_w,
                                             int text_x) {
    if (!snap->editor_input.insert_mode && cmd_idx == snap->edit_line) {
        int indent_chars = snap->active_indent_chars;
        return repl_code_panel_row_count(snap,
                                         snap->editor_input.input,
                                         text_x + indent_chars * FONT_W,
                                         panel_w);
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
        if (replay_extra_rows) {
            int v_count = 0;
            if (snap->editor_virtual_lines) {
                for (int v = 0; v < snap->editor_virtual_lines->count; v++) {
                    if (snap->editor_virtual_lines->items[v].after_line_idx == i) {
                        v_count++;
                    }
                }
            }
            replay_extra_rows[i] = v_count;
        }
    }
}

static int repl_code_panel_insert_rows(const UiRenderSnapshot *snap,
                                       int panel_w, int text_x) {
    int indent_chars = snap->active_indent_chars;
    return repl_code_panel_row_count(snap,
                                     snap->editor_input.input,
                                     text_x + indent_chars * FONT_W,
                                     panel_w);
}

static int repl_code_panel_trailing_row_count(const UiRenderSnapshot *snap,
                                        int panel_w, int text_x) {
    if (snap->edit_line == snap->document_count) {
        int indent_chars = snap->active_indent_chars;
        return repl_code_panel_row_count(snap,
                                         snap->editor_input.input,
                                         text_x + indent_chars * FONT_W,
                                         panel_w);
    }
    return 1;
}

static int repl_code_panel_cursor_doc_line_from_layout(
    const UiRenderSnapshot *snap,
    int header_rows, const int *cmd_main_rows, const int *replay_extra_rows,
    int panel_w, int text_x) {
    int cursor_doc_line = header_rows;

    /* The three former branches (insert mode / in-range edit line /
     * out-of-range fallback) all summed the same prefix and then added
     * the identical cursor-row term. The prefix length is the same in
     * every case: min(edit_line, document_count) — insert mode clamps
     * to both bounds; non-insert uses edit_line when it is
     * < document_count, otherwise document_count, which is that same
     * min. So the dispatch was redundant. */
    int prefix = (snap->edit_line < snap->document_count)
                     ? snap->edit_line : snap->document_count;
    for (int i = 0; i < prefix; i++) {
        cursor_doc_line += cmd_main_rows[i];
        cursor_doc_line += replay_extra_rows[i];
    }
    cursor_doc_line += repl_code_panel_cursor_row(
        snap,
        snap->editor_input.input,
        text_x + snap->active_indent_chars * FONT_W,
        panel_w, snap->editor_input.cursor_pos, NULL, NULL, NULL);

    return cursor_doc_line;
}

static int repl_code_panel_follow_doc_line_from_layout(
    const UiRenderSnapshot *snap,
    int cursor_doc_line, int header_rows, const int *cmd_main_rows,
    const int *replay_extra_rows) {
    int follow_doc_line = cursor_doc_line;
    int src_line = snap->replay.src_line_idx;

    if (snap->replay.active && src_line >= 0 && src_line < snap->document_count) {
        follow_doc_line = header_rows;
        for (int i = 0; i < src_line; i++) {
            follow_doc_line += cmd_main_rows[i];
            follow_doc_line += replay_extra_rows[i];
        }
        if (replay_extra_rows[src_line] > 0) {
            follow_doc_line += cmd_main_rows[src_line];
            follow_doc_line += replay_extra_rows[src_line] - 1;
        } else if (cmd_main_rows[src_line] > 0) {
            follow_doc_line += cmd_main_rows[src_line] - 1;
        }
    }

    return follow_doc_line;
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

    for (int cmd_idx = 0; cmd_idx <= snap->document_count; cmd_idx++) {
        if (snap->editor_input.insert_mode && cmd_idx == snap->edit_line) {
            int insert_rows = repl_code_panel_insert_rows(snap, layout->panel_w,
                                                          layout->text_x);
            if (row < insert_rows) {
                if (out_target) *out_target = -1;
                if (out_on_insert_line) *out_on_insert_line = 1;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            row -= insert_rows;
        }

        if (cmd_idx < snap->document_count) {
            int main_rows = layout->cmd_main_rows[cmd_idx];
            if (row < main_rows) {
                if (out_target) *out_target = cmd_idx;
                if (out_on_insert_line) *out_on_insert_line = 0;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            row -= main_rows;

            if (row < layout->replay_extra_rows[cmd_idx]) {
                if (out_target) *out_target = cmd_idx;
                if (out_on_insert_line) *out_on_insert_line = 0;
                if (out_row_offset) *out_row_offset = 0;
                return 1;
            }
            row -= layout->replay_extra_rows[cmd_idx];
        } else {
            int newline_rows = repl_code_panel_trailing_row_count(snap, layout->panel_w,
                                                            layout->text_x);
            if (row < newline_rows) {
                if (out_target) *out_target = snap->document_count;
                if (out_on_insert_line) *out_on_insert_line = 0;
                if (out_row_offset) *out_row_offset = row;
                return 1;
            }
            return 0;
        }
    }

    return 0;
}

static void repl_code_panel_find_highlight_rows(const UiRenderSnapshot *snap,
                                                int *out_normal_idx,
                                                int *out_color_idx,
                                                int *out_tutorial_insertion_idx) {
    int normal_idx = -1;
    int color_idx = -1;
    int tutorial_insertion_idx = -1;

    if (snap && snap->editor_highlights) {
        for (int i = 0; i < snap->editor_highlights->count; i++) {
            const UiHighlight *highlight = &snap->editor_highlights->items[i];
            if (highlight->kind == HIGHLIGHT_FEEDING_NORMAL)
                normal_idx = highlight->line_idx;
            else if (highlight->kind == HIGHLIGHT_FEEDING_COLOR)
                color_idx = highlight->line_idx;
            else if (highlight->kind == HIGHLIGHT_TUTORIAL_INSERTION)
                tutorial_insertion_idx = highlight->line_idx;
        }
    }

    if (out_normal_idx) *out_normal_idx = normal_idx;
    if (out_color_idx) *out_color_idx = color_idx;
    if (out_tutorial_insertion_idx) *out_tutorial_insertion_idx = tutorial_insertion_idx;
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
                                        &builder->highlight_tutorial_insertion_idx);

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

static void repl_code_panel_set_vertex_label(UiTextPanelRow *row,
                                             const UiRenderSnapshot *snap,
                                             int is_vertex, int vnum,
                                             int primitive_vnums_exact) {
    if (!row || !snap || !snap->code_panel.show_vertex_indices || !is_vertex)
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
    MARKER_PRIORITY_FEEDING_NORMAL,
    MARKER_PRIORITY_FEEDING_COLOR,
    MARKER_PRIORITY_TUTORIAL_INSERTION
} MarkerPriority;

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

static float repl_code_panel_fade_slot_step(float fade_duration, int line_len) {
    int safe_len = line_len > 0 ? line_len : 1;
    int total_slots = safe_len + TUTORIAL_FADE_SETTLE_CHARS;
    return fade_duration / (float)total_slots;
}

static int repl_code_panel_fade_front(const UiRenderSnapshot *snap, int line_idx, int line_len) {
    if (!snap->tutorial_fade.active || line_idx != snap->tutorial_fade.fade_line_idx)
        return -1;
    float now = snap->anim_time;
    if (now >= snap->tutorial_fade.fade_start_t + snap->tutorial_fade.fade_duration)
        return -1;
    int safe_len = line_len > 0 ? line_len : 1;
    float step = repl_code_panel_fade_slot_step(snap->tutorial_fade.fade_duration, safe_len);
    if (step <= 0.0f)
        return -1;
    float elapsed = now - snap->tutorial_fade.fade_start_t;
    if (elapsed <= 0.0f)
        return 0;
    int front = (int)(elapsed / step);
    if (front < 0)
        front = 0;
    if (front >= safe_len)
        return -1;
    return front;
}

static float repl_code_panel_fade_alpha(const UiRenderSnapshot *snap, int line_idx, int char_idx, int line_len) {
    if (!snap->tutorial_fade.active || line_idx != snap->tutorial_fade.fade_line_idx)
        return 1.0f;
    float now = snap->anim_time;
    if (now >= snap->tutorial_fade.fade_start_t + snap->tutorial_fade.fade_duration)
        return 1.0f;
    int safe_len = line_len > 0 ? line_len : 1;
    if (char_idx < 0) char_idx = 0;
    if (char_idx >= safe_len) char_idx = safe_len - 1;
    float step = repl_code_panel_fade_slot_step(snap->tutorial_fade.fade_duration, safe_len);
    if (step <= 0.0f)
        return 1.0f;
    float elapsed = (now - snap->tutorial_fade.fade_start_t) - (float)char_idx * step;
    float alpha = elapsed / step;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return alpha;
}

static float repl_code_panel_fade_settle(const UiRenderSnapshot *snap, int line_idx, int char_idx, int line_len) {
    if (!snap->tutorial_fade.active || line_idx != snap->tutorial_fade.fade_line_idx)
        return 1.0f;
    float now = snap->anim_time;
    if (now >= snap->tutorial_fade.fade_start_t + snap->tutorial_fade.fade_duration)
        return 1.0f;
    int safe_len = line_len > 0 ? line_len : 1;
    if (char_idx < 0) char_idx = 0;
    if (char_idx >= safe_len) char_idx = safe_len - 1;
    float step = repl_code_panel_fade_slot_step(snap->tutorial_fade.fade_duration, safe_len);
    float settle_duration = step * (float)TUTORIAL_FADE_SETTLE_CHARS;
    if (settle_duration <= 0.0f)
        return 1.0f;
    float elapsed = (now - snap->tutorial_fade.fade_start_t) - (float)(char_idx + 1) * step;
    float settle = elapsed / settle_duration;
    if (settle < 0.0f) settle = 0.0f;
    if (settle > 1.0f) settle = 1.0f;
    return settle;
}

static int repl_code_panel_line_is_fading(const UiRenderSnapshot *snap, int line_idx) {
    return snap->tutorial_fade.active && line_idx == snap->tutorial_fade.fade_line_idx &&
           snap->anim_time < snap->tutorial_fade.fade_start_t + snap->tutorial_fade.fade_duration;
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

    front = repl_code_panel_fade_front(snap, line_idx, line_len);
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
        s = repl_code_panel_fade_settle(snap, line_idx, i, line_len);
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
                    repl_code_panel_fade_alpha(snap, line_idx, front, line_len)),
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
 * The keyword keeps the full class color; literals/constants/variables are
 * dimmer/desaturated tiers of that same hue, so the class stays recognizable
 * and a line never clashes with itself. Kinds differ only in brightness and
 * saturation, not hue. {brightness, saturation} multipliers, all <= the
 * keyword so it stays dominant; tuned against the (0.06,0.06,0.10) panel
 * background and guarded by test_repl_code_panel_syntax's contrast check. */
static const float k_syntax_shade[REPL_SYNTAX_KIND_COUNT][2] = {
    [REPL_SYNTAX_CONSTANT] = { 0.90f, 0.72f },  /* vivid-ish, slightly dim */
    [REPL_SYNTAX_VARIABLE] = { 0.78f, 0.52f },  /* muted, mid brightness */
    [REPL_SYNTAX_LITERAL]  = { 0.66f, 0.36f },  /* most muted + dimmest */
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

        /* Quoted string: the whole run (incl. quotes) is one literal. Do
         * not descend into a label("...") format string. */
        if (c == '"') {
            int start = i;

            i++;
            while (text[i] && text[i] != '"')
                i++;
            if (text[i] == '"')
                i++;
            out[n++] = (UiSyntaxSpan){ start, i - start, REPL_SYNTAX_LITERAL };
            continue;
        }

        /* Numeric literal: digits / '.' / exponent. A leading '.' counts
         * only when a digit follows (otherwise it is structural). */
        if (isdigit(c) ||
            (c == '.' && isdigit((unsigned char)text[i + 1]))) {
            int start = i;

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
            } else if (repl_eval_is_reserved_ident(name)) {
                /* reserved keyword / math fn used bare — structural */
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

/* mode: 0 = off (no spans), 1 = on, 2 = on + fake-bold constants. */
static void repl_code_panel_apply_syntax_segments(const UiRenderSnapshot *snap,
                                                  const char *text,
                                                  CmdType type,
                                                  int mode,
                                                  UiTextPanelRow *row) {
    UiSyntaxSpan spans[UI_TEXT_PANEL_MAX_COLOR_SEGMENTS];
    CmdSyntaxCategory cat;
    int count;

    if (!row || !text || !text[0] || mode <= 0)
        return;

    cat = repl_cmd_type_category(type);
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
                /* On+Bold mode fake-bolds constants only. */
                .bold = (mode == 2 &&
                         spans[i].kind == REPL_SYNTAX_CONSTANT),
            };
    }
}

static void repl_code_panel_add_static_row(ReplCodePanelBuilder *builder,
                                           const char *text,
                                           UiTextPanelColor color) {
    UiTextPanelRow *row = repl_code_panel_push_row(builder);

    if (!row)
        return;

    row->text = text ? text : "";
    row->kind = UI_TEXT_PANEL_ROW_STATIC;
    row->left_gutter_label = builder->file_line++;
    row->color = color;
    row->hit_eligible = 0;
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
    repl_code_panel_set_vertex_label(row, builder->snap, is_vertex, vnum,
                                     primitive_vnums_exact);
    repl_code_panel_set_right_action(row, builder->snap, line_idx);
    repl_code_panel_apply_command_overlays(builder, line_idx, row);
    if (repl_code_panel_line_is_fading(builder->snap, line_idx))
        repl_code_panel_apply_fade_segments(builder->snap, line_idx, display_text, row);
    else
        repl_code_panel_apply_syntax_segments(
            builder->snap,
            display_text,
            builder->snap->document_cmds[line_idx].type,
            builder->snap->code_panel.syntax_highlight, row);
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

        snprintf(text, MAX_LINE_LEN, "%s%s", virtual_line->text, virtual_line->aux);
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

static void repl_code_panel_build_rows(ReplCodePanelBuilder *builder) {
    const UiRenderSnapshot *snap;
    int vnum = 0;
    int loop_depth = 0;
    int tess_depth = 0;
    int in_tess_poly = 0;
    int primitive_vnums_exact = 1;

    if (!builder || !builder->snap)
        return;

    snap = builder->snap;

    if (repl_code_panel_chrome_visible(snap)) {
        for (int i = 0; i < snap->import_export.workspace_header_line_count; i++) {
            repl_code_panel_add_static_row(
                builder,
                snap->import_export.workspace_header_lines[i],
                repl_code_panel_rgb(0.45f, 0.55f, 0.42f));
        }
        for (int i = 0; g_header_pre[i]; i++) {
            repl_code_panel_add_static_row(
                builder, g_header_pre[i],
                repl_code_panel_rgb(REPL_CODE_PANEL_CHROME_RGB));
        }
        for (int i = 0; g_display_header[i]; i++) {
            repl_code_panel_add_static_row(
                builder, g_display_header[i],
                repl_code_panel_rgb(REPL_CODE_PANEL_CHROME_RGB));
        }
        /* Scratch decoration row: panel-only (the exporter emits the
         * arrays as file-scope statics on demand instead). */
        repl_code_panel_add_static_row(
            builder, REPL_CODE_PANEL_SCRATCH_DECL_LINE,
            repl_code_panel_category_color(CMD_VAR_DECLARE));
        for (int i = 0; i < RENDER_STATE_LINE_COUNT; i++) {
            repl_code_panel_add_static_row(
                builder,
                snap->import_export.render_state_lines[i],
                repl_code_panel_rgb(REPL_CODE_PANEL_STATE_RGB));
        }
        for (int i = 0; i < CAM_LINE_COUNT; i++) {
            repl_code_panel_add_static_row(
                builder,
                snap->import_export.cam_lines[i],
                repl_code_panel_rgb(REPL_CODE_PANEL_STATE_RGB));
        }
        for (int i = 0; i < snap->lights_display_count; i++) {
            repl_code_panel_add_static_row(builder, snap->lights_display_lines[i],
                                           repl_code_panel_rgb(REPL_CODE_PANEL_STATE_RGB));
        }
        for (int i = 0; g_header_post[i]; i++) {
            repl_code_panel_add_static_row(
                builder, g_header_post[i],
                repl_code_panel_rgb(REPL_CODE_PANEL_CHROME_RGB));
        }
    }

    for (int i = 0; i < snap->document_count; i++) {
        int is_edit;
        int is_vertex;
        char aux_label[8] = "";

        if (snap->editor_input.insert_mode && i == snap->edit_line) {
            repl_code_panel_add_input_row(builder, -1, snap->edit_line,
                                          snap->active_indent_chars, i, NULL);
        }

        if (snap->document_cmds[i].valid) {
            if (snap->document_cmds[i].type == CMD_BEGIN) {
                vnum = 0;
                primitive_vnums_exact = (loop_depth == 0);
            } else if (snap->document_cmds[i].type == CMD_TESS_BEGIN_POLYGON) {
                vnum = 0;
                in_tess_poly = 1;
                tess_depth = 1;
                primitive_vnums_exact = (loop_depth == 0);
            }
        }

        is_edit = (!snap->editor_input.insert_mode && i == snap->edit_line);
        is_vertex = snap->document_cmds[i].valid &&
                    repl_cmd_emits_vertex(snap->document_cmds[i].type);

        if (snap->code_panel.show_vertex_indices && is_vertex) {
            snprintf(aux_label, sizeof(aux_label),
                     primitive_vnums_exact ? "v%d" : "vn", vnum);
        }

        if (is_edit) {
            repl_code_panel_add_input_row(builder, i, -1,
                                          snap->active_indent_chars, i,
                                          aux_label[0] ? aux_label : NULL);
        } else {
            repl_code_panel_add_command_row(builder, i, is_vertex, vnum,
                                            primitive_vnums_exact);
        }

        repl_code_panel_add_virtual_rows(builder, i);

        if (is_vertex)
            vnum++;
        if (snap->document_cmds[i].valid) {
            switch (snap->document_cmds[i].type) {
            case CMD_FOR_BEGIN:
                loop_depth++;
                primitive_vnums_exact = 0;
                break;
            case CMD_FOR_END:
                if (loop_depth > 0)
                    loop_depth--;
                break;
            case CMD_END:
                primitive_vnums_exact = 1;
                break;
            case CMD_TESS_BEGIN_CONTOUR:
                if (in_tess_poly)
                    tess_depth++;
                break;
            case CMD_TESS_END:
                if (in_tess_poly) {
                    if (tess_depth > 0)
                        tess_depth--;
                    if (tess_depth == 0) {
                        in_tess_poly = 0;
                        primitive_vnums_exact = 1;
                    }
                }
                break;
            default:
                break;
            }
        }
    }

    if (snap->edit_line == snap->document_count) {
        repl_code_panel_add_input_row(builder, -1, snap->document_count,
                                      snap->active_indent_chars,
                                      snap->document_count, NULL);
    } else {
        repl_code_panel_add_placeholder_row(builder, snap->document_count,
                                            snap->trailing_indent_chars);
    }

    if (repl_code_panel_chrome_visible(snap)) {
        for (int i = 0; g_footer_pre_init[i]; i++) {
            if (strcmp(g_footer_pre_init[i],
                       REPL_EXPORT_RESHAPE_PROJ_SENTINEL) == 0) {
                /* Frame-frozen in the snapshot by the controller; its
                 * storage outlives this render, so add_static_row may hold
                 * the pointer directly (like the literal g_footer lines).
                 * Same block the row-count pass read, so they agree. */
                for (int j = 0; j < snap->reshape_proj_count; j++)
                    repl_code_panel_add_static_row(
                        builder, snap->reshape_proj_lines[j],
                        repl_code_panel_rgb(REPL_CODE_PANEL_CHROME_RGB));
                continue;
            }
            repl_code_panel_add_static_row(
                builder, g_footer_pre_init[i],
                repl_code_panel_rgb(REPL_CODE_PANEL_CHROME_RGB));
        }
        for (int i = 0; i < snap->init_section_count; i++) {
            repl_code_panel_add_static_row(builder, snap->init_section_lines[i],
                                           repl_code_panel_rgb(REPL_CODE_PANEL_CHROME_RGB));
        }
        for (int i = 0; g_footer_post_init[i]; i++) {
            repl_code_panel_add_static_row(
                builder, g_footer_post_init[i],
                repl_code_panel_rgb(REPL_CODE_PANEL_CHROME_RGB));
        }
    }

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

/* Right-aligned statusbar cluster: a "[focus] focus" keycap+label and
 * the existing "[F1] help" keycap+label. The geometry is derived once
 * here so the renderer and the hit-test agree on the clickable focus
 * keycap box (window / GL coords, bottom-left origin), with no
 * arithmetic duplicated across the two passes. */
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
    char line[64];
    char aa[32];
    int  cmds_w;
    int  line_w;
    int  aa_w;
    int  has_aa;
    int  right_edge;
} ReplStatusbarLeft;

static ReplStatusbarLeft repl_code_panel_statusbar_left(
        const UiRenderSnapshot *snap, int sx) {
    ReplStatusbarLeft L;
    int tx = sx + CODE_MARGIN_X;
    int edit_line = snap->edit_line;

    snprintf(L.cmds, sizeof L.cmds, "%d/%d cmds",
             snap->flat_program_count, MAX_COMMANDS);
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
        if (snap->render.accum_aa_enabled && snap->render.accum_samples > 1)
            snprintf(L.aa, sizeof L.aa, "AA %dx", snap->render.accum_samples);
        else
            snprintf(L.aa, sizeof L.aa, "AA off");
        L.aa_w = (int)strlen(L.aa) * FONT_SMALL_W;
        tx += L.aa_w;
    } else {
        L.aa[0] = '\0';
        L.aa_w = 0;
    }
    L.right_edge = tx;
    return L;
}

typedef struct {
    int text_y;
    int help_kx, help_lbl_x, help_kw;
    int focus_kx, focus_lbl_x, focus_kw;
    int ky, kh;                 /* keycap box y / h (shared) */
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

    /* The right cluster is always drawn from the right edge; the left
     * text from the left. On narrow/default panels they collide, so
     * suppress whichever right chip the left text would reach (focus
     * first — it sits inboard of help). Keyboard Ctrl+Shift+F / F1
     * still work when a chip is hidden. */
    h.help_visible  = (h.help_kx  >= left_end + gap);
    h.focus_visible = (h.focus_kx >= left_end + gap);
    return h;
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

        /* Right cluster, drawn from the right edge. Each chip is
         * suppressed when the left status text would collide with it
         * (focus first — it sits inboard of help); the keyboard
         * Ctrl+Shift+F / F1 paths still work when a chip is hidden. */
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

    repl_code_panel_build_rows(&builder);

    g_builder_cache.snap    = snap;
    g_builder_cache.builder = builder;
    g_builder_cache.builder.text_snap.rows = g_repl_code_panel_rows;
    g_builder_cache.valid   = 1;

    memset(&text_out, 0, sizeof(text_out));
    ui_text_panel_render(&builder.text_snap, &text_out);

    if (out) {
        out->cursor_px = text_out.cursor_px;
        out->cursor_py = text_out.cursor_py;
        out->cursor_valid = text_out.cursor_valid;
    }

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
    if (mx >= builder.text_snap.cp_x &&
        mx < builder.text_snap.cp_x + builder.text_snap.cp_w &&
        gl_y >= builder.text_snap.cp_y &&
        gl_y < builder.text_snap.cp_y + builder.text_snap.cp_h) {
        if (gl_y < builder.text_snap.cp_y + STATUSBAR_H) {
            /* Same hints geometry the renderer uses, so the click
             * targets line up exactly with the drawn keycaps. */
            ReplStatusbarHints h = repl_code_panel_statusbar_hints(
                snap, builder.text_snap.cp_x, builder.text_snap.cp_y,
                builder.text_snap.cp_w, STATUSBAR_H);
            if (h.focus_visible &&
                gl_y >= h.ky && gl_y < h.ky + h.kh &&
                mx >= h.focus_kx && mx < h.focus_kx + h.focus_kw) {
                hit.kind = UI_HIT_CODE_FOCUS_TOGGLE;
                hit.local_x = (float)(mx - builder.text_snap.cp_x);
                hit.local_y = (float)(gl_y - builder.text_snap.cp_y);
                return hit;
            }
            if (h.help_visible &&
                gl_y >= h.ky && gl_y < h.ky + h.kh &&
                mx >= h.help_kx && mx < h.help_kx + h.help_kw) {
                hit.kind = UI_HIT_HELP_TOGGLE;
                hit.local_x = (float)(mx - builder.text_snap.cp_x);
                hit.local_y = (float)(gl_y - builder.text_snap.cp_y);
                return hit;
            }
            hit.kind = UI_HIT_CODE_PANEL_CHROME;
            hit.local_x = (float)(mx - builder.text_snap.cp_x);
            hit.local_y = (float)(gl_y - builder.text_snap.cp_y);
            return hit;
        }
        hit.kind = mx < builder.text_snap.cp_x + builder.text_snap.text_x
            ? UI_HIT_CODE_GUTTER
            : UI_HIT_CODE_TEXT;
        hit.local_x = (float)(mx - builder.text_snap.cp_x);
        hit.local_y = (float)(gl_y - builder.text_snap.cp_y);
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
