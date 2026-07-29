# Abandoned Plans

This directory holds plans that were designed but **deliberately dropped**: no
implementation landed, and none is intended. They are kept because the design
work — especially the alternatives that were weighed and rejected — is the
expensive part, and a future reader asking "was this considered?" deserves the
answer plus the reasoning.

Distinct from the sibling buckets:

- `not-started/` — designed, not built, **still wanted**.
- `partial/` — some phases landed, the rest deferred on purpose.
- `not-landed/` — implemented on a branch, not merged to `main`.

Each resident carries a status header stating when and why it was abandoned. A
plan moves back to `not-started/` if it is ever revived.

| Plan | Abandoned | Why |
|---|---|---|
| `winding-texture-mode.md` | 2026-07-29 | Winding view stays the shipped 2-state Off/Color toggle; the FRONT/BACK textured mode isn't wanted. Kept for the two-pass-cull vs. single-pass multitexture-combiner analysis (and the working combiner reference implementation), which is reusable if a textured face-orientation mode is ever revisited. |
