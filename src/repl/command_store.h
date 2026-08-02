/*
 * src/repl/command_store.h - Source-command array mutations (insert/delete/replace).
 *
 * Minimal facade for shifting and replacing entries in the live source-command
 * array. Centralizes array mechanics (inserting/deleting ranges, shifting
 * entries, range normalization) without owning parsing, undo, variable
 * registration, or editor cursor policy. Callers handle those domain-specific
 * concerns; mutations go through this one place for consistency and testability.
 *
 * Command store facade: ReplCommandStore wraps a pointer to the live command
 * array and its count and capacity. Obtained via repl_command_store_live(),
 * which reads the current REPL state. All mutations take a store reference
 * and update the underlying array atomically.
 *
 * Cursor (edit-line) policy lives with the caller, not the store. Inserts and
 * deletes that shift the document can mechanically shift a caller-owned
 * cursor; pass an optional `int *cursor_inout` to have the store apply the
 * standard insert/delete math to the caller's int in place. NULL means "no
 * cursor mutation - pure data op." The cursor pointer is supplied per call,
 * so the store does not retain editor state.
 *
 * Only insert ops also honour REPL_COMMAND_STORE_ADJUST_EDIT_LINE as the
 * intent flag (so callers can pass a non-NULL `cursor_inout` for capacity
 * accounting while still asking the insert math to skip the cursor - useful
 * for autocommit-without-advance code paths). Delete and replace ops do not
 * check the flag; delete callers gate by passing NULL/non-NULL directly,
 * replace never shifts the cursor.
 *
 * Design: This module owns array mechanics (shifting, bounds checking,
 * capacity). Callers own parsing, undo snapshots, variable registration, cursor
 * policy, and error recovery. Mutations are atomic within the store layer;
 * compound operations (insert + declare variable + mark dirty) are coordinated
 * by callers.
 */
#ifndef REPL_COMMAND_STORE_H
#define REPL_COMMAND_STORE_H

#include "repl/command.h"

/* Facade over the live source-command array. Points to the current command
 * buffer, its count, and capacity. Obtained via repl_command_store_live().
 * All mutations take a store reference and update these atomically. Cursor
 * (edit-line) state is no longer held by the store - pass an optional
 * `int *cursor_inout` to the mutating ops to have the store shift a
 * caller-owned cursor by the standard insert/delete math. */
typedef struct {
    GLCmd *cmds;
    int   *count;
    int    capacity;
} ReplCommandStore;

/* Flags for insert operations. REPL_COMMAND_STORE_ADJUST_EDIT_LINE gates
 * the cursor shift on insert ops (delete and replace ignore it). When set
 * AND the caller passes a non-NULL `cursor_inout` in opts, the store
 * applies the standard insert math to the caller's int. */
enum {
    REPL_COMMAND_STORE_ADJUST_EDIT_LINE = 1 << 0
};

/* Options bag for cursor-aware store mutators (insert / delete). Pass
 * NULL for the default (`flags = 0`, `cursor_inout = NULL`) - pure data
 * op, no cursor math.
 *
 * `flags` is consulted only by inserts (delete callers gate by
 * NULL/non-NULL cursor_inout instead - there is no insert-equivalent
 * "adjust" flag for delete). `cursor_inout` is the caller-owned cursor
 * the store shifts in place when non-NULL; for inserts the shift is
 * also gated on REPL_COMMAND_STORE_ADJUST_EDIT_LINE. */
typedef struct {
    int  flags;
    int *cursor_inout;
} ReplStoreMutOpts;

/* Obtain a facade over the live command array (g_cmds[], its count, and
 * capacity). Valid only in the current scope; re-fetch when needed. Used by
 * callers that own the mutation logic and want to commit changes. */
ReplCommandStore repl_command_store_live(void);

/* Query the current command count in the store. */
int  repl_command_store_count(const ReplCommandStore *store);

/* Query the capacity of the store (max commands). */
int  repl_command_store_capacity(const ReplCommandStore *store);

/* Check whether the store can accommodate count additional commands without
 * overflow. Returns 1 if there's space, 0 if full. Used for pre-flight checks
 * before insertion. */
int  repl_command_store_can_insert(const ReplCommandStore *store, int count);

/* Normalize a (start, count) range into valid bounds. Clamps to array bounds
 * and returns the adjusted (out_start, out_count). Returns 1 if the resulting
 * range is non-empty, 0 if empty after clamping. Used for range validation
 * before delete/replace operations. */
int  repl_command_store_normalize_range(const ReplCommandStore *store,
                                        int start, int count,
                                        int *out_start, int *out_count);

/* Insert multiple commands at position pos. Shifts cmds[pos..count) to the
 * right, then copies the source commands into the freed space. Returns 1
 * on success, 0 on error (full or out of bounds). Called by paste,
 * load-from-file, and undo operations.
 *
 * `opts` is optional; pass NULL for the default (no cursor math). When
 * non-NULL with `opts->cursor_inout != NULL && opts->flags &
 * REPL_COMMAND_STORE_ADJUST_EDIT_LINE`, the store applies the insert math
 * to `*opts->cursor_inout`: when `pos <= *cursor_inout`, cursor += count.
 *
 * This API mutates only the GLCmd array. Callers that need parallel text
 * writes pair the call with `editor_buffer_insert_lines(pos, lines, count)`
 * (typically inside one undo transaction). */
int  repl_command_store_insert_many(ReplCommandStore *store, int pos,
                                    const GLCmd *cmds, int count,
                                    const ReplStoreMutOpts *opts);

/* Insert a single command. Convenience wrapper around insert_many(). Returns
 * 1 on success, 0 on error. Pair with `editor_buffer_insert_line(pos, line)`
 * when a parallel text write is needed. */
int  repl_command_store_insert_one(ReplCommandStore *store, int pos,
                                   const GLCmd *cmd,
                                   const ReplStoreMutOpts *opts);

/* Replace a single command at pos. Does not shift; overwrites in place.
 * Returns 1 on success, 0 if out of bounds. Used for editing existing lines
 * without changing array size. Pair with
 * `editor_buffer_replace_line(pos, line)` when a parallel text write is
 * needed. No opts parameter - replace never shifts the cursor. */
int  repl_command_store_replace_one(ReplCommandStore *store, int pos,
                                    const GLCmd *cmd);

/* Delete a range of commands. Shifts cmds[start+count..] left to fill the
 * deleted space. Returns 1 on success, 0 on error. Pair with
 * `editor_buffer_delete_range(start, count)` for the parallel text shift.
 *
 * `opts` is optional; pass NULL for no cursor math. When non-NULL with
 * `opts->cursor_inout != NULL`, the store applies the standard delete
 * math to `*opts->cursor_inout`: cursors before the deleted range are
 * unchanged; cursors inside it snap to `start`; cursors past it move
 * back by `count`. The result is clamped to [0, post-delete line count].
 * `opts->flags` is ignored - delete callers gate via NULL/non-NULL
 * `cursor_inout` directly. */
int  repl_command_store_delete_range(ReplCommandStore *store, int start,
                                     int count,
                                     const ReplStoreMutOpts *opts);

/* Load a complete command array into the store (bulk replace). Clears the
 * current commands and copies in the new array. Returns 1 on success, 0 if
 * the array doesn't fit. Pair with `editor_buffer_load_lines(lines, count)`
 * for the parallel text bulk load.
 *
 * No cursor parameter - cursor policy after a bulk load is the caller's
 * concern (scene-restore preserves the saved value, reset zeroes, etc.).
 * Callers that need to update the cursor do so explicitly before/after
 * the call. */
int  repl_command_store_load(ReplCommandStore *store,
                             const GLCmd *cmds, int count);

/* Clear all commands (set count to 0). Does not adjust any cursor; caller
 * must handle cursor repositioning. Pair with `editor_buffer_clear()` for
 * the parallel text clear. */
void repl_command_store_clear(ReplCommandStore *store);

#endif /* REPL_COMMAND_STORE_H */
