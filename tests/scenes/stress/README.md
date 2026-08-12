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
| `loop-break-continue-stress.glr` | Loop control flow boundaries | Exercises nested loops up to 3 levels deep with dynamic loop boundaries, dynamic loop steps, conditional `break` statements, and conditional `continue` statements to test control flow correctness. |
| `scratch-array-stress.glr` | Scratch arrays & custom matrix transforms | Tests reading and writing to scratch arrays (`A` & `B`), variable array indexing under loops, and composing custom affine transformations via `glMultMatrixf(A)`. |
| `mutual-recursion-stress.glr` | Mutual recursion & function call stack | Establishes a mutual recursion chain (`func1` -> `func2` -> `func1`) with local variables shadowing, parameter stack push/pop overhead, and depth exit condition checking. |
| `deep-math-expression-stress.glr` | Deep nested math expressions & complex conditions | Evaluates complex combinations of functions (`smoothstep`, `lerp`, `clamp`, `pow`, `abs`, `sqrt`, `atan2`, `rand`) and nested boolean conditions within loop branches. |
| `deep-call-chain-shadowing-stress.glr` | Scope shadowing & call stack unrolling | Executes a 5-level deep function call chain (`func0` -> `func1` -> `func2` -> `func3` -> `func4`) with collision of parameter/local names (`x`, `y`, `z`, `a`, `b`) to test lexical scoping. |
| `scratch-array-lookup-stress.glr` | Scratch block assignments & dynamic indexing | Combines scratch block writes (`A[0] = {...}`) and nested-loop dynamic lookup arithmetic (`C[abs(i+j)%16]`) to build matrices applied via `glMultMatrixf(A)`. |
| `opengl-state-toggle-stress.glr` | OpenGL state transitions inside loops | Rapidly toggles various OpenGL states (`GL_LIGHTING`, `GL_DEPTH_TEST`, `GL_CULL_FACE`, `GL_BLEND`) dynamically within nested loops on every render pass. |
| `recursion-depth-limit-stress.glr` | Deep recursion stack boundary | Executes a recursion chain up to 60 levels deep, testing stack frame allocation and limits close to the compile ceiling of 64. |
| `nested-conditional-chain-stress.glr` | Extensive conditional branching | Exercises over 15 distinct branching paths inside nested conditionals (`if` / `else if` / `else`) to stress compiler decision networks. |
| `multi-clip-plane-stress.glr` | Multiple clip planes | Enables `GL_CLIP_PLANE0..2` simultaneously with animated plane equations under nested matrix frames and two-sided lighting on cut faces. |
| `stencil-depth-mask-stress.glr` | Stencil multipass + masks | Invisible stencil window (`glColorMask`/`glDepthMask` off), geometry where stencil matches, translucent shell where it does not. |
| `dead-code-if0-stress.glr` | `if(0)` / `if(1)` elimination | Constant-false bodies (including nested calls and loops) must emit nothing from flatten; constant-true and dynamic branches still run. |
| `inline-multmatrix-expr-stress.glr` | Inline `glMultMatrixf` expressions | Sixteen expression cells (not a scratch array): shear, planar-shadow projection, and spin matrices stacked under push/pop. |
| `func-scope-capacity-stress.glr` | Function scope capacity | One helper near `MAX_EXPR_VARS` (12 params + 18 locals + 1 loop iterator); appearance args live in a second helper. |
| `named-func-alias-stress.glr` | Named function aliases | `NAME(params)` aliases with recursive branching, coexistence with a classic `func0` slot, and shared local names across aliases. |
| `label-raster-pos-stress.glr` | `label()` + raster pos | Multi-arg `%f` / `%%` formats, labels after nested transforms, and labels emitted from function bodies and loops. |
| `polygon-mode-offset-stress.glr` | Polygon mode + offset | `GL_FILL` / `GL_LINE` / `GL_POINT` cycling, per-face modes, and `glPolygonOffset` separating coplanar wire overlays. |
| `loop-iterator-shadow-stress.glr` | Iterator scope shadowing | Loop binders reuse local and global names; after the loop the outer binding must still hold its pre-loop value. |
| `dynamic-for-bounds-stress.glr` | Dynamic for bounds | Expression-driven start/end/step that depend on `t` and outer counters, with nested `continue` / `break`. |
| `fog-mode-switch-stress.glr` | Fog mode transitions | Mid-frame switches among `GL_LINEAR` / `GL_EXP` / `GL_EXP2`, density and color rewrites, and a fog-disabled sharp marker. |

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
./gl-repl tests/scenes/stress/loop-break-continue-stress.glr
./gl-repl tests/scenes/stress/scratch-array-stress.glr
./gl-repl tests/scenes/stress/mutual-recursion-stress.glr
./gl-repl tests/scenes/stress/deep-math-expression-stress.glr
./gl-repl tests/scenes/stress/deep-call-chain-shadowing-stress.glr
./gl-repl tests/scenes/stress/scratch-array-lookup-stress.glr
./gl-repl tests/scenes/stress/opengl-state-toggle-stress.glr
./gl-repl tests/scenes/stress/recursion-depth-limit-stress.glr
./gl-repl tests/scenes/stress/nested-conditional-chain-stress.glr
./gl-repl tests/scenes/stress/multi-clip-plane-stress.glr
./gl-repl tests/scenes/stress/stencil-depth-mask-stress.glr
./gl-repl tests/scenes/stress/dead-code-if0-stress.glr
./gl-repl tests/scenes/stress/inline-multmatrix-expr-stress.glr
./gl-repl tests/scenes/stress/func-scope-capacity-stress.glr
./gl-repl tests/scenes/stress/named-func-alias-stress.glr
./gl-repl tests/scenes/stress/label-raster-pos-stress.glr
./gl-repl tests/scenes/stress/polygon-mode-offset-stress.glr
./gl-repl tests/scenes/stress/loop-iterator-shadow-stress.glr
./gl-repl tests/scenes/stress/dynamic-for-bounds-stress.glr
./gl-repl tests/scenes/stress/fog-mode-switch-stress.glr
```
