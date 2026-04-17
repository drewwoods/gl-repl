# Local GL Stubs

This directory provides no-op GL, GLU, and GLUT headers for remote build
machines that do not have OpenGL development packages installed.

Normal builds still prefer system headers when they are present. To force the
stubs and skip GL/GLU/GLUT linker dependencies, build with:

```sh
make test-stubs
```

or add `USE_GL_STUBS=1` to another target:

```sh
make sample USE_GL_STUBS=1
```

The stubs are only for compilation and non-rendering tests. They do not create
a window or draw anything.
