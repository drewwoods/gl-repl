/*
 * repl_export.h - Save/load of REPL sessions to/from C source files.
 *
 * Bidirectional text format for persisting complete REPL state (commands,
 * variables, camera, settings, workspace directory) as standalone C files.
 * Files round-trip cleanly: saving and re-loading preserves all state.
 *
 * Export format (save_output): Writes a complete C file with:
 *   1. Header comments with workspace metadata (@var name=value, @cfg setting=value,
 *      @scene-name <name>, @workspace-dir <path>). Used by import to restore context.
 *   2. Global variable declarations for user-defined predefined variables (float x, y, z).
 *   3. Camera state as the raw glTranslatef/glRotatef sequence the REPL uses internally
 *      (not a pose matrix — the exact command history).
 *   4. REPL function definitions converted to C function syntax (for reloading as
 *      CMD_FUNC_DEF on import).
 *   5. Geometry commands in the display() function body (user-edited commands).
 *
 * Import format (load_from_file): Line-by-line scan that:
 *   1. Parses leading workspace header directives (@var, @cfg, @scene-name, @workspace-dir).
 *   2. Extracts camera state (raw glTranslatef/glRotatef lines).
 *   3. Detects function definitions (lines matching C function syntax).
 *   4. Feeds remaining geometry lines through feed_line() for normal parsing.
 *   5. Stores pending scene-name and workspace-dir for the caller to apply after loading.
 *
 * Header templates: g_header_pre/post and g_footer_pre/post_init are boilerplate
 * segments (includes, setup, display() signature, cleanup). Inserted around the
 * exported code to create a valid C program.
 *
 * Workspace integration: repl_state_refresh_workspace_header_lines() rebuilds the
 * export header text from current state. repl_state_parse_workspace_header_line()
 * consumes a single header directive during import and stores pending scene/workspace
 * metadata for the caller to apply after load_from_file() returns.
 *
 * Multi-scene export lives in repl_scenes.c: workspace saves iterate user-scene
 * slots, set the export scene-name hint, and call repl_export_save_output() for
 * each scene.
 */
#ifndef REPL_EXPORT_H
#define REPL_EXPORT_H

/* Boilerplate C file segments for export. g_header_pre is the initial includes
 * and setup; g_header_post follows the metadata comments; g_footer_pre_init is
 * before the display() function; g_footer_post_init follows the function body.
 * Together they bracket the exported code to create a valid C program. */
extern const char  *g_header_pre[];
extern const char  *g_header_post[];
extern const char  *g_footer_pre_init[];
extern const char  *g_footer_post_init[];

/* Export current REPL state to a C source file. Writes header metadata (@var, @cfg,
 * @scene-name, @workspace-dir), global variable declarations, camera state, function
 * definitions, and geometry commands to filename. The file is a complete, standalone
 * C program that can be reloaded via load_from_file(). Called by save-to-output and
 * workspace export routines. */
void repl_export_save_output(const char *filename);

/* Import a C source file saved by save_output(). Parses workspace header directives,
 * camera state, function definitions, and geometry commands. Feeds geometry lines
 * through feed_line() for normal parsing. Returns 1 on success, 0 on error (parse
 * failure, open failure). Pending scene-name and workspace-dir directives remain
 * in import/export state for the caller to consume after return. */
int  repl_export_load_from_file(const char *filename);

/* Refresh the export header text from current state. Pre-builds the header metadata
 * lines (@var, @cfg, @scene-name, @workspace-dir) from the current predefined
 * variables, render settings, and workspace directory. Called after mutations
 * (variable declare, config toggle, workspace directory change) to keep the export
 * buffer in sync. */
void repl_state_refresh_workspace_header_lines(void);

/* Parse a single workspace header directive line (e.g., "@var x=5" or "@cfg wireframe=1").
 * Updates internal state accordingly. Returns 1 if the line was a recognized directive,
 * 0 if it's not a directive (caller should process as regular code). Used during
 * load_from_file() to extract metadata. */
int  repl_state_parse_workspace_header_line(const char *line);

#endif
