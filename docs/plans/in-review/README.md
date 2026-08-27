# In-Review Plans

This directory holds drafted plans that are awaiting a design read before
implementation. Move a plan to `../not-started/` once its design is accepted,
or to `../active/` if implementation begins immediately.

- [File-Scope Function Definitions In The Code Panel](file-scope-function-defs.md)
  - draw function defs and global decls above the generated `display()`
    line, projecting the `.glr` phase order (`doc_order.h`) the exporter
    already writes. Project 1 (panel chrome + indent base + dump) is
    fully specified, no open decisions; Project 2 (making the order
    total across commit, import, paste and tutorials) is gated on a
    tutorial decision.
- [Lua Scene Extensions](lua-scene-extensions.md)
  - workspace-scoped Lua scene calls with focused GL access, next-frame
    outputs, interactive panels, and required standalone-C export hooks.
