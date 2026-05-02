/*
 * repl_debug.c - Diagnostic dumps for CLI flags and tests.
 */
#include "repl_debug.h"

#include "sample.h"
#include "repl_command_spec.h"
#include "repl_pipeline.h"
#include "repl_state.h"

#include <stddef.h>

void repl_debug_dump_editor(FILE *out) {
    FILE *dst = out ? out : stdout;

    fprintf(dst, "=== REPL Editor Dump ===\n");
    fprintf(dst,
            "num_cmds=%d edit_line=%d inserting=%d flat_dirty=%d normals_dirty=%d\n",
            repl_state_document_count(), repl_state_edit_line(),
            repl_state_insert_mode(), repl_state_flat_program_dirty(),
            repl_state_normals_dirty());

    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        const GLCmd *cmd = &repl_state_document_cmds()[cmd_idx];
        fprintf(dst,
                "%4d | %-22s | valid=%d has_vars=%d is_auto=%d src_idx=%d | %s\n",
                cmd_idx, repl_cmd_type_name(cmd->type), cmd->valid,
                cmd->has_vars, cmd->is_auto, cmd->src_cmd_idx,
                editor_buffer_line(cmd_idx) ?
                    editor_buffer_line(cmd_idx) : "");
    }

    fprintf(dst, "--- source ---\n");
    for (int cmd_idx = 0; cmd_idx < repl_state_document_count(); cmd_idx++) {
        const GLCmd *cmd = &repl_state_document_cmds()[cmd_idx];
        if (!cmd->valid) continue;
        fprintf(dst, "%s\n", editor_buffer_line(cmd_idx) ?
                              editor_buffer_line(cmd_idx) : "");
    }
    fprintf(dst, "--- camera ---\n");
    {
        ReplCameraState cam = repl_state_camera();
        fprintf(dst, "rx=%g ry=%g dist=%g tx=%g ty=%g tz=%g\n",
                (double)cam.rx, (double)cam.ry, (double)cam.dist,
                (double)cam.tx, (double)cam.ty, (double)cam.tz);
    }
    update_cam_lines();
    {
        ReplImportExportView meta = repl_state_import_export();
        for (int cam_line_idx = 0; cam_line_idx < CAM_LINE_COUNT; cam_line_idx++)
            fprintf(dst, "%s\n", meta.cam_lines[cam_line_idx]);
    }
    fprintf(dst, "--- init ---\n");
    for (int init_line_idx = 0; init_line_idx < init_section_line_count(); init_line_idx++) {
        char line[MAX_LINE_LEN];
        init_section_line(init_line_idx, line, sizeof(line));
        fprintf(dst, "%s\n", line);
    }
    fprintf(dst, "=== End REPL Editor Dump ===\n");
    fflush(dst);
}

void repl_debug_dump_flat_commands(FILE *out) {
    FILE *dst = out ? out : stdout;
    FlatProgramView flat_program = repl_state_flat_program_view();
    const GLCmd *g_flat_cmds = flat_program.cmds;
    int g_num_flat_cmds = flat_program.cmd_count;

    if (repl_state_flat_program_dirty()) {
        flatten_commands();
        flat_program = repl_state_flat_program_view();
        g_flat_cmds = flat_program.cmds;
        g_num_flat_cmds = flat_program.cmd_count;
    }

    fprintf(dst, "=== REPL Flattened Commands Dump ===\n");
    fprintf(dst, "num_flat_cmds=%d\n", g_num_flat_cmds);

    for (int flat_idx = 0; flat_idx < g_num_flat_cmds; flat_idx++) {
        const GLCmd *cmd = &g_flat_cmds[flat_idx];
        fprintf(dst,
                "%4d | %-22s | valid=%d has_vars=%d src_idx=%d call_src_idx=%d root_call_src_idx=%d func_scope=0x%08x | %s\n",
                flat_idx, repl_cmd_type_name(cmd->type), cmd->valid,
                cmd->has_vars, cmd->src_cmd_idx, cmd->call_src_cmd_idx,
                cmd->root_call_src_cmd_idx, cmd->func_scope_mask,
                editor_buffer_line(cmd->src_cmd_idx) ?
                    editor_buffer_line(cmd->src_cmd_idx) : "");
    }
    fprintf(dst, "=== End REPL Flattened Commands Dump ===\n");
    fflush(dst);
}

void repl_debug_dump_runtime_state_layout(FILE *out) {
    FILE *dst = out ? out : stdout;

#define REPL_RUNTIME_STATE_FIELDS(X)                                                               \
    X(ReplDocumentState, document)                                                                 \
    X(ReplFlatProgramState, flat_program)                                                          \
    X(ReplVariableState, variables)                                                                \
    X(ReplCodePanelRuntimeState, code_panel)                                                       \
    X(ReplHelpState, help)                                                                         \
    X(ReplVariablePanelState, variable_panel)                                                      \
    X(ReplVariableDragState, variable_drag)                                                        \
    X(ReplProfilePanelState, profile_panel)                                                        \
    X(ReplStatusState, status)                                                                     \
    X(ReplCameraState, camera)                                                                     \
    X(ReplPointerState, pointer)                                                                   \
    X(ReplViewportState, viewport)                                                                 \
    X(ReplPresentationState, presentation)                                                         \
    X(ReplRenderState, render)                                                                     \
    X(ReplReplayRuntimeState, replay)                                                              \
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
    _Static_assert(REPL_RUNTIME_STATE_EXPECTED_SIZE == sizeof(ReplRuntimeState),
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
