/*
 * src/repl/examples.h - Built-in example library.
 *
 * Registry of predefined example programs (geometry demos, animation
 * techniques, etc.). Examples are normally compiled into the binary as
 * generated string arrays and can be loaded by the user via F12 cycling or the
 * Example dropdown menu. A runtime examples directory may replace the generated
 * catalog for authoring.
 */
#ifndef REPL_EXAMPLES_H
#define REPL_EXAMPLES_H

#include <limits.h>

#include "repl/catalog_tags.h"  /* ReplTagNode, ReplItemTagNode */

typedef enum {
    REPL_EXAMPLE_SOURCE_GLR = 0, /* raw REPL/example-source lines */
    REPL_EXAMPLE_SOURCE_C        /* full exported/importable C source */
} ReplExampleSourceFormat;

/* Query the source line array for an example. Returns a null-terminated array
 * whose interpretation depends on repl_example_source_format(idx). Index must
 * be in range [0, repl_example_count()). The returned pointer is valid until
 * the runtime examples catalog is replaced or cleared. */
const char *const *repl_example_lines(int idx);

/* Query how repl_example_lines() should be loaded. `.glr` catalog files are
 * raw REPL/example-source snippets; `.c` catalog files are complete exported-C
 * files and must be fed through the import path. */
ReplExampleSourceFormat repl_example_source_format(int idx);

/* Query the display name of an example (shown in the Example dropdown and menu).
 * Returns a pointer to a constant string. Index must be in range [0,
 * repl_example_count()). Used by the Example dropdown UI and by F12 cycling. */
const char *repl_example_name(int idx);

/* Query the total number of built-in examples. Used by the Example dropdown and
 * F12 cycling to determine the range of valid example indices. */
int repl_example_count(void);

/* Curated metadata tags used by the Scene menu. Dynamic linked list tag query API.
 * Tag index 0 is the synthetic "All" group - every example is a member. */
enum {
    REPL_EXAMPLE_TAG_ALL = 0,
    REPL_EXAMPLE_TAG_2D,
    REPL_EXAMPLE_TAG_3D,
    REPL_EXAMPLE_TAG_POLYGONS,
    REPL_EXAMPLE_TAG_LINES,
};

int repl_example_tag_count(void);
const char *repl_example_tag_label(int tag_idx);
int repl_example_has_tag(int example_idx, int tag_idx);
int repl_example_count_for_tag(int tag_idx);
int repl_example_index_for_tag(int tag_idx, int ordinal);
int repl_example_visible_tag_count(void);
int repl_example_visible_tag_at(int dense_idx);

/* Dynamic Tag Linked List Helpers */
const ReplTagNode *repl_example_find_or_register_tag(const char *name);
int repl_example_attach_tag(ReplItemTagNode **item_tags_head, const ReplTagNode *tag);
void repl_example_free_item_tags(ReplItemTagNode *item_tags_head);

/* Subheading API - mirrors repl_tutorial_subheading. Returns the example's
 * free-form section label or NULL (no subheading / out-of-range idx). */
const char *repl_example_subheading(int example_idx);

/* Optional runtime override for development/live iteration. `dir` must contain
 * a catalog.ini with file paths relative to `dir`. On success, the query API
 * serves that catalog instead of the compiled-in generated data. */
int  repl_examples_load_dir(const char *dir, char *err_buf, int err_sz);
void repl_examples_clear_runtime_catalog(void);

#endif /* REPL_EXAMPLES_H */
