/*
 * src/repl/export_format_shared.h - Shared import/export format vocabulary.
 *
 * Keep this header narrow: it owns the round-trip string constants and the
 * import/export state access macros used by both the reader and writer halves.
 * Parser/emitter helper functions stay in their owning translation units.
 */
#ifndef REPL_EXPORT_FORMAT_SHARED_H
#define REPL_EXPORT_FORMAT_SHARED_H

#include "repl/state_owners.h"

/* Read and write access to the ReplImportExportState slice.
 *
 * Spell these at the call site - `IMPORT_EXPORT_VIEW.cam_lines`,
 * `IMPORT_EXPORT_WRITABLE->cam_lines`. There used to be a `g_cam_lines` /
 * `g_cam_lines_writable` alias per field, a transitional shim from the
 * state-facade refactor that kept the pre-facade call sites compiling. It
 * outlived its transition and lied twice: `g_` is this tree's marker for a
 * **file-private static** (CLAUDE.md, Conventions) and these are neither
 * file-private nor variables - VIEW expands to a call returning a by-value
 * view struct, WRITABLE to one returning a pointer, so a reader who trusted
 * the prefix would assume a cheap load and might write it in a loop. The
 * read/write split also belongs in the expression, not in a name suffix:
 * these two spellings show at the call site which one it is. */
#define IMPORT_EXPORT_VIEW     (repl_state_import_export())
#define IMPORT_EXPORT_WRITABLE (repl_state_import_export_writable())

#define REPL_WORKSPACE_DIRECTIVE_SCENE_NAME    "scene-name"
#define REPL_WORKSPACE_DIRECTIVE_WORKSPACE_DIR "workspace-dir"
#define REPL_WORKSPACE_DIRECTIVE_VAR           "var"
#define REPL_WORKSPACE_DIRECTIVE_FUNC          "func"
#define REPL_WORKSPACE_DIRECTIVE_CFG           "cfg"
#define REPL_WORKSPACE_BANNER_DIRECTIVE        "workspace"
#define REPL_WORKSPACE_BANNER_DIRECTIVE_PREFIX "workspace:"
#define REPL_WORKSPACE_HEADER_BANNER \
    "/* @workspace: REPL state (auto-saved) */"

#define REPL_SNIPPET_DIRECTIVE_DECLARE       "declare"
#define REPL_SNIPPET_DIRECTIVE_FUNC_BODY     "func-body"
/* Trailing marker on a normal the autonormal pass generated (GLCmd.is_auto).
 * Unlike the directives above it rides on the command's own line rather than
 * owning one, so the exported file keeps one line per source row. Written by
 * export_cmd_writer.c, read and stripped by import_feed_one_line(). */
#define REPL_EXPORT_AUTO_NORMAL_MARKER       "@auto"
#define REPL_EXPORT_C89_LOOP_SCOPE_MARKER    "repl-export-c89-loop-scope"
#define REPL_EXPORT_C89_LOOP_VAR_MARKER      "repl-export-c89-loop-var"
#define REPL_EXPORT_FORWARD_DECL_MARKER      "repl-export-forward-decl"
#define REPL_EXPORT_GLFLOAT1_HELPER          "repl_glfloat1"
#define REPL_EXPORT_GLFLOAT3_HELPER          "repl_glfloat3"
#define REPL_EXPORT_GLFLOAT4_HELPER          "repl_glfloat4"
#define REPL_EXPORT_GLDOUBLE4_HELPER         "repl_gldouble4"
#define REPL_EXPORT_GLFLOAT16_HELPER         "repl_glfloat16"

#endif /* REPL_EXPORT_FORMAT_SHARED_H */
