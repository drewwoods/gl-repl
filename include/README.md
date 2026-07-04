# Project Include Helpers

This directory is for header-only project helpers and vendored single-header
dependencies that are used by normal source builds.

Current contents:

- [`gl_includes.h`](gl_includes.h) - project-wide GL/GLU/GLUT include shim.
- [`gl_2d.h`](../src/ui/core/gl_2d.h) - header-only 2D OpenGL helper functions.
- [`miniaudio.h`](miniaudio.h) - vendored miniaudio dependency.

freeglut itself is vendored separately under `third_party/freeglut/` (full
source, built as a static library with the macOS Cocoa backend); see
`scripts/vendor-freeglut.sh` and `docs/THIRD_PARTY_LICENSES.md`.

No-op GL/GLU/GLUT stub headers live under `tests/gl-stubs/include/` and are
only added to the compiler include path when `USE_GL_STUBS=1`.
