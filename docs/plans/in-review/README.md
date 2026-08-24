# In-Review Plans

This directory holds drafted plans that are awaiting a design read before
implementation. Move a plan to `../not-started/` once its design is accepted,
or to `../active/` if implementation begins immediately.

- [File-Scope Function Definitions In The Code Panel](file-scope-function-defs.md)
  - draw function defs (and global decls) above the generated
    `display()` line; export and flatten already treat them as
    file-scope, so the work is the panel projection plus the base
    indent.
