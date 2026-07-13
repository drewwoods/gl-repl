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

#define IMPORT_EXPORT_VIEW     (repl_state_import_export())
#define IMPORT_EXPORT_WRITABLE (repl_state_import_export_writable())

#define g_workspace_header_lines      (IMPORT_EXPORT_VIEW.workspace_header_lines)
#define g_workspace_header_line_count (IMPORT_EXPORT_VIEW.workspace_header_line_count)
#define g_render_state_lines          (IMPORT_EXPORT_VIEW.render_state_lines)
#define g_camera_comment_line         (IMPORT_EXPORT_VIEW.camera_comment_line)
#define g_cam_lines                   (IMPORT_EXPORT_VIEW.cam_lines)
#define g_export_scene_name_hint      (IMPORT_EXPORT_VIEW.export_scene_name_hint)
#define g_pending_scene_name          (IMPORT_EXPORT_VIEW.pending_scene_name)
#define g_pending_workspace_dir       (IMPORT_EXPORT_VIEW.pending_workspace_dir)

#define g_workspace_header_lines_writable      (IMPORT_EXPORT_WRITABLE->workspace_header_lines)
#define g_workspace_header_line_count_writable (IMPORT_EXPORT_WRITABLE->workspace_header_line_count)
#define g_render_state_lines_writable          (IMPORT_EXPORT_WRITABLE->render_state_lines)
#define g_camera_comment_line_writable         (IMPORT_EXPORT_WRITABLE->camera_comment_line)
#define g_cam_lines_writable                   (IMPORT_EXPORT_WRITABLE->cam_lines)
#define g_export_scene_name_hint_writable      (IMPORT_EXPORT_WRITABLE->export_scene_name_hint)
#define g_pending_scene_name_writable          (IMPORT_EXPORT_WRITABLE->pending_scene_name)
#define g_pending_workspace_dir_writable       (IMPORT_EXPORT_WRITABLE->pending_workspace_dir)

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
#define REPL_EXPORT_C89_LOOP_SCOPE_MARKER    "repl-export-c89-loop-scope"
#define REPL_EXPORT_C89_LOOP_VAR_MARKER      "repl-export-c89-loop-var"
#define REPL_EXPORT_GLFLOAT1_HELPER          "repl_glfloat1"
#define REPL_EXPORT_GLFLOAT3_HELPER          "repl_glfloat3"
#define REPL_EXPORT_GLFLOAT4_HELPER          "repl_glfloat4"
#define REPL_EXPORT_GLDOUBLE4_HELPER         "repl_gldouble4"

#endif /* REPL_EXPORT_FORMAT_SHARED_H */
