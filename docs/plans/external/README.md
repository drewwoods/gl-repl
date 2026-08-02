# External Plans

This directory holds plans for work that lives **outside this repository**
(e.g. upstream forks, third-party patches). They are kept here as the
design specs that motivated the external work, so the gl-repl context
that drove them is preserved alongside the in-tree plans.

| Plan | Where the work lives | What it covers |
|---|---|---|
| `freeglut-osmesa-backend.md` | [drewwoods/freeglut](https://github.com/drewwoods/freeglut) @ `osmesa-backend` (implemented) | OSMesa off-screen backend for headless GL - needed for `--export-ply` feedback capture without a display. The same branch also carries the `SIGUSR1` screenshot + `FREEGLUT_CAPTURE_FRAMES` record-mode capture extras. Vendored into this tree (see `third_party/freeglut/VENDORED.txt`). |
