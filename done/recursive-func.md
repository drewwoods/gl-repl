Short answer: function arguments are reasonable to add; safe recursion is a bigger change.

The reason is that the current function system is really macro expansion, not a runtime call system. Functions are only `CMD_FUNC_DEF` / `CMD_CALL` entries in [sample.h](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/sample.h#L73-L106), the parser only recognizes `funcN {` and `funcN()` in [repl_core.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#L3598) and [repl_core.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#L2378), and calls are expanded inline during flattening in [repl_core.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#L2747-L2758). At execution time, `CMD_CALL` is ignored because it should already be flattened away in [repl_core.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#L3124).

What helps is that expression evaluation already has a scoped-variable hook via `ExprCtx` in [repl_eval.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_eval.c#L53-L125). So passing arguments into ordinary GL command expressions is pretty doable. The hard part is control flow: `for(...)` headers are re-parsed without local vars in [repl_eval.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_eval.c#L354-L406), and `if(...)` conditions are committed using only predefined vars in [repl_core.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#L3669-L3773), while identifier detection only knows `x y z i j k n t` in [repl_eval.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_eval.c#L19-L43). That means recursive termination like `if(depth <= 0)` will not work correctly until `if` and `for` become local-scope-aware.

My estimate:
- Args only, for use inside GL command expressions: medium, about 1 focused day.
- Args usable in `if`/`for`, plus recursion with a depth/call-count guard, plus save/export updates: medium-hard, more like 2 to 4 days.

One important distinction: a literal icosahedron does not need recursion at all. That can be done today with fixed vertices/faces, or with parameterized helpers once args exist. Recursive support matters more if what you really want is icosphere-style subdivision from an icosahedron.

If I were implementing it, I would not jump to a full runtime call stack first. I’d keep the current flattening model and do this in phases:
1. Parse `func name(a, b, c) {` and `name(expr, expr, expr)`.
2. Bind call arguments into local `ExprVar`s during `flatten_range`.
3. Make `if` and `for` evaluate against those locals during flattening.
4. Add recursion guards and update export, which currently assumes `static void funcN(void)` in [repl_core.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#L894-L949) and [repl_core.c](/Users/drew/Documents/Work/Code/openGL/samples/gen-ai/OpenGL-Vibe/src/immediate-mode-repl/claude4.6-opus-thinking/repl_core.c#L1666-L1702).

If you want, I can turn that into a concrete patch plan for this sample.

