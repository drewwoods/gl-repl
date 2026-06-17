#ifndef GLR_MESH_EXPORT_H
#define GLR_MESH_EXPORT_H

/* Capture the current scene's geometry via one glRenderMode(GL_FEEDBACK)
 * pass and write it to `path` as an ASCII PLY mesh (positions, per-vertex
 * color, synthesized normals, triangular faces).
 *
 * Runs the existing flat-program executor with GL in feedback mode under a
 * fixed identity-modelview + ortho + viewport transform, so user glVertex
 * geometry, GLU-tessellated polygons, and the GLUT solids (teapot/sphere/
 * cube/cone/torus) are all captured through one path. The pure parsing /
 * writing half lives in src/support/mesh_ply.c.
 *
 * Must be called on the GLUT thread with a live GL context (feedback needs a
 * context). Returns the triangle count (>= 0) on success, or < 0 on error
 * (empty scene, feedback buffer cap exceeded, file open / write failure).
 * Sets the REPL status message in every case.
 *
 * When `srgb_decode` is non-zero, vertex colors are decoded from sRGB to
 * linear light before being written (see MeshPlyOptions.srgb_decode) — for
 * color-managed viewers that would otherwise render them washed out. See
 * plans/.../ply-feedback-export.md. */
int glr_export_mesh_ply(const char *path, int srgb_decode);

/* Compute the world-space bounding sphere of the current scene's geometry
 * via the same GL_FEEDBACK capture path as glr_export_mesh_ply (identity
 * modelview, so the box is post-user-transform but pre-camera). Writes the
 * box center to out_center[3] and the half-diagonal radius to *out_radius.
 *
 * Used by the fit-frame feature to choose a camera distance that fills the
 * scene viewport with all geometry visible. Like glr_export_mesh_ply this
 * needs a live GL context on the GLUT thread (and is cheap: one feedback
 * pass, no file I/O). Returns 1 on success, 0 when there is nothing to
 * frame (empty scene, capture failure, or vertex-free stream) — in which
 * case the outputs are left untouched and the caller should not move the
 * camera. */
int glr_capture_world_bbox(float out_center[3], float *out_radius);

#endif /* GLR_MESH_EXPORT_H */
