/*
 * src/repl/export_state.h - Shared dimensions for import/export state text.
 */
#ifndef REPL_EXPORT_STATE_H
#define REPL_EXPORT_STATE_H

/* Every workspace-header directive shares this one line buffer, emitted
 * in order (banner, @scene-name, @workspace-dir, @var, @func, @cfg) by
 * repl_state_refresh_workspace_header_lines(). Size it for the worst case
 * so a var-heavy scene can't crowd the trailing @cfg lines off the end —
 * the old 48-line budget silently truncated them (vertex_outlines among
 * them) once @var + @func filled the buffer first:
 *     1   banner ("// @workspace: ...")
 *     1   @scene-name
 *     1   @workspace-dir
 *    24   @var   (MAX_PREDEF_VARS)
 *    10   @func  (REPL_FUNC_SLOT_COUNT)
 *    32   @cfg   (REPL_CFG_MAX_ITEMS — the cfg bag's own cap)
 *   = 69; rounded up for headroom. A STATIC_ASSERT in src/repl/export.c
 * pins this against the real constants, and emit_cfgs() warns on stderr
 * if it ever still has to drop a @cfg line. */
#define MAX_WORKSPACE_HEADER_LINES 72
#define WORKSPACE_HEADER_LINE_LEN   96
#define CAM_LINE_COUNT 4
#define CAM_LINE_LEN   96    /* per-line buffer width for cam_lines[] */
#define RENDER_STATE_LINE_COUNT 2
#define RENDER_STATE_LINE_LEN   64    /* per-line buffer width for render_state_lines[] */

#endif /* REPL_EXPORT_STATE_H */