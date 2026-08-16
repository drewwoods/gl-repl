# In-Review Plans

This directory holds drafted plans that are awaiting a design read before
implementation. Move a plan to `../not-started/` once its design is accepted,
or to `../active/` if implementation begins immediately.

| Plan | Topic |
|---|---|
| [call-frame-provenance.md](call-frame-provenance.md) | Identity for dynamic funcN invocations, so a replay PATH annotation can show how execution reached the emitted vertex - the flat record keeps only the first and last rung of the call ladder today |
| [console-command.md](console-command.md) | `console(...)`, a `label()`-shaped primitive that writes to a panel instead of the framebuffer, auto-indented by call depth |
