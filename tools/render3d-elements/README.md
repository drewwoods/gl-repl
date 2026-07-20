# Render3D elements (REPL scenes)

This tool collects REPL-language recreations and experiments for Render3D
elements such as grids and backdrops. Its runtime example catalog is under
`src/`, separate from the application's built-in examples. The Ember grid is
the first entry; add future scenes under `src/scenes/` and register them in
`src/catalog.ini`.

The far grid expands to more than the normal 8192 flat-command budget.  Build
a one-off binary in its own build directory, then pass `src` to
`--examples-dir`:

```sh
make -C ../.. gl-repl BUILD=render3d-elements CFLAGS=-DMAX_FLAT_COMMANDS=32768
../../gl-repl --examples-dir src --example "Ember grid" --no-audio
```

Run those commands from `tools/render3d-elements/`. `BUILD=render3d-elements`
keeps the larger-capacity objects under `build/render3d-elements/`; it does
not change the project-wide default in `config.h`.

On startup, allow roughly 60 warm-up frames for the application's default XZ
Ruler theme to finish its transition to Off.  Captures taken sooner will show
the fading ruler over the REPL-authored Ember grid.
