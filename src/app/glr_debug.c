/*
 * glr_debug.c - Diagnostic dumps for CLI flags and tests.
 */
#include "app/glr_debug.h"

#include "repl/export.h"
#include "repl/command_spec.h"
#include "repl/pipeline.h"
#include "repl/state_owners.h"
#include "repl/state.h"

#include <stddef.h>

#include <c_compat.h>  /* STATIC_ASSERT (C99/C11 portable) */

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

void glr_debug_dump_flat_commands(FILE *out, EditorBufferView text) {
    FILE *dst = out ? out : stdout;
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *flat_cmds = flat_program.cmds;
    int num_flat_cmds = flat_program.cmd_count;

    if (repl_state_flat_program_dirty()) {
        repl_flatten_commands(editor_state_edit_line());
        flat_program = repl_state_flat_program_view();
        flat_cmds = flat_program.cmds;
        num_flat_cmds = flat_program.cmd_count;
    }

    fprintf(dst, "=== REPL Flattened Commands Dump ===\n");
    fprintf(dst, "num_flat_cmds=%d\n", num_flat_cmds);

    for (int flat_idx = 0; flat_idx < num_flat_cmds; flat_idx++) {
        const GLCmd *cmd = &flat_cmds[flat_idx];
        const char *line_text = editor_buffer_view_line(text, cmd->src_cmd_idx);
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

void glr_debug_dump_runtime_state_layout(FILE *out) {
    FILE *dst = out ? out : stdout;

/* The runtime-state layout dump no longer includes ReplReplayRuntimeState,
 * the presentation slice, or render-config toggles: those moved off
 * ReplRuntimeState to replay_state.c and glr_state.c respectively. (Replay
 * migration was Phase F commit 33; presentation/render relocation was
 * step 7a of feature/decouple-repl-from-gl-repl-alt.md.) */
#define REPL_RUNTIME_STATE_FIELDS(X)                                                               \
    X(ReplDocumentState, document)                                                                 \
    X(ReplFlatProgramState, flat_program)                                                          \
    X(ReplVariableState, variables)                                                                \
    X(ReplRenderState, render)                                                                     \
    X(ReplSceneRuntimeState, scenes)                                                               \
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
