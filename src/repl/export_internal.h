/*
 * src/repl/export_internal.h - Private substrate for the export writer.
 *
 * Public save/load types stay in export.h. This header is only for the
 * export writer TUs split by output region.
 */
#ifndef REPL_EXPORT_INTERNAL_H
#define REPL_EXPORT_INTERNAL_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "repl/export.h"
#include "repl/export_format_shared.h"
#include "c_compat.h"
#include "config.h"
#include "source_document.h"
#include "repl/command_store.h"
#include "repl/host_effects.h"
#include "repl/program_query.h"
#include "repl/scenes.h"
#include "repl/core_internal.h"
#include "repl/util.h"
#include "repl/executor.h"
#include "repl/parser.h"
#include "repl/pipeline.h"
#include "repl/source_scope.h"
#include "repl/cfg_baseline.h"

#define g_export_cfg_bridge (repl_config_bridge())

#define REPL_EXPORT_CFG_SLUG_POINT_ATTENUATION "point_attenuation"
#define REPL_EXPORT_CFG_SLUG_MSAA              "msaa"
#define REPL_EXPORT_CFG_SLUG_LINE_SMOOTH       "line_smooth"

/* One formatted float uses REPL_SOURCE_FLOAT_TEXT_MAX (text_helpers.h);
 * exported GLfloat helper calls join up to 4 of them with ", " separators. */
#define EXPORT_FLOAT_TEXT_MAX REPL_SOURCE_FLOAT_TEXT_MAX
#define EXPORT_FLOAT_LIST_MAX (4 * EXPORT_FLOAT_TEXT_MAX)

typedef struct ExportNeeds {
    int needs_tess;
    int needs_rand;
    int needs_label;
    int needs_scratch_a;
    int needs_scratch_b;
    int needs_scratch_c;
    int tune_count;
    int tune_total;
    const char *tune_names[REPL_TUNE_MAX_KNOBS];
} ExportNeeds;

enum {
    EXPORT_NAME_MAX = 64,
    EXPORT_NAME_SET_MAX = 160
};

typedef struct ExportGeneratedNames {
    char draw_scene[EXPORT_NAME_MAX];
    char draw_tuning_overlay[EXPORT_NAME_MAX];
    char tuning_step[EXPORT_NAME_MAX];
    char hud_text[EXPORT_NAME_MAX];
    char tuning_window_width[EXPORT_NAME_MAX];
    char tuning_window_height[EXPORT_NAME_MAX];
    char keyboard_key[EXPORT_NAME_MAX];
    char keyboard_mouse_x[EXPORT_NAME_MAX];
    char keyboard_mouse_y[EXPORT_NAME_MAX];
    char tune_modifiers[EXPORT_NAME_MAX];
    char tune_normalized_key[EXPORT_NAME_MAX];
    char tune_step_scale[EXPORT_NAME_MAX];
    char overlay_width[EXPORT_NAME_MAX];
    char overlay_height[EXPORT_NAME_MAX];
    char overlay_text_y[EXPORT_NAME_MAX];
} ExportGeneratedNames;

typedef struct ExportScaffoldContext {
    ExportNeeds              needs;
    const ReplExportLayout  *layout;
    ExportGeneratedNames     names;
} ExportScaffoldContext;

void export_set_source_text_view(SourceTextView text);
SourceTextView export_source_text_view(void);
const char *export_document_text(int cmd_idx);

void export_format_decl_float(char *buf, size_t n, float v);
void export_format_float_list(char *buf, size_t n, const float *v, int count);
void export_write_c89_line(FILE *f, const char *line);

void format_cmd_source_as_c(char *out, size_t out_sz,
                            const char *source_text, int translate_exprs);
int write_materialfv_as_c89(FILE *f, const char *source_text);
int write_point_parameterfv_as_c89(FILE *f, const char *source_text);
int find_export_block_end(int begin_idx);
int export_uses_tess_commands(void);
void write_render_body_range_as_c(FILE *f, int start, int end_idx,
                                  int skip_func_defs);

int export_has_persistent_predef_vars(void);
void write_predef_var_globals(FILE *f);
void write_save_restore_helpers(FILE *f);
void write_rand_helper(FILE *f);
void write_glfloat_vector_helpers(FILE *f);
void write_label_helper(FILE *f);
void write_render_helper_as_c(FILE *f, const char *name);
void write_func_defs_as_c(FILE *f);
void write_tess_preamble(FILE *f);

void emit_export_header_pre(FILE *f, const ExportNeeds *needs);
void emit_export_cam_lines(FILE *f);
void emit_export_init_section_to_file(FILE *f, int include_tess);
void emit_footer_post_init(FILE *f, int win_w, int win_h);

ExportNeeds export_collect_needs(void);
void export_generated_names_init(ExportGeneratedNames *names);
int export_display_has_multiple_enabled_passes(void);
void write_tune_helpers(FILE *f, const ExportNeeds *needs,
                        const ExportGeneratedNames *names);
void emit_export_display(FILE *f, const ExportNeeds *needs,
                         const ReplExportLayout *layout,
                         const ExportGeneratedNames *names);

#endif /* REPL_EXPORT_INTERNAL_H */
