/*
 * tools/repl_demo/stubs.c -- No-op shim translation unit for repl_demo.
 *
 * This file is intentionally empty: the REPL pipeline reaches editor, UI,
 * and controller services through host-effect and export bridges, so no
 * stubs are needed. Keeping the translation unit in the build makes a new
 * stub a visible signal that the pipeline has acquired an upper-layer
 * dependency.
 *
 * The demo installs only edit-line hooks. Status, config, camera, and
 * tutorial requests have no installed target and therefore no-op; this
 * keeps the demo headless and independent of app, editor, UI, and peer
 * modules.
 */
