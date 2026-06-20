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
 * explicit // camera block. Built-in defaults (CFG_DEFAULT_* macros in gl_repl.h)
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
 * Query API: repl_example_count() returns the total number of examples;
 * repl_example_name() retrieves an example's display name; repl_example_lines()
 * retrieves the source line array for loading. Used by the UI to populate the
 * Example dropdown and by src/repl/example_loader.c to load examples.
 */
#ifndef REPL_EXAMPLES_H
#define REPL_EXAMPLES_H

#include <limits.h>

#include "repl/catalog_tags.h"  /* repl_catalog_tag_bit_for_count */

/* Query the source line array for an example. Returns a null-terminated array
 * of REPL command strings (ready to feed through the commit pipeline). Index
 * must be in range [0, repl_example_count()). The returned pointer is valid
 * for the lifetime of the program. Used by src/repl/example_loader.c to load examples. */
const char *const *repl_example_lines(int idx);

/* Query the display name of an example (shown in the Example dropdown and menu).
 * Returns a pointer to a constant string. Index must be in range [0,
 * repl_example_count()). Used by the Example dropdown UI and by F12 cycling. */
const char *repl_example_name(int idx);

/* Query the total number of built-in examples. Used by the Example dropdown and
 * F12 cycling to determine the range of valid example indices. */
int repl_example_count(void);

/* Curated metadata tags used by the Scene menu. Examples keep their flat
 * identity; tags are only a secondary discovery index. Tag index 0 is the
 * synthetic "All" group — every example is a member (folded into the mask
 * by repl_example_tag_mask), so the Scene menu's first example group lists
 * every example once, matching the flat order the F12 cycle walks.
 *
 * The enum is in the header so app-layer code (the example-presentation
 * reset bridge in src/app/glr_ctrl.c) can name tags symbolically when
 * deciding which tag-default cfg overrides to apply. The bit-shifted
 * EXAMPLE_TAG_* macros used inside g_example_entries[] stay private to
 * examples.c. */
enum {
    REPL_EXAMPLE_TAG_ALL = 0,
    REPL_EXAMPLE_TAG_2D,
    REPL_EXAMPLE_TAG_3D,
    REPL_EXAMPLE_TAG_POLYGONS,
    REPL_EXAMPLE_TAG_LINES,
    REPL_EXAMPLE_TAG_COUNT
};

int repl_example_tag_count(void);
const char *repl_example_tag_label(int tag_idx);
unsigned int repl_example_tag_mask(int example_idx);
int repl_example_has_tag(int example_idx, int tag_idx);
int repl_example_count_for_tag(int tag_idx);
int repl_example_index_for_tag(int tag_idx, int ordinal);
int repl_example_visible_tag_count(void);
int repl_example_visible_tag_at(int dense_idx);

/* Subheading API — mirrors repl_tutorial_subheading. Returns the example's
 * free-form section label or NULL (no subheading / out-of-range idx). The
 * Scene menu uses this to group entries within a tag flyout: consecutive
 * examples sharing a non-NULL subheading render under one `### subheading`
 * chrome row. Catalog authors must keep same-subheading entries contiguous
 * per tag (enforced by test_example_subheading_metadata). */
const char *repl_example_subheading(int example_idx);

static inline unsigned int repl_example_tag_bit(int tag_idx) {
    return repl_catalog_tag_bit_for_count(tag_idx, repl_example_tag_count());
}

#endif /* REPL_EXAMPLES_H */
