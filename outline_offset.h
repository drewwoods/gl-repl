/*
 * outline_offset.h - Polygon-offset depth-bias constants for the outline pass.
 *
 * Shared by the live app controller and the export path so both rendering modes
 * use the same depth bias when drawing wire outlines over the filled pass.
 * The live pass is in src/app/glr_ctrl.c; src/repl/export.c emits the same
 * constants into exported standalone C so the visual result matches.
 *
 * This stays at the repo root because it is a tiny cross-layer constant header,
 * not a scene-module API.
 */
#ifndef OUTLINE_OFFSET_H
#define OUTLINE_OFFSET_H

#define REPL_OUTLINE_POLYGON_OFFSET_FACTOR (-0.01f)
#define REPL_OUTLINE_POLYGON_OFFSET_UNITS  (-100.0f)

#endif /* OUTLINE_OFFSET_H */
