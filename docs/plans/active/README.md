# Active Plans

This directory holds plans that are **actively in flight**. Some implementation commits or staged changes have landed, and work is currently progressing.

| Plan | Topic |
|---|---|
| [gl4es-line-width-emulation.md](gl4es-line-width-emulation.md) | Web `glLineWidth` via screen-space quads, preceded by `GL_POLYGON_OFFSET_LINE` |
| [call-frame-provenance.md](call-frame-provenance.md) | Identity for dynamic funcN invocations, so a replay PATH annotation can show how execution reached the emitted vertex |
| [BYOE.md](BYOE.md) | Bring your own editor: `--watch` external-editor sync, in three stages |
| `workspace-management.md` | Multiple named workspaces, scene delete, and fixing single-file exports in the packaged app |
| `camera-header-tags.md` | Role-tagged `// camera` lines and one shared reader, ending the file-load vs catalog-load divergence |
| [scope-and-overlay-maintainability.md](scope-and-overlay-maintainability.md) | From the 2026-10-22 GLU overlay fixes: one cursor-block query in `source_scope`, the shared GLU depth helper everywhere (with a guard), vertex numbering computed once at flatten, one outline walk drawn in two phases, doc links that stop churning on line numbers, build traps, invariant-sweep test policy, a written overlay-scope spec per primitive kind, overlay setup out of `glr_ctrl.c`; plus workspace manifest forward compatibility. Items 1, 2, 4, 5, 6, 8 and 10 are done; 3, 7, 9 and 11 remain |
