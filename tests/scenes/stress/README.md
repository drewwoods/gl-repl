# GLR Stress Test Suite & Catalogue

This directory contains a suite of targeted `.glr` stress test files designed to exercise parser, compiler, evaluator, flattener, and OpenGL execution corner cases in `gl-repl`.

It is a **runtime `--examples-dir` catalog, not a built-in one**: nothing here is compiled into the binary, and none of it appears in the Scene menu. That is why it lives under `tests/` rather than in [`examples/`](../../../examples/README.md), and it is also what lets `catalog.ini` tag entries by the corner case each probes (`Scoping`, `AttribStack`, `Evaluator`) - the compiled-in catalog accepts only `2D` / `3D` / `Polygons` / `Lines`.

## Test Catalogue

| File | Corner Case Tested | Description |
|---|---|---|
| `color-mask-clear-stress.glr` | `glColorMask` + `glClear` | Disables specific color channels (`GL_FALSE` / `GL_TRUE` masks), issues `glClear(GL_COLOR_BUFFER_BIT)`, verifies channel filtering during frame clears, selective geometry color writes, and color mask state restoration. |
| `nested-if-branching-stress.glr` | Deeply nested `if` conditionals | Exercises 5-level deep nested `if` / `else if` / `else` conditional blocks within unrolled `for` loops, evaluating dynamic time-varying (`sin(t)`, `cos(t)`) logic and cleanly skipping empty/non-taken geometry blocks. |
| `function-local-shadowing-stress.glr` | Local variable name collisions & scoping | Declares `float var;` local variables with identical names (`x`, `y`, `i`) across caller, callee, and sibling functions and top-level `static float` globals, validating lexical scope isolation and non-clobbering semantics. |
| `function-order-dependency-stress.glr` | Function call & definition order | Invokes functions before/after their definition, testing nested calls (`func2` -> `func1` -> `func0`), matrix transformation accumulation, and call-order dependency in the flattener. |
| `attrib-stack-push-pop-stress.glr` | `glPushAttrib` / `glPopAttrib` nesting | Pushes and pops multiple attribute bitmasks (`GL_ENABLE_BIT`, `GL_LIGHTING_BIT`, `GL_COLOR_BUFFER_BIT`, `GL_ALL_ATTRIB_BITS`), verifying state preservation, depth test toggling, and proper restoration upon popping stacked frames. |
| `matrix-stack-recursion-stress.glr` | Matrix stack & recursive function calls | Implements a 3D fractal tree using recursive function calls with nested `glPushMatrix()` / `glPopMatrix()` calls, unrolling up to 81 branch nodes and verifying matrix stack balance. |
| `expression-eval-boundary-stress.glr` | Math evaluator edge cases | Exercises floating-point modulo (`%`), inverse trig (`atan2`), `sqrt(abs(...))`, `pow`, `clamp`, `lerp`, high-frequency trig signals, and complex boolean expressions (`&&`, `||`). |

## Catalog File

Machine-readable entries for all test cases are recorded in `catalog.ini`.

## How to Run & Validate

### 1. Load as an Example Catalog
Replace the built-in example catalog at runtime with the stress directory:
```bash
./gl-repl --examples-dir tests/scenes/stress
./gl-repl --examples-dir tests/scenes/stress --list-examples
```

### 2. Load Individual Scene Files
Launch `gl-repl` directly on any stress scene:
```bash
./gl-repl tests/scenes/stress/color-mask-clear-stress.glr
./gl-repl tests/scenes/stress/nested-if-branching-stress.glr
./gl-repl tests/scenes/stress/function-local-shadowing-stress.glr
./gl-repl tests/scenes/stress/function-order-dependency-stress.glr
./gl-repl tests/scenes/stress/attrib-stack-push-pop-stress.glr
./gl-repl tests/scenes/stress/matrix-stack-recursion-stress.glr
./gl-repl tests/scenes/stress/expression-eval-boundary-stress.glr
```
