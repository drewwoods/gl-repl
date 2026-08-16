/*
 * glr_debug.c - Diagnostic dumps for CLI flags and tests.
 */
#include <stdio.h>
#include "repl/flatten.h"
#include "source_document.h"
#include "app/glr_debug.h"

#include "editor/state.h"
#include "repl/export.h"
#include "repl/command_spec.h"
#include "repl/eval.h"       /* repl_func_alias_get, REPL_FUNC_SLOT_COUNT */
#include "repl/text_helpers.h" /* parse_repl_func_signature */
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

/* Argument half of a flat-dump row: `num_args`, the live `args[]`, and
 * the payload member this command type owns (reading an inactive union
 * member would be undefined, so the switch is not a convenience).
 *
 * Floats print as `%a`, not `%g`. The dump's job here is to let two
 * flattenings of the same program be compared byte for byte - refactors
 * that move a value between storage kinds must not move the value - and
 * a decimal rendering can hide a difference below its own precision,
 * which would defeat exactly that use. Provenance and storage metadata
 * (`src_cmd_idx`, `call_src_cmd_idx`, `root_call_src_cmd_idx`,
 * `var_idx`) legitimately shift under such a refactor; filter those
 * columns out rather than expecting an empty raw diff. */
static void debug_dump_flat_args(FILE *dst, const GLCmd *cmd) {
    int arg_count = cmd->num_args;
    const int arg_cap = (int)(sizeof(cmd->args) / sizeof(cmd->args[0]));

    if (arg_count < 0) arg_count = 0;
    if (arg_count > arg_cap) arg_count = arg_cap;

    fprintf(dst, "args=%d[", arg_count);
    for (int arg_idx = 0; arg_idx < arg_count; arg_idx++)
        fprintf(dst, "%s%a", arg_idx ? ", " : "",
                (double)cmd->args[arg_idx]);
    fprintf(dst, "]");

    switch (cmd->type) {
    case CMD_MULT_MATRIXF:
        fprintf(dst, " m[");
        for (int cell = 0; cell < REPL_MATRIX_CELL_COUNT; cell++)
            fprintf(dst, "%s%a", cell ? ", " : "",
                    (double)cmd->payload.matrix.m[cell]);
        fprintf(dst, "]");
        break;
    case CMD_LABEL:
        fprintf(dst, " fmt=\"%s\"", cmd->payload.label.fmt);
        break;
    case CMD_VAR_DECLARE:
        fprintf(dst, " decl[");
        for (int name_idx = 0; name_idx < cmd->payload.decl.count &&
                               name_idx < MAX_NAMES_PER_DECL; name_idx++)
            fprintf(dst, "%s%s", name_idx ? ", " : "",
                    cmd->payload.decl.names[name_idx]);
        fprintf(dst, "]");
        break;
    case CMD_VAR_ASSIGN:
        if (cmd->var_idx == REPL_VAR_IDX_LOCAL)
            fprintf(dst, " prev_local=%a",
                    (double)cmd->payload.assign.prev_local_value);
        break;
    default:
        break;
    }
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
    fprintf(dst, "call_frames=%d/%d args=%d/%d overflow=%d\n",
            flat_program.call_frame_count, MAX_CALL_FRAMES,
            flat_program.call_frame_arg_count, MAX_CALL_FRAME_ARGS,
            flat_program.call_frame_overflow);

    for (int flat_idx = 0; flat_idx < num_flat_cmds; flat_idx++) {
        const GLCmd *cmd = &flat_cmds[flat_idx];
        const char *line_text = source_text_line(text, cmd->src_cmd_idx);
        fprintf(dst,
                "%4d | %-22s | valid=%d has_vars=%d src_idx=%d call_src_idx=%d root_call_src_idx=%d depth=%d frame=%d var_idx=%d func_scope=0x%08x | ",
                flat_idx, repl_cmd_type_name(cmd->type), cmd->valid,
                cmd->has_vars, cmd->src_cmd_idx, cmd->call_src_cmd_idx,
                cmd->root_call_src_cmd_idx, cmd->call_depth,
                repl_flat_cmd_call_frame(&flat_program, flat_idx),
                cmd->var_idx, cmd->func_scope_mask);
        debug_dump_flat_args(dst, cmd);
        fprintf(dst, " | %s\n", line_text ? line_text : "");
    }
    fprintf(dst, "=== End REPL Flattened Commands Dump ===\n");
    fflush(dst);
}

/* --flat-histogram: where the MAX_FLAT_COMMANDS flatten budget is being
 * spent. Two sections: per-function inclusive costs (func_scope_mask -
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
     * dump over <= MAX_EDITOR_COMMANDS entries - simplicity over speed. */
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

static const char *debug_call_frame_func_name(int func_slot, char *buf, int buf_sz) {
    const char *alias = repl_func_alias_get(func_slot);

    if (alias && alias[0])
        return alias;
    snprintf(buf, (size_t)buf_sz, "func%d", func_slot);
    return buf;
}

static void debug_dump_call_frame_line(FILE *dst, FlatProgramView view,
                                       SourceTextView text, int frame,
                                       int indent) {
    const ReplCallFrame *f;
    char name_buf[REPL_FUNC_NAME_MAX];
    const char *name;
    char param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
    int param_count = 0;
    int def_fn = -1;
    int a;

    if (!repl_call_frame_ok(&view, frame))
        return;
    f = &view.call_frames[frame];
    name = debug_call_frame_func_name(f->func_slot, name_buf, (int)sizeof(name_buf));

    for (int i = 0; i < indent; i++)
        fputc(' ', dst);
    fprintf(dst, "[%d] %s(", frame, name);

    /* Names are derived from the current CMD_FUNC_DEF, not stored. */
    {
        const GLCmd *doc = repl_state_document_cmds();
        int doc_count = repl_state_document_count();
        for (int i = 0; i < doc_count; i++) {
            if (doc[i].type == CMD_FUNC_DEF &&
                (int)doc[i].args[0] == f->func_slot) {
                const char *line = source_text_line(text, i);
                parse_repl_func_signature(line ? line : "",
                                          &def_fn, param_names,
                                          MAX_EXPR_VARS, &param_count);
                break;
            }
        }
    }
    for (a = 0; a < f->arg_count; a++) {
        float val = 0.0f;
        if (view.call_frame_args &&
            f->arg_offset + a >= 0 &&
            f->arg_offset + a < view.call_frame_arg_count)
            val = view.call_frame_args[f->arg_offset + a];
        if (a < param_count && param_names[a][0])
            fprintf(dst, "%s%s=%g", a ? ", " : "", param_names[a], (double)val);
        else
            fprintf(dst, "%s%g", a ? ", " : "", (double)val);
    }
    fprintf(dst, ")  site=%d  flat=[%d, %d)  depth=%d\n",
            f->call_src_cmd_idx, f->flat_begin, f->flat_end, f->depth);
}

static void debug_dump_call_frame_tree(FILE *dst, FlatProgramView view,
                                       SourceTextView text, int parent,
                                       int indent) {
    int frame;

    for (frame = 0; frame < view.call_frame_count; frame++) {
        if (!repl_call_frame_ok(&view, frame))
            continue;
        if (view.call_frames[frame].parent != parent)
            continue;
        debug_dump_call_frame_line(dst, view, text, frame, indent);
        debug_dump_call_frame_tree(dst, view, text, frame, indent + 2);
    }
}

void glr_debug_dump_call_tree(FILE *out, SourceTextView text) {
    FILE *dst = out ? out : stdout;
    FlatProgramView view;

    repl_refresh_flat_program(editor_state_edit_line());
    view = repl_state_flat_program_view();

    fprintf(dst, "=== REPL Call Tree ===\n");
    fprintf(dst, "frames: %d/%d  args: %d/%d\n",
            view.call_frame_count, MAX_CALL_FRAMES,
            view.call_frame_arg_count, MAX_CALL_FRAME_ARGS);
    if (view.call_frame_overflow)
        fprintf(dst,
                "NOTE: call-frame table overflow; later invocations are unindexed\n");
    debug_dump_call_frame_tree(dst, view, text, REPL_CALL_FRAME_NONE, 0);
    fprintf(dst, "reconstructed frames: %d\n", view.call_frame_count);
    fprintf(dst, "=== End REPL Call Tree ===\n");
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

void glr_debug_dump_current_call_tree(FILE *out) {
    glr_debug_dump_call_tree(out, source_document_view());
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
