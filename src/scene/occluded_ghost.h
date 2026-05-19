/*
 * occluded_ghost.h - Shared styling for the "geometry can't fully hide it"
 * ghost pass used by scene overlays.
 *
 * Several scene helpers draw a two-pass overlay: one depth-tested solid
 * pass, plus a depth-disabled "ghost" pass at reduced alpha with a line
 * stipple so the overlay still reads when it sits inside user geometry.
 * The overlays only look consistent if every helper uses the same dashed
 * stipple pattern (and the ghost passes the same alpha fraction). These
 * constants are that convention; they are values, not a mechanism — each
 * helper still owns how it applies them (transform_guides routes the
 * alpha through its private g_guide_alpha_mul; render.c multiplies it
 * inline; the plain dashed-line consumers set their own line factor).
 *
 * STIPPLE consumers: render.c (orbit-target gizmo),
 * guides/transform_guides.c (transform guides), axes.c (negative axes),
 * lights.c (directional-light ray + indicator cross),
 * guides/geometry_guides.c. ALPHA is used only by the true two-pass
 * ghost helpers (render.c, transform_guides.c). Scene-internal, so it
 * lives under src/scene/ rather than at the repo root.
 */
#ifndef SCENE_OCCLUDED_GHOST_H
#define SCENE_OCCLUDED_GHOST_H

/* Shared glLineStipple pattern for scene-overlay dashed lines and the
 * depth-disabled ghost pass (caller picks the line factor). */
#define SCENE_OCCLUDED_GHOST_STIPPLE 0x0F0F

/* Alpha multiplier applied to the ghost pass relative to the solid pass. */
#define SCENE_OCCLUDED_GHOST_ALPHA   0.4f

#endif /* SCENE_OCCLUDED_GHOST_H */
