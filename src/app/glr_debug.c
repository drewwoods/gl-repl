/*
 * glr_debug.c - Diagnostic dumps for CLI flags and tests.
 */
#include <stdio.h>
#include "repl/flatten.h"
#include "source_document.h"
#include "app/glr_debug.h"
#include "app/glr_ctrl.h"      /* glr_ctrl_bootstrap_repl */
#include "app/glr_actions.h"   /* glr_scene_load_example */

#include "editor/state.h"
#include "repl/export.h"
#include "repl/command_spec.h"
#include "repl/eval.h"       /* repl_func_alias_get, REPL_FUNC_SLOT_COUNT */
#include "repl/pipeline.h"
#include "repl/state.h"

#include <string.h>          /* memset (flat histogram) */

#include <stddef.h>

#include "c_compat.h"  /* STATIC_ASSERT (C99/C11 portable) */

void glr_debug_dump_editor(FILE *out, SourceTextView text) {
    FILE *dst = out ? out : stdout;

    fprintf(dst, "=== REPL Editor Dump ===\n");
    fprintf(dst,
            "num_cmds=%d edit_line=%d inserting=%d flat_dirty=%d normals_dirty=%d\n",
            repl_state_document_count(), editor_state_edit_line(),
            editor_insert_mode(), repl_state_flat_program_dirty(),
            repl_state_normals_dirty());

    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        const GLCmd *cmd = &repl_state_document_cmds()[cmd_idx];
        const char *line_text = source_text_line(text, cmd_idx);
        fprintf(dst,
                "%4d | %-22s | valid=%d has_vars=%d is_auto=%d src_idx=%d | %s\n",
                cmd_idx, repl_cmd_type_name(cmd->type), cmd->valid,
                cmd->has_vars, cmd->is_auto, cmd->src_cmd_idx,
                line_text ? line_text : "");
    }

    repl_dump_code_panel_text(dst, text);
    fprintf(dst, "=== End REPL Editor Dump ===\n");
    fflush(dst);
}

void glr_debug_dump_flat_commands_sync(FILE *out, SourceTextView text) {
    FILE *dst = out ? out : stdout;
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;
    int num_flat_cmds = flat_program.cmd_count;

    if (repl_refresh_flat_program(editor_state_edit_line()) !=
        REPL_FLAT_REFRESH_NONE) {
        flat_program = repl_state_flat_program_view();
        flat_cmds = flat_program.cmds;
        num_flat_cmds = flat_program.cmd_count;
    }

    fprintf(dst, "=== REPL Flattened Commands Dump ===\n");
    fprintf(dst, "num_flat_cmds=%d\n", num_flat_cmds);

    for (int flat_idx = 0; flat_idx < num_flat_cmds; flat_idx++) {
        const GLCmd *cmd = &flat_cmds[flat_idx];
        const char *line_text = source_text_line(text, cmd->src_cmd_idx);
        fprintf(dst,
                "%4d | %-22s | valid=%d has_vars=%d src_idx=%d call_src_idx=%d root_call_src_idx=%d func_scope=0x%08x | %s\n",
                flat_idx, repl_cmd_type_name(cmd->type), cmd->valid,
                cmd->has_vars, cmd->src_cmd_idx, cmd->call_src_cmd_idx,
                cmd->root_call_src_cmd_idx, cmd->func_scope_mask,
                line_text ? line_text : "");
    }
    fprintf(dst, "=== End REPL Flattened Commands Dump ===\n");
    fflush(dst);
}

/* --flat-histogram: where the MAX_FLAT_COMMANDS flatten budget is being
 * spent. Two sections: per-function inclusive costs (func_scope_mask —
 * exact across nesting/recursion; a nested call's commands count
 * toward every function on its chain, so the section can sum past the
 * flat total), then per-source-line direct emissions sorted
 * descending (each flat command counts toward exactly one line, so
 * this section sums to the flat total). */
void glr_debug_dump_flat_histogram(FILE *out, SourceTextView text) {
    FILE *dst = out ? out : stdout;
    static int line_counts[MAX_EDITOR_COMMANDS];
    static int order[MAX_EDITOR_COMMANDS];

    repl_refresh_flat_program(editor_state_edit_line());

    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;
    int num_flat_cmds = flat_program.cmd_count;
    const GLCmd *doc = repl_state_document_cmds();
    int doc_count = repl_state_document_count();
    if (doc_count > MAX_EDITOR_COMMANDS) doc_count = MAX_EDITOR_COMMANDS;

    fprintf(dst, "=== REPL Flat-Cost Histogram ===\n");
    fprintf(dst, "flat total: %d/%d\n", num_flat_cmds, MAX_FLAT_COMMANDS);

    fprintf(dst, "-- functions (inclusive, all call sites) --\n");
    for (int slot = 0; slot < REPL_FUNC_SLOT_COUNT; slot++) {
        int def_idx = -1;
        for (int i = 0; i < doc_count; i++) {
            if (doc[i].type == CMD_FUNC_DEF && (int)doc[i].args[0] == slot) {
                def_idx = i;
                break;
            }
        }
        if (def_idx < 0) continue;
        int count = 0;
        for (int i = 0; i < num_flat_cmds; i++)
            if (flat_cmds[i].func_scope_mask & (1u << slot)) count++;
        const char *alias = repl_func_alias_get(slot);
        fprintf(dst, "%5d  %5.1f%%  func%d%s%s (line %d)\n",
                count,
                num_flat_cmds > 0 ? 100.0 * count / num_flat_cmds : 0.0,
                slot,
                (alias && alias[0]) ? " " : "",
                (alias && alias[0]) ? alias : "",
                def_idx);
    }

    memset(line_counts, 0, sizeof(line_counts));
    for (int i = 0; i < num_flat_cmds; i++) {
        int src = flat_cmds[i].src_cmd_idx;
        if (src >= 0 && src < MAX_EDITOR_COMMANDS)
            line_counts[src]++;
    }
    int used = 0;
    for (int i = 0; i < doc_count; i++)
        if (line_counts[i] > 0)
            order[used++] = i;
    /* Selection sort, descending by count (ties: line order). Offline
     * dump over <= MAX_EDITOR_COMMANDS entries — simplicity over speed. */
    for (int a = 0; a < used - 1; a++) {
        int best = a;
        for (int b = a + 1; b < used; b++)
            if (line_counts[order[b]] > line_counts[order[best]])
                best = b;
        if (best != a) {
            int tmp = order[a];
            order[a] = order[best];
            order[best] = tmp;
        }
    }

    fprintf(dst, "-- source lines (direct emissions) --\n");
    for (int a = 0; a < used; a++) {
        int li = order[a];
        const char *line_text = source_text_line(text, li);
        fprintf(dst, "%5d  %5.1f%%  line %4d | %s\n",
                line_counts[li],
                num_flat_cmds > 0 ? 100.0 * line_counts[li] / num_flat_cmds
                                  : 0.0,
                li, line_text ? line_text : "");
    }
    fprintf(dst, "=== End REPL Flat-Cost Histogram ===\n");
    fflush(dst);
}

void glr_debug_dump_current_editor(FILE *out) {
    glr_debug_dump_editor(out, source_document_view());
}

void glr_debug_dump_current_flat_commands_sync(FILE *out) {
    glr_debug_dump_flat_commands_sync(out, source_document_view());
}

void glr_debug_dump_current_flat_histogram(FILE *out) {
    glr_debug_dump_flat_histogram(out, source_document_view());
}

void glr_debug_dump_runtime_state_layout(FILE *out) {
    FILE *dst = out ? out : stdout;

/* The runtime-state layout dump lists only REPL-owned runtime slices. Peer/app
 * state has its own owners and is outside ReplRuntimeState. */
#define REPL_RUNTIME_STATE_FIELDS(X)                                                               \
    X(ReplDocumentState, document)                                                                 \
    X(ReplFlatProgramState, flat_program)                                                          \
    X(ReplVariableState, variables)                                                                \
    X(ReplRenderState, render)                                                                     \
    X(ReplSceneRuntimeState, scene_runtime)                                                        \
    X(ReplImportExportState, import_export)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define REPL_HAS_ALIGNOF
#define REPL_ALIGNOF(type) _Alignof(type)
#elif defined(__GNUC__) || defined(__clang__)
#define REPL_HAS_ALIGNOF
#define REPL_ALIGNOF(type) __alignof__(type)
#else
#warning "Need _Alignof or compiler __alignof__ support for ReplRuntimeState layout check"
#endif

#define REPL_ALIGN_UP(value, alignment)                                                           \
    (((value) + ((alignment) - 1)) & ~((alignment) - 1))

#define REPL_FIELD_END(type, name)                                                                \
    (offsetof(ReplRuntimeState, name) + sizeof(((ReplRuntimeState *)0)->name))

#define REPL_RUNTIME_STATE_LAST_FIELD_END                                                         \
    REPL_FIELD_END(ReplImportExportState, import_export)

#define REPL_RUNTIME_STATE_EXPECTED_SIZE                                                          \
    REPL_ALIGN_UP(REPL_RUNTIME_STATE_LAST_FIELD_END, REPL_ALIGNOF(ReplRuntimeState))

#if defined(REPL_HAS_ALIGNOF)
    STATIC_ASSERT(REPL_RUNTIME_STATE_EXPECTED_SIZE == sizeof(ReplRuntimeState),
                   "ReplRuntimeState layout list does not end at the actual struct size,\n\n"
                   "!!!! vvvvv READ vvvvv  !!!!\n"
                   "Please make sure ALL ReplRuntimeState fields are included in "
                   "REPL_RUNTIME_STATE_FIELDS\n"
                   "!!!! ^^^^^ READ ^^^^^  !!!!\n\n");
#endif

#define REPL_PRINT_FIELD(type, name)                                                               \
    do {                                                                                           \
        size_t offset = offsetof(ReplRuntimeState, name);                                          \
        size_t size = sizeof(((ReplRuntimeState *)0)->name);                                       \
        size_t end = offset + size;                                                                \
        size_t gap = 0;                                                                            \
        if (offset > previous_end)                                                                 \
            gap = offset - previous_end;                                                           \
        fprintf(dst, "%-28s offset=%9zu size=%9.3f KB end=%9zu gap_before=%5zu\n", #type, offset,  \
                size / 1024.0, end, gap);                                                          \
        previous_end = end;                                                                        \
    } while (0);

    size_t previous_end = 0;
    size_t struct_size = sizeof(ReplRuntimeState);
    size_t tail_padding = 0;

    fprintf(dst, "ReplRuntimeState layout:\n");
    fprintf(dst, "----------------------------------------------------------------------------\n");

    REPL_RUNTIME_STATE_FIELDS(REPL_PRINT_FIELD)

    if (struct_size > previous_end)
        tail_padding = struct_size - previous_end;

    fprintf(dst, "----------------------------------------------------------------------------\n");
    fprintf(dst, "%-24s size=%5zu\n", "last field end", previous_end);
    fprintf(dst, "%-24s size=%5zu\n", "struct total", struct_size);
    fprintf(dst, "%-24s size=%5zu\n", "tail padding", tail_padding);

#if defined(REPL_HAS_ALIGNOF)
    fprintf(dst, "%-24s size=%5zu\n", "struct alignment",
            (size_t)REPL_ALIGNOF(ReplRuntimeState));
    fprintf(dst, "%-24s size=%5zu\n", "expected total",
            (size_t)REPL_RUNTIME_STATE_EXPECTED_SIZE);
#endif

    fflush(dst);

#undef REPL_PRINT_FIELD
#undef REPL_RUNTIME_STATE_EXPECTED_SIZE
#undef REPL_RUNTIME_STATE_LAST_FIELD_END
#undef REPL_FIELD_END
#undef REPL_ALIGN_UP
#undef REPL_HAS_ALIGNOF
#undef REPL_ALIGNOF
#undef REPL_RUNTIME_STATE_FIELDS
}

/* CLI dump dispatch — see glr_debug.h. Keeps the GL-free bootstrap + dump
 * sequence out of main(): the same @cfg/status services install as the
 * windowed path, but nothing renders, so status messages are silent. */
int glr_debug_run_dumps(const GlrCliOptions *opts, FILE *out) {
    FILE *dst = out ? out : stdout;

    if (!(opts->dump_code || opts->dump_flat || opts->dump_flat_histogram ||
          opts->dump_state_layout))
        return 0;

    /* Dump-only paths skip glr_ctrl_init_gl (no GL context, no window).
     * glr_ctrl_bootstrap_repl installs the app services (status sink +
     * export-config bridge) at its top so @cfg in imported files is applied
     * even on the dump path. */
    if (opts->dump_code || opts->dump_flat || opts->dump_flat_histogram) {
        glr_ctrl_bootstrap_repl(opts->input_file);
        /* --example works on the dump paths too: the loader chain (reset
         * transients, undo note, repl_load_example) is GL-free, so built-ins
         * can be inspected without a window. */
        if (opts->example_index >= 0)
            glr_scene_load_example(opts->example_index);
    }
    if (opts->dump_code)
        glr_debug_dump_current_editor(dst);
    if (opts->dump_flat)
        glr_debug_dump_current_flat_commands_sync(dst);
    if (opts->dump_flat_histogram)
        glr_debug_dump_current_flat_histogram(dst);
    if (opts->dump_state_layout)
        glr_debug_dump_runtime_state_layout(dst);
    return 1;
}
