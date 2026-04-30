# Local GL Stubs

This directory provides no-op GL, GLU, and GLUT headers for builds that should
compile without system OpenGL development packages.

Use:

```sh
make test-stubs
make sample USE_GL_STUBS=1
```

The stubs are compilation and non-rendering test support only. They do not
create a window, draw pixels, or provide a real GL context. If the sample starts
calling a new GL/GLU/GLUT symbol, extend the matching stub header under
`tests/gl-stubs/include/`.

Header layout:

- `tests/gl-stubs/include/GL/gl.h` - fixed-function GL types, constants, and no-op calls.
- `tests/gl-stubs/include/GL/glu.h` - GLU projection, quadric, and tessellator calls.
- `tests/gl-stubs/include/GL/freeglut.h` - GLUT/freeglut callbacks and shapes.
- `tests/gl-stubs/include/GLUT/` and `tests/gl-stubs/include/OpenGL/` - compatibility wrappers.
