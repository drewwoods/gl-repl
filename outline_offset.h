/*
 * outline_offset.h - polygon-offset depth-bias constants for the REPL's
 * outline-pass rendering trick.
 *
 * Shared by:
 *   - imrepl_ctrl.c — live rendering: re-runs user geometry in
 *     glPolygonMode(GL_LINE) with these offsets to draw outlines on top
 *     of the filled pass without z-fighting.
 *   - src/repl/export.c — emits the same constants into the exported C file
 *     so the standalone binary draws outlines identically.
 *
 * Not under src/scene/ because no scene module consumes them — the
 * outline pass moved to the controller in the J-series guides hoist.
 */
#ifndef OUTLINE_OFFSET_H
#define OUTLINE_OFFSET_H

#define REPL_OUTLINE_POLYGON_OFFSET_FACTOR (-0.01f)
#define REPL_OUTLINE_POLYGON_OFFSET_UNITS  (-100.0f)

#endif /* OUTLINE_OFFSET_H */
