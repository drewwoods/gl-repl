# Agent Notes

## Local GL Stub Headers

This sample includes local no-op OpenGL, GLU, and GLUT headers under `include/`
so remote development machines can compile and run non-rendering tests without
installing system GL development packages.

Use the stubs when a remote or CI-like machine is missing headers such as
`GL/gl.h`, `GL/glu.h`, `GL/freeglut.h`, `GL/glut.h`, `GLUT/glut.h`, or
`OpenGL/gl.h`.

Preferred commands:

```sh
make test-stubs
make sample USE_GL_STUBS=1
```

`USE_GL_STUBS=1` changes the include path to prefer this sample's local
`include/` directory and removes `-lGL`, `-lGLU`, and `-lglut` from the link
flags. Stub-mode objects are written to `build/*-gl-stubs` so they do not mix
with normal rendering builds.

## Important Constraints

- The stubs are for compilation and non-rendering tests only. They do not open a
  window, draw pixels, create a real GL context, or exercise real GL behavior.
- Normal builds should continue to use system OpenGL/GLU/GLUT headers and
  libraries when available. Do not make the stubs the default rendering path.
- If new GL, GLU, or GLUT symbols are added to the sample, update the matching
  stub header in `include/GL/`, `include/GLUT/`, or `include/OpenGL/`.
- Keep the stubs minimal and no-op. They should model types, constants, and
  callable signatures well enough for builds and tests, not become a fake
  renderer.
- After changing stub coverage, verify both paths when possible:

```sh
make test-stubs
make sample USE_GL_STUBS=1
make sample
```

## Header Layout

- `include/GL/gl.h`: fixed-function GL typedefs, constants, and no-op calls.
- `include/GL/glu.h`: GLU quadric, projection, and tessellator declarations.
- `include/GL/freeglut.h`: GLUT/freeglut constants, callback types, and no-op
  window/input/font/shape calls.
- `include/GL/glext.h`, `include/GL/glut.h`, `include/GLUT/glut.h`,
  `include/OpenGL/gl.h`, and `include/OpenGL/glu.h`: compatibility wrappers for
  the include styles used across platforms.
