/*
 * src/repl/attrib_bits.h - glPushAttrib/glPopAttrib attribute-bit mapping.
 *
 * Pure (no GL calls, no live-GL state reads) mapping layer shared by the
 * editor's per-bit cursor highlighting (glr_ctrl) and, later, the OpenGL-state
 * popup fold (gl_state_inspector). Answers three questions about the REPL
 * source document:
 *
 *   1. repl_attrib_bits_for_cmd  - which GL_*_BIT groups does one command's
 *      state fall under (context-free coarse mask; multi-bit membership falls
 *      out naturally, matching real GL).
 *   2. repl_attrib_cmd_writes    - which *atomic state cells* does one command
 *      write, each tagged with its covering attribute bits. Flow-sensitive:
 *      while GL_COLOR_MATERIAL is enabled a glColor* line also (re)writes the
 *      selected material cells, so the caller threads a ReplAttribFlowState.
 *   3. the two collectors        - fold the source document with real LIFO
 *      attribute-stack semantics and report which prior/scoped setter lines a
 *      push saves / a pop reverts, for the editor highlight.
 *
 * The 10 supported bits and their canonical order are owned by
 * command_spec.c's k_attrib_bits[] (repl_attrib_bit_entries()); this module
 * derives everything from that single table so it cannot drift.
 */
#ifndef REPL_ATTRIB_BITS_H
#define REPL_ATTRIB_BITS_H

#include "repl/command.h"

/* Number of supported GL_*_BIT groups (mirrors k_attrib_bits[]). */
#define REPL_ATTRIB_BIT_COUNT 11

/* Depth of the real glPushAttrib/glPopAttrib stack all REPL mirrors keep.
 * Virtual user-source depth is unbounded; only the first frames at this depth
 * drive or snapshot real GL state. OpenGL guarantees at least 16 attribute
 * stack entries, while the app also uses one outer frame. */
#define REPL_ATTRIB_STACK_CAP 8

/* A comfortable cap for a collector result: at most one line per distinct
 * atomic state cell (~55), coalesced, so 64 can never truncate a real
 * document. Callers size their ReplAttribHighlightLine[] buffers with this. */
#define REPL_ATTRIB_HL_MAX 64

/* Upper bound on atomic state cells a single command can write. Five is the
 * maximum: a glColor* line owns its CURRENT color cell plus, while color
 * material is enabled, as many as four selected material cells
 * (GL_FRONT_AND_BACK x GL_AMBIENT_AND_DIFFUSE). */
#define REPL_ATTRIB_MAX_ITEMS_PER_CMD 5

/* Caller-threaded flow context for the context-sensitive color-material
 * analysis. Initialize with repl_attrib_flow_init() (the GL defaults:
 * disabled, GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE); repl_attrib_cmd_writes
 * reads it (does glColor* drive material cells?) and updates it (glEnable/
 * glDisable(GL_COLOR_MATERIAL) and glColorMaterial change it). */
typedef struct {
    int      color_material_enabled;
    unsigned color_material_face;
    unsigned color_material_mode;
} ReplAttribFlowState;

/* One atomic state cell a command writes: a normalized 32-bit identity plus
 * the GL_*_BIT mask under which glPushAttrib saves / glPopAttrib restores it. */
typedef struct {
    unsigned item_id;
    unsigned attrib_bits;
} ReplAttribCellWrite;

/* One editor highlight line: a source line index plus the union of canonical
 * bit *indices* (0..9, as a bitmask) whose colors should mark it. */
typedef struct {
    int      line_idx;
    unsigned bit_idx_mask;
} ReplAttribHighlightLine;

/* One supported GL_*_BIT token found in source text. Char offsets address the
 * exact string passed to repl_attrib_bit_token_spans(), so renderers can color
 * a live edit buffer without translating coordinates from a committed row. */
typedef struct {
    int char_start;
    int char_end;
    int bit_idx;
} ReplAttribTokenSpan;

/* Context-free coarse mask of GL_*_BIT groups a command's state falls under.
 * 0 when the command is not an attribute-scoped state setter. Does NOT fold in
 * the flow-dependent color-material -> LIGHTING membership (that is
 * context-sensitive and belongs only in repl_attrib_cmd_writes). */
unsigned repl_attrib_bits_for_cmd(const GLCmd *cmd);

/* Same mask keyed by command type alone, for consumers gating a restore on
 * membership without a real command in hand (the inspector's group restore,
 * the executor's bookkeeping restore, the overlay walkers). `enum_arg0` is the
 * cap enum for CMD_ENABLE/CMD_DISABLE (whose membership depends on it); every
 * other type ignores it (pass 0). Routing these checks through here keeps
 * cell->bit membership in exactly one place. */
unsigned repl_attrib_bits_for_type(CmdType type, unsigned enum_arg0);

/* Canonical index 0..REPL_ATTRIB_BIT_COUNT-1 of a single GL_*_BIT value, or -1
 * if it is not one of the supported bits. */
int repl_attrib_bit_index(unsigned single_bit);

/* Scan `text` for supported GL_*_BIT names inside its first (...) range.
 * Fills up to `max` spans and returns the number written. The scan is purely
 * textual: callers decide whether a matched bit is active in a parsed mask. */
int repl_attrib_bit_token_spans(const char *text, ReplAttribTokenSpan *out,
                                int max);

/* Initialize a flow context to the GL defaults. */
void repl_attrib_flow_init(ReplAttribFlowState *flow);

/* Fill out[] with the atomic cells `cmd` writes (returns the count, 0..5) and
 * update *flow. `flow` may be NULL (treated as GL defaults, no update). */
int repl_attrib_cmd_writes(const GLCmd *cmd, ReplAttribFlowState *flow,
                           ReplAttribCellWrite out[REPL_ATTRIB_MAX_ITEMS_PER_CMD]);

/* Cursor-on-push: the prior setter lines whose values glPushAttrib(push_line)
 * saves ("what's saved"). Cursor-on-pop: the scoped setter lines between the
 * matching push and glPopAttrib(pop_line) whose effects the pop reverts
 * ("what's reverted"). Both fold the live source document with real nested
 * LIFO attribute-stack semantics over the same block-unaware source-order
 * policy as repl_source_scope_collect_unbalanced; an orphan pop reports
 * nothing. Coalesced by line (bit indices unioned). Return the number of lines
 * written, capped at `max`. Return 0 when the cursor is not on the right
 * bracket. */
int repl_attrib_collect_push_saved(int push_line,
                                   ReplAttribHighlightLine *out, int max);
int repl_attrib_collect_pop_reverted(int pop_line,
                                     ReplAttribHighlightLine *out, int max);

#endif /* REPL_ATTRIB_BITS_H */
