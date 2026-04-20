# Agent Notes

This file captures durable repo guidance that came out of recent work on
example metadata, presentation defaults, and camera behavior.

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

## Example Metadata Invariants

- Built-in examples in `repl_examples.c` can start with contiguous leading
  `// @cfg slug = value` lines, followed optionally by a 5-line `// camera`
  preset block.
- `repl_core.c` consumes that leading metadata before feeding the remaining
  example lines through the normal commit pipeline, so metadata stays hidden
  from the code panel.
- Example `@cfg` parsing reuses `parse_workspace_header_line()` from
  `repl_export.c`, but only for scene-presentation slugs currently allowed by
  the example loader:
  `wireframe`, `grid`, `grid_major`, `grid_extent`, `axes`,
  `vertex_labels`, `normal_vectors`, `vertex_outlines`, `vertex_points`,
  `vertex_guides`, `light_indicators`, `backdrop`, `camera_rotate`.
- Non-leading `@cfg` lines are not treated as metadata; they remain ordinary
  comments.

## Reset And Restore Rules

- Every example load resets the allowed non-camera scene-presentation settings
  to their built-in defaults before applying leading example `@cfg` metadata.
  This prevents stale grid/axes/overlay/backdrop state from leaking between
  examples.
- Camera is intentionally excluded from that reset. Examples inherit the
  current `g_cam_*` state unless they provide the explicit leading `// camera`
  header.
- `restore_user_scene()` still restores commands and predefined variables only.
  Leaving an example does not restore camera or other presentation state.

## Function Declaration Invariants

- The REPL command list has a leading declaration area: `float` declarations
  first, then complete user `funcN` definition blocks, then ordinary scene
  commands. This mirrors the exporter, which emits user functions before
  `render_repl_geometry()`.
- When a function is typed after scene commands, `repl_editor.c` promotes the
  function block into that declaration area and resumes editing after the
  commands that were shifted behind it. Keep nearby top-level comments with the
  promoted function when possible.
- Imported `// @declare` markers must still recreate `CMD_VAR_DECLARE` entries
  at the front of the command list, even though exported files encounter those
  markers inside the snippet after static function definitions.
- Focused coverage for this behavior lives in `test_repl_core_commit.c`,
  `test_repl_core_io.c`, and the example 03 exact round-trip check in
  `test_repl_core_examples.c`.

## Shared Defaults

- Keep the single source of truth for example-owned presentation defaults in
  the `CFG_DEFAULT_*` macro block in `sample.h`.
- `repl_core.c` initializers, example reset helpers, and focused example tests
  should reuse those macros instead of duplicating literals.

## Touch Points And Validation

- When changing example metadata behavior, inspect `repl_core.c`,
  `repl_export.c`, `repl_examples.c`, `sample.h`, and
  `test_repl_core_examples.c` together.
- Use `make test_repl_core_examples` as the focused regression suite for this
  area. Run `make test` if the change touches shared defaults or broader REPL
  state.
