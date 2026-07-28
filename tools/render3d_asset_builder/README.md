# Render3D asset builder (REPL scenes)

This tool collects REPL-language recreations and experiments for Render3D
elements such as grids and backdrops. Its runtime example catalog is in this
directory, separate from the application's built-in examples. The Ember grid is
the first entry; add future scenes under `scenes/` and register them in
`catalog.ini`.

The far grid expands to more than the normal 8192 flat-command budget.  Build
a one-off binary in its own build directory, then pass `.` to
`--examples-dir`:

```sh
make -C ../.. render-3d-asset-builder
../../render-3d-asset-builder --examples-dir . --example "Ember grid" --no-audio
```

Run those commands from `tools/render3d_asset_builder/`. `make render-3d-asset-builder`
keeps the larger-capacity objects under `build/render3d_asset_builder/`; it does
not change the project-wide default in [`config.h`](../../config.h).

On startup, allow roughly 60 warm-up frames for the application's default XZ
Ruler theme to finish its transition to Off.  Captures taken sooner will show
the fading ruler over the REPL-authored Ember grid.
