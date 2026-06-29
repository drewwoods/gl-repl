#include "repl/examples.h"
#include "repl/catalog_tags.h"

#include <stddef.h>
#include "c_compat.h"   /* STATIC_ASSERT (C99/C11 portable) */

/* Each example is an array of source lines terminated by NULL.
 * Lines are processed sequentially:
 *   "for(...) {"  -> CMD_FOR_BEGIN + CMD_FOR_END, enters block
 *   "funcN {"     -> CMD_FUNC_DEF + CMD_FUNC_END, enters block
 *   "if(...) {"   -> CMD_IF_BEGIN + CMD_IF_END, enters block
 *   "}"           -> closes current block
 *   "x = expr;"   -> CMD_VAR_ASSIGN
 *   "funcN()"     -> CMD_CALL
 *   anything else -> parse_command() as a regular GL command
 *
 * Predefined examples can optionally start with contiguous scene-presentation
 * config metadata lines using the exported-file format:
 *   "// @cfg axes = AXES_THEME_COMPASS"
 *   "// @cfg vertex_outlines = 0"
 * Supported slugs are limited to visual scene settings such as wireframe,
 * grid, axes, vertex overlays, backdrop, and camera_rotate. These metadata
 * lines are consumed by the example loader and are not shown in the code panel.
 *
 * A camera preset may then follow immediately after the cfg lines:
 *   "// camera"
 *   "glTranslatef(0.0f, 0.0f, -dist);"
 *   "glRotatef(rx, 1.0f, 0.0f, 0.0f);"
 *   "glRotatef(ry, 0.0f, 1.0f, 0.0f);"
 *   "glTranslatef(-tx, -ty, -tz);"
 * Both cfg and camera metadata are consumed by the example loader and are not
 * shown in the code panel.
 */

/* Example source arrays and the catalog entry table are generated from
 * examples/catalog.ini plus scene snippets under examples/scenes/. */

typedef unsigned int ReplExampleTagMask;

typedef struct {
    const char *name;
    const char *const *lines;
    ReplExampleTagMask tags;
    /* Optional in-flyout section subheading. Free-form display string
     * (e.g. "Basics", "Functions") — the Scene menu groups consecutive
     * examples sharing the same subheading under a chrome `### subheading`
     * header. NULL = no header. Convention enforced by
     * test_example_subheading_metadata: per tag, each non-NULL subheading
     * must appear in a single contiguous run — interleaving causes
     * duplicate headers. */
    const char *subheading;
} ReplExampleEntry;

/* REPL_EXAMPLE_TAG_ALL is a synthetic tag: every example is a member.
 * It is not listed in any g_example_entries[] mask literal; instead
 * repl_example_tag_mask() ORs its bit into every example's mask, so the
 * whole tag query API (has_tag / count_for_tag / index_for_tag /
 * visible_tag_*) — and therefore the Scene menu's "All" group and the
 * F12 cycle — pick it up with no per-entry bookkeeping. Kept at index 0
 * so "All" sorts first under "### EXAMPLES". Enum lives in examples.h. */

#define EXAMPLE_TAG_BIT(tag) (1u << (tag))
#define EXAMPLE_TAG_ALL      EXAMPLE_TAG_BIT(REPL_EXAMPLE_TAG_ALL)
#define EXAMPLE_TAG_2D       EXAMPLE_TAG_BIT(REPL_EXAMPLE_TAG_2D)
#define EXAMPLE_TAG_3D       EXAMPLE_TAG_BIT(REPL_EXAMPLE_TAG_3D)
#define EXAMPLE_TAG_POLYGONS EXAMPLE_TAG_BIT(REPL_EXAMPLE_TAG_POLYGONS)
#define EXAMPLE_TAG_LINES    EXAMPLE_TAG_BIT(REPL_EXAMPLE_TAG_LINES)

static const char *const g_example_tag_labels[] = {
    "All",
    "2D",
    "3D",
    "Polygons",
    "Lines",
};
/* Must stay 1:1 with the REPL_EXAMPLE_TAG_* enum: repl_example_tag_count()
 * returns REPL_EXAMPLE_TAG_COUNT but repl_example_tag_label() indexes this
 * table, so any drift is an out-of-bounds read. */
STATIC_ASSERT((int)(sizeof(g_example_tag_labels) /
                    sizeof(g_example_tag_labels[0])) == REPL_EXAMPLE_TAG_COUNT,
              "g_example_tag_labels[] out of sync with REPL_EXAMPLE_TAG_COUNT");

/* Catalog order matters: the Scene menu groups consecutive same-subheading
 * entries into one `### subheading` header per per-tag flyout, so each
 * subheading must run contiguously within every tag it surfaces under.
 * test_example_subheading_metadata enforces this. */
#include "../../build/generated/repl_examples_data.inc"

static int example_catalog_entry_count(void) {
    return repl_example_count();
}

static int example_catalog_tag_count(void) {
    return REPL_EXAMPLE_TAG_COUNT;
}

static unsigned int example_catalog_tag_bit(int tag_idx) {
    return repl_example_tag_bit(tag_idx);
}

static const ReplCatalogTagOps g_example_tag_ops = {
    example_catalog_entry_count,
    example_catalog_tag_count,
    g_example_tag_labels,
    repl_example_tag_mask,
    example_catalog_tag_bit,
};

int repl_example_count(void) {
    return (int)(sizeof(g_example_entries) / sizeof(g_example_entries[0]));
}

const char *repl_example_name(int idx) {
    if (idx < 0 || idx >= repl_example_count())
        return NULL;
    return g_example_entries[idx].name;
}

const char *const *repl_example_lines(int idx) {
    if (idx < 0 || idx >= repl_example_count())
        return NULL;
    return g_example_entries[idx].lines;
}

int repl_example_tag_count(void) {
    return REPL_EXAMPLE_TAG_COUNT;
}

unsigned int repl_example_tag_mask(int example_idx) {
    if (example_idx < 0 || example_idx >= repl_example_count())
        return 0u;
    /* Fold in the synthetic "All" membership so every tag query derives
     * it uniformly; entry literals stay free of an explicit ALL bit. */
    return g_example_entries[example_idx].tags | EXAMPLE_TAG_ALL;
}

REPL_DEFINE_CATALOG_TAG_WRAPPERS(example, &g_example_tag_ops)

const char *repl_example_subheading(int example_idx) {
    if (example_idx < 0 || example_idx >= repl_example_count())
        return NULL;
    return g_example_entries[example_idx].subheading;
}
