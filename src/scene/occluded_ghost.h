/*
 * occluded_ghost.h - Shared styling for the "geometry can't fully hide it"
 * ghost pass used by scene overlays.
 *
 * Several scene helpers draw a two-pass overlay: one depth-tested solid
 * pass, plus a depth-disabled "ghost" pass at reduced alpha with a line
 * stipple so the overlay still reads when it sits inside user geometry.
 * The ghost only looks consistent if every helper uses the same stipple
 * pattern and the same alpha fraction. These two constants are that
 * convention; they are values, not a mechanism — each helper still owns
 * how it applies them (transform_guides routes the alpha through its
 * private g_guide_alpha_mul; render.c multiplies it inline).
 *
 * Consumers: src/scene/render.c (orbit-target gizmo),
 * src/scene/guides/transform_guides.c (transform guides). Scene-internal,
 * so it lives under src/scene/ rather than at the repo root.
 */
#ifndef SCENE_OCCLUDED_GHOST_H
#define SCENE_OCCLUDED_GHOST_H

/* glLineStipple pattern for the depth-disabled ghost pass. */
#define SCENE_OCCLUDED_GHOST_STIPPLE 0x0F0F

/* Alpha multiplier applied to the ghost pass relative to the solid pass. */
#define SCENE_OCCLUDED_GHOST_ALPHA   0.4f

#endif /* SCENE_OCCLUDED_GHOST_H */
