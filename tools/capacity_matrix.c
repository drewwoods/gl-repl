/*
 * tools/capacity_matrix.c - print state-scaling matrix for tunable
 * MAX_* / depth constants in the gl-repl tree.
 *
 * Run via `make capacity-matrix`. The output table shows, for each
 * tunable constant: its current compile-time value, the per-unit
 * memory cost (bytes added to total state when the constant grows
 * by 1), the total bytes contributed at the current setting, and
 * the dominant storage line.
 *
 * The per-unit math is hand-curated. Each row sums every long-lived
 * (.bss / .data / static) array dimension that scales with the
 * constant - transient stack arrays don't count. When you add a new
 * MAX_* or change a struct that uses one, append (or update) a row.
 *
 * The numbers are deliberately conservative - file-private structs
 * (UserScene in src/repl/scenes.c) are computed approximately because
 * their definitions aren't visible from a tool TU. Notes call out
 * those cases.
 */
#include "editor/limits.h"
#include "editor/undo.h"
#include "repl/command.h"
#include "repl/command_spec.h"
#include "repl/compile.h"
#include "repl/flatten.h"   /* FlatCmdLocalVars (MAX_FLAT_COMMANDS row) */
#include <stdbool.h>
#include "repl/scenes.h"
#include "repl/eval.h"
#include "repl/export_state.h"
#include "subsystems/replay/replay.h"
#include "render3d/render_types.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Must match the storage definitions in editor_undo.c and friends. */
#define UNDO_RING_DEPTH         32
#define UNDO_REDO_RING_PAIRS    2

typedef struct {
    const char *name;
    long        value;
    size_t      per_unit_bytes;
    const char *notes;
} CapRow;

static const CapRow rows[] = {
    /* Source-line capacity. Fans out into every full document copy. */
    { "MAX_EDITOR_COMMANDS", MAX_EDITOR_COMMANDS,
      sizeof(GLCmd) * UNDO_RING_DEPTH * UNDO_REDO_RING_PAIRS
        + sizeof(GLCmd) * 1 /* live document */
        + MAX_LINE_LEN * UNDO_RING_DEPTH * UNDO_REDO_RING_PAIRS
        + MAX_LINE_LEN * 1 /* editor_buffer */
        + (sizeof(GLCmd) + MAX_LINE_LEN) * MAX_USER_SCENES,
      "undo+redo rings (x64) + document + editor_buffer + user scenes (x8)" },

    /* Flat-program capacity (split from the source cap 2026-07-10).
     * One live array pair - no undo/scene fan-out. */
    { "MAX_FLAT_COMMANDS", MAX_FLAT_COMMANDS,
      sizeof(GLCmd) + sizeof(FlatCmdLocalVars) + sizeof(int)
        + sizeof(unsigned) + sizeof(int) + sizeof(float)
          /* VertexLabelSticky (file-private to edit_overlays.c) */,
      "ReplFlatProgramState.cmds + .local_vars + .call_frame_idx + vertex-label sticky table" },

    { "MAX_CALL_FRAMES", MAX_CALL_FRAMES,
      sizeof(ReplCallFrame),
      "ReplFlatProgramState.call_frames[] - interned invocation topology" },

    { "MAX_CALL_FRAME_ARGS", MAX_CALL_FRAME_ARGS,
      sizeof(float),
      "ReplFlatProgramState.call_frame_args[] - per-frame argument arena" },

    /* Per-line text width. Multiplies into every full-document buffer. */
    { "MAX_LINE_LEN", MAX_LINE_LEN,
      MAX_EDITOR_COMMANDS * UNDO_RING_DEPTH * UNDO_REDO_RING_PAIRS
        + MAX_EDITOR_COMMANDS
        + MAX_EDITOR_COMMANDS * MAX_USER_SCENES
        + MAX_WORKSPACE_HEADER_LINES
        + MAX_INPUT_LEN /* editor input + ghost + hint scratch ~ a few input slots */,
      "editor_buffer + undo/redo (x64) + user scenes (x8) lines, all multiplied by MAX_EDITOR_COMMANDS" },

    /* Single-line input buffer + a small handful of scratch buffers. */
    { "MAX_INPUT_LEN", MAX_INPUT_LEN,
      6, /* input + pending_newline + autocomplete ghost + hint + line_buf + status approx */
      "editor input + pending_newline + autocomplete ghost/hint + status scratch" },

    /* Predefined-variable table size. Mostly dwarfed by undo rings. */
    { "MAX_PREDEF_VARS", MAX_PREDEF_VARS,
      sizeof(ExprVar) * 3 /* g_predef_vars + g_fallback + ReplPredefView */
        + (sizeof(float) + 16) * UNDO_RING_DEPTH * UNDO_REDO_RING_PAIRS
        + (sizeof(float) + 16) * MAX_USER_SCENES
        + sizeof(float) /* replay baseline */,
      "ExprVar tables + undo/redo rings x 64 x (vals + 16-char names) + user scenes x 8" },

    /* Per-expression visible-scope size. Mostly transient. */
    { "MAX_EXPR_VARS", MAX_EXPR_VARS,
      0,
      "transient stack scopes (compile + autocomplete); durable use sits inside FlatCmdLocalVars, counted under MAX_FLAT_COMMANDS" },

    /* Names per `float a, b, c;` declaration. */
    { "MAX_NAMES_PER_DECL", MAX_NAMES_PER_DECL,
      sizeof(ExprVar) * 2 /* GLCmd.payload.decl.names slots in document + flat */
        + sizeof(ExprVar) * 0, /* nominally inside the command rows too */
      "GLCmd.payload.decl.names[][16]; effective cost folds into MAX_EDITOR_COMMANDS + MAX_FLAT_COMMANDS" },

    /* User-scene slots. Each slot holds a full document copy. */
    { "MAX_USER_SCENES", MAX_USER_SCENES,
      sizeof(GLCmd) * MAX_EDITOR_COMMANDS
        + MAX_LINE_LEN * MAX_EDITOR_COMMANDS
        + (sizeof(float) + 16) * MAX_PREDEF_VARS
        + 96 /* approximate: name + scene_cfg + last_touch + edit_line + flags */,
      "approx 1 UserScene = 1 EditorUndoSnapshot worth of bytes (file-private struct)" },

    /* Compile-time temp arrays. Per-call only. */
    { "MAX_COMMIT_CMDS", MAX_COMMIT_CMDS,
      sizeof(GLCmd) + MAX_LINE_LEN, /* per ReplCompiledChange instance, but those are stack-only */
      "ReplCompiledChange.cmds[]/.text[][] - per-call stack only, no durable state" },

    /* Number of GL_LIGHT slots tracked by the renderer. */
    { "MAX_LIGHTS", MAX_LIGHTS,
      sizeof(Render3dLight),
      "GlrRenderState.lights[] (theme-seeded; REPL owns only an enable bitmask)" },

    /* Undo / redo ring depth. Multiplies the entire snapshot cost. */
    { "REPL_UNDO_DEPTH", UNDO_RING_DEPTH,
      sizeof(EditorUndoSnapshot) * UNDO_REDO_RING_PAIRS,
      "doubles the per-snapshot cost across both rings" },

    /* Replay fade ring. Per-batch cost depends on flat-program size. */
    { "REPLAY_FADE_BATCH_MAX", REPLAY_FADE_BATCH_MAX,
      0,
      "fade batch ring (struct private to replay.c; dominant cost is the captured GLCmd[] view, not the index entries)" },

    /* Function-hint metadata table. */
    { "MAX_FUNC_HINT_PARAMS", MAX_FUNC_HINT_PARAMS,
      sizeof(char *),
      "ReplFuncCompletion.params[] - pointers into static const strings" },

    /* Autocomplete match list size. */
    { "MAX_AC_MATCHES", MAX_AC_MATCHES,
      3 * sizeof(char *) + sizeof(int),
      "EditorAutocompleteState (matches[], insert_matches[]) + parallel provider statics (g_ac_func_matches[], g_ac_alias_slots[])" },

    /* Workspace header line cap. Each line is a full source-line-length string. */
    { "MAX_WORKSPACE_HEADER_LINES", MAX_WORKSPACE_HEADER_LINES,
      MAX_LINE_LEN /* WORKSPACE_HEADER_LINE_LEN aliases MAX_LINE_LEN today */,
      "ReplImportExportState.workspace_header_lines[][MAX_LINE_LEN]" },
};

static void format_bytes(size_t b, char *buf, size_t bufsz) {
    if (b >= 1024UL * 1024UL) {
        snprintf(buf, bufsz, "%.2f MB", (double)b / (1024.0 * 1024.0));
    } else if (b >= 1024UL) {
        snprintf(buf, bufsz, "%.2f KB", (double)b / 1024.0);
    } else {
        snprintf(buf, bufsz, "%zu B", b);
    }
}

int main(void) {
    printf("\n");
    printf("Capacity matrix - gl-repl state scaling\n");
    printf("=======================================\n\n");

    printf("Major struct sizes (sizeof at the current configuration):\n");
    printf("  GLCmd                     = %zu B\n", sizeof(GLCmd));
    printf("  ExprVar                   = %zu B\n", sizeof(ExprVar));
    printf("  ReplPredefView            = %zu B\n", sizeof(ReplPredefView));
    printf("  EditorUndoSnapshot          = %zu B (%.2f MB)\n",
           sizeof(EditorUndoSnapshot),
           (double)sizeof(EditorUndoSnapshot) / (1024.0 * 1024.0));
    printf("  ReplCompiledChange        = %zu B (%.2f KB)\n",
           sizeof(ReplCompiledChange),
           (double)sizeof(ReplCompiledChange) / 1024.0);
    printf("  Undo+redo rings (64 snapshots) = %.2f MB\n",
           (double)(sizeof(EditorUndoSnapshot) * UNDO_RING_DEPTH * UNDO_REDO_RING_PAIRS)
               / (1024.0 * 1024.0));
    printf("\n");

    /* Header */
    printf("%-30s %10s %16s %16s   %s\n",
           "Constant", "Value", "Per-unit", "Current total", "Notes");
    printf("%-30s %10s %16s %16s   %s\n",
           "------------------------------",
           "----------",
           "----------------",
           "----------------",
           "-----");

    size_t durable_total = 0;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const CapRow *r = &rows[i];
        char per[32], total[32];
        size_t cur = r->per_unit_bytes * (size_t)r->value;
        format_bytes(r->per_unit_bytes, per, sizeof(per));
        format_bytes(cur, total, sizeof(total));
        printf("%-30s %10ld %16s %16s   %s\n",
               r->name, r->value, per, total, r->notes);
        durable_total += cur;
    }

    printf("\n");
    {
        char buf[32];
        format_bytes(durable_total, buf, sizeof(buf));
        printf("Sum across rows (rough total durable state): %s\n", buf);
    }
    printf("Note: rows overlap (e.g. MAX_EDITOR_COMMANDS and MAX_LINE_LEN both\n");
    printf("count editor_buffer + undo rings) - the sum overstates the\n");
    printf("true footprint. For an authoritative figure, sum sizeof of\n");
    printf("g_repl_state + g_undo_buf + g_redo_buf + g_user_scenes.\n\n");

    /* Quick "what if I bumped this?" examples for the most common
     * tweaks. Hand-coded delta: per-unit x (target - current). */
    printf("Common tweaks:\n");
    {
        const long predef_at = MAX_PREDEF_VARS;
        char buf32[32], buf64[32], buf256[32];
        size_t per = 0;
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
            if (rows[i].value == predef_at &&
                __builtin_strcmp(rows[i].name, "MAX_PREDEF_VARS") == 0)
                per = rows[i].per_unit_bytes;
        format_bytes(per * 16,  buf32,  sizeof(buf32));
        format_bytes(per * 48,  buf64,  sizeof(buf64));
        format_bytes(per * 240, buf256, sizeof(buf256));
        printf("  MAX_PREDEF_VARS %ld -> 32  : +%s\n", predef_at, buf32);
        printf("  MAX_PREDEF_VARS %ld -> 64  : +%s\n", predef_at, buf64);
        printf("  MAX_PREDEF_VARS %ld -> 256 : +%s\n", predef_at, buf256);
    }
    {
        char dbl[32], quad[32];
        size_t per = 0;
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
            if (__builtin_strcmp(rows[i].name, "MAX_EDITOR_COMMANDS") == 0)
                per = rows[i].per_unit_bytes;
        format_bytes(per * MAX_EDITOR_COMMANDS,     dbl,  sizeof(dbl));
        format_bytes(per * MAX_EDITOR_COMMANDS * 3, quad, sizeof(quad));
        printf("  MAX_EDITOR_COMMANDS %d -> %d : +%s\n",
               MAX_EDITOR_COMMANDS, MAX_EDITOR_COMMANDS * 2, dbl);
        printf("  MAX_EDITOR_COMMANDS %d -> %d : +%s\n",
               MAX_EDITOR_COMMANDS, MAX_EDITOR_COMMANDS * 4, quad);
    }
    {
        char dbl[32];
        size_t per = 0;
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
            if (__builtin_strcmp(rows[i].name, "MAX_FLAT_COMMANDS") == 0)
                per = rows[i].per_unit_bytes;
        format_bytes(per * MAX_FLAT_COMMANDS, dbl, sizeof(dbl));
        printf("  MAX_FLAT_COMMANDS %d -> %d : +%s\n",
               MAX_FLAT_COMMANDS, MAX_FLAT_COMMANDS * 2, dbl);
    }
    printf("\n");
    return 0;
}
