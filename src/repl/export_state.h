/*
 * src/repl/export_state.h - Shared dimensions for import/export state text.
 */
#ifndef REPL_EXPORT_STATE_H
#define REPL_EXPORT_STATE_H

/* Every workspace-header directive shares this one line buffer, emitted
 * in order (banner, @scene-name, @workspace-dir, @func, @cfg) by
 * repl_state_refresh_workspace_header_lines(). Size it for the worst case
 * so a scene can't crowd the trailing @cfg lines off the end -
 * the old 48-line budget silently truncated them (vertex_outlines among
 * them) once @func filled the buffer first:
 *     1   banner ("@workspace: ..." block-comment directive)
 *     1   @scene-name
 *     1   @workspace-dir
 *    10   @func  (REPL_FUNC_SLOT_COUNT)
 *    48   @cfg   (REPL_CFG_MAX_ITEMS - the cfg bag's own cap)
 *   = 61; rounded up for headroom. A STATIC_ASSERT in src/repl/export.c
 * pins this against the real constants, and emit_cfgs() warns on stderr
 * if it ever still has to drop a @cfg line. */
#define MAX_WORKSPACE_HEADER_LINES 80
#define WORKSPACE_HEADER_LINE_LEN   96
/* Camera transform block: one set of names dimensions both the
 * cam_lines[] preview storage (state_views.h) and the
 * ReplExportCameraBlock interchange struct (export.h). Defined here -
 * the bottom of the include chain - because state_views.h needs them
 * to declare the state struct and cannot include export.h back. */
/* dist, rx, ry, spin, pan - the canonical emit order. The `spin` slot
 * carries exported C's g_angle animation hook and is empty in the
 * hook-less projection the .glr writer and code panel use, so every
 * consumer skips empty slots rather than assuming five rows. */
#define REPL_EXPORT_CAMERA_LINES    5
#define REPL_EXPORT_CAMERA_LINE_MAX 96
#define RENDER_STATE_LINE_COUNT 2
#define RENDER_STATE_LINE_LEN   64    /* per-line buffer width for render_state_lines[] */

#endif /* REPL_EXPORT_STATE_H */
