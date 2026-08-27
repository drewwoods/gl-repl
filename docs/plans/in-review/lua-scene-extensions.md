# Lua Scene Extensions

## Summary

Add a deliberately bounded Lua extension system for authoring new scene
effects without modifying the REPL itself.

| Option | Assessment |
|---|---|
| Compile-time C registry | Simple and fast, but every extension requires rebuilding the native and web applications. |
| Native libraries plus Wasm side modules | Maximum access, but creates two compilation and distribution workflows and complicates web linking, packaging, and macOS signing. Emscripten supports side modules, but they require a linkable main module and runtime filesystem loading. |
| Embedded Lua | **Recommended:** one source format, synchronous execution, native/web parity, a small stable host surface, and no per-extension app rebuild. |

Vendor and statically link Lua 5.4.9, the final 5.4 bug-fix release, on native
and web. Freezing the completed 5.4 series provides a stable embedding ABI.
References: [Lua version history](https://www.lua.org/versions.html) and
[Emscripten dynamic linking](https://emscripten.org/docs/compiling/Dynamic-Linking.html).

## Public Interfaces and Semantics

### Scene-call syntax

Introduce one generic REPL primitive:

```c
ext::grassy-hills::render(arg0, arg1, ...);
```

- The extension namespace is lowercase alphanumeric plus hyphens and matches
  the Lua filename stem.
- Function and output names are ordinary identifiers.
- Calls accept at most eight numeric expression arguments.
- Calls are forbidden inside active `glBegin` or GLU tessellation blocks.
- One `CMD_EXT_CALL` carries the namespace and function names in its tagged
  payload. Individual extensions never add `CmdType` entries.

Add read-only, next-frame outputs to expressions:

```c
float visibility = ext::occlusion::visible;
```

- Modules declare numeric or boolean outputs in metadata; booleans map to
  `0` and `1`.
- Outputs initialize to zero, persist until changed or the scene unloads, and
  are not saved as scene state.
- A call executes immediately at its source position. Outputs published by
  the final visible sample dirty the flat program and become readable during
  the next frame's flatten.
- Undeclared outputs and calls with invalid arity are compile errors.

### Lua module contract

Each Lua file returns a versioned descriptor containing:

- `api_version` and `namespace`;
- declared calls, their arity ranges, and their Lua functions;
- declared numeric/boolean outputs;
- optional panel metadata and input callback;
- optional `init_gl` and `shutdown_gl` lifecycle callbacks;
- the standalone-C export sections and per-call emitter.

Module loading and export metadata are GL-free. `init_gl` runs once when an
active scene first references the module. `shutdown_gl` runs while a context is
available when the scene drops it. Context destruction implicitly reclaims GL
resources after a window-manager close where no current context remains.

Invoke Lua through `lua_pcall` so ordinary script errors disable the extension
and produce a status diagnostic instead of unwinding through C. Extensions are
trusted and in-process. Library restrictions exist for native/web consistency,
not as a security sandbox; an infinite loop can still freeze the host. See the
[Lua C API](https://www.lua.org/manual/5.4/manual.html).

### Focused graphics API

Expose a portable subset sufficient for the intended extension cases:

- camera position, right/up basis, projection, scene rectangle, animation
  time, and frame/sample indices;
- fixed-function state, matrices, immediate drawing, normals, texture
  coordinates, texture environment, and texture-coordinate generation;
- extension-owned opaque texture handles, including raw-RGBA creation/update,
  PPM asset loading, framebuffer-to-texture copy, binding, and parameters;
- asynchronous boolean occlusion queries: begin/end, availability, and
  non-blocking result retrieval.

Do not expose shaders, framebuffer objects, arbitrary GL procedure loading, or
a broad raw-OpenGL binding in v1.

Calls run for every main accumulation or blur sample. Only the final sample
publishes outputs. Diagnostic wireframe/winding views, depth probes, PLY
capture, and replay-fade redraws suppress extension calls. Explicit extension
`begin` and `end` calls may intentionally affect following REPL geometry. The
host restores pass-level state and forcibly closes unbalanced queries at pass
end.

## Integration Changes

### REPL and frame pipeline

- Extend the neutral executor contract with an extension-dispatch callback and
  immutable call context. The REPL executor remains unaware of Lua and app
  state.
- Extend the render execution context with camera/frame data and accumulation
  sample index/count so billboarding and sample-aware effects require no
  global reads.
- Add neutral extension-spec and output views to compile/flatten evaluation.
  Handle `ext::` syntax in both interactive and bulk-load dispatchers,
  expression dependency tracking, formatting, autocomplete/help, replay
  annotations, state classification, and export/import parity.
- Implement an app-owned extension manager with at most eight active modules.
  It owns Lua states, declared metadata, outputs, resource handles, lifecycle
  synchronization, and error state.

### Workspace persistence and web delivery

Upgrade managed workspaces compatibly:

- Version 2 adds up to eight `extension=<name>.lua` entries under
  `extensions/` and manifest-listed asset leaves under `assets/`.
- Version 1 remains readable. Writers emit version 2 only when sidecars exist.
- Loads validate every listed file before mutation. Save As copies sidecars
  transactionally and publishes the manifest last.
- Extension calls outside a managed workspace fail clearly.
- Web workspace import places the selected manifest, scenes, Lua files, and
  assets into MEMFS before invoking the normal workspace loader. Emscripten's
  virtual filesystem then supports the same synchronous C file access; see
  the [filesystem overview](https://emscripten.org/docs/porting/files/file_systems_overview.html).

### Interactive extension panels

Support one panel per active extension:

- Reserve eight extension slots in the existing overlay layout, ordered above
  document panels and below profiling panels.
- The host owns title chrome, close control, clipping, placement, easing, and
  focus.
- During snapshot preparation, Lua fills a capped retained draw list of text,
  lines, rectangles, and extension texture handles. UI rendering remains
  snapshot-driven rather than calling mutable extension code mid-render.
- Content clicks, drag motion/up, wheel events, and focused keys are delivered
  with panel-local coordinates. App modals, tours, Escape, and global
  shortcuts retain priority; Escape releases extension focus.

### Standalone C export

Every used extension must provide deduplicated C prologue, initialization, and
shutdown sections plus a per-call emitter that accepts C-translated argument
expressions.

- Export fails explicitly if the module or hook is missing or errors.
- Generated blocks carry `@ext-call` markers so importing exported C
  reconstructs the original namespaced line and skips helper code.
- Export hooks must emit self-contained C. External runtime dependencies or
  unembedded assets are rejected.

## Test Plan

- Parse, format, and load both semicolon entry shapes; validate namespaces,
  functions, outputs, arity, nested expressions, loops, functions, and
  forbidden block placement.
- Verify synchronous source-order GL calls, next-frame output visibility,
  structural reflattening, scene-switch reset, and failed-call isolation.
- Verify accumulation invokes calls once per sample but commits outputs once;
  diagnostic, probe, PLY, and replay-fade paths suppress calls.
- Exercise texture lifecycle, framebuffer copying, billboarding context,
  non-blocking occlusion results, unbalanced-query cleanup, and GL-stub builds.
- Verify extension panels do not overlap existing panels, retained drawing
  clips correctly, texture previews render, and mouse/wheel/key capture follows
  routing priority.
- Round-trip version 1 and 2 workspaces; cover missing, duplicate, and
  path-traversal sidecars, transactional rollback, Save As copying, and browser
  MEMFS import.
- Compile and run exported extension scenes against GL stubs; verify missing
  export hooks fail and `@ext-call` markers import back identically.
- Run native tests, `test-stubs`, `test-web`, C99/state-ownership guards, and
  one browser GL integration case for texture and occlusion behavior.

## Assumptions and Deliberate Limits

- No native binary plugins, hot reload, dependency resolver, marketplace,
  implicit frame hooks, same-frame return values, multiple panels per
  extension, or persistence of Lua internals in undo/tour snapshots.
- Maximums are eight active extensions, one panel per extension, eight call
  arguments, and fixed-capacity output/draw-list storage.
- Extensions execute only when referenced by the active scene. Switching
  scenes tears them down and resets their outputs.
