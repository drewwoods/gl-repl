/*
 * src/repl/examples.h - Built-in example library.
 *
 * Registry of predefined example programs (geometry demos, shader examples,
 * animation techniques, etc.). Examples are compiled into the binary as constant
 * string arrays and can be loaded by the user via F12 cycling or the Example
 * dropdown menu.
 *
 * Example structure: Each example is an array of REPL source lines (commands,
 * declarations, for/func/if blocks, etc.) plus a display name. Lines are
 * formatted and ready to feed through the commit pipeline without modification.
 * Examples may include leading metadata:
 *   - @cfg directives to customize scene presentation (grid theme, axes,
 *     overlays, backdrop, etc.). Metadata is stripped before the code-panel
 *     renders the example.
 *   - An optional // camera block specifying initial camera position/rotation.
 *
 * Example lifecycle: F12 cycles through examples and user scenes. Loading an
 * example resets non-camera scene-presentation settings to defaults, applies
 * the example's @cfg metadata, and feeds the example lines through the commit
 * pipeline. Camera is inherited (not reset) unless the example supplies an
 * explicit // camera block. Built-in defaults (CFG_DEFAULT_* macros in sample.h)
 * define the reset baseline, ensuring consistent starting state across example
 * transitions.
 *
 * User scene system integration: When a user edits an example, the first mutation
 * triggers repl_promote_example_if_needed(), which allocates a fresh user scene,
 * copies the current state into it (inheriting the example's name with
 * de-duplication), and marks that slot as active. Subsequent mutations accumulate
 * into the user scene; the example remains unchanged. Switching away from the
 * user scene via F12 does not restore the example state — user edits are
 * independent once promoted.
 *
 * Query API: repl_examples_count() returns the total number of examples;
 * repl_examples_name() retrieves an example's display name; repl_examples_lines()
 * retrieves the source line array for loading. Used by the UI to populate the
 * Example dropdown and by src/repl/core.c to load and feed examples.
 */
#ifndef REPL_EXAMPLES_H
#define REPL_EXAMPLES_H

/* Query the source line array for an example. Returns a null-terminated array
 * of REPL command strings (ready to feed through the commit pipeline). Index
 * must be in range [0, repl_examples_count()). The returned pointer is valid
 * for the lifetime of the program. Used by src/repl/core.c to load examples. */
const char *const *repl_examples_lines(int idx);

/* Query the display name of an example (shown in the Example dropdown and menu).
 * Returns a pointer to a constant string. Index must be in range [0,
 * repl_examples_count()). Used by the Example dropdown UI and by F12 cycling. */
const char *repl_examples_name(int idx);

/* Query the total number of built-in examples. Used by the Example dropdown and
 * F12 cycling to determine the range of valid example indices. */
int repl_examples_count(void);

#endif /* REPL_EXAMPLES_H */
