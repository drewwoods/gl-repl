# In-Review Plans

This directory holds drafted plans that are awaiting a design read before
implementation. Move a plan to `../not-started/` once its design is accepted,
or to `../active/` if implementation begins immediately.

| Plan | Topic |
|---|---|
| [`vertex-label-depth-readback-stall.md`](vertex-label-depth-readback-stall.md) | Move the vertex-label occlusion depth read to the frame's already-drained end-of-frame `glFinish` and consume it one frame later, removing a ~14 ms/frame mid-frame pipeline stall on render-ahead drivers. Drafts the PBO alternative and records why it measured no benefit. |
