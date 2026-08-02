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
 * linear light before being written (see MeshPlyOptions.srgb_decode) - for
 * color-managed viewers that would otherwise render them washed out. */
int glr_export_mesh_ply(const char *path, int srgb_decode);

#endif /* GLR_MESH_EXPORT_H */
