/*
 * repl_command_store.h - Source-command array mutations (insert/delete/replace).
 *
 * Minimal facade for shifting and replacing entries in the live source-command
 * array. Centralizes array mechanics (inserting/deleting ranges, shifting
 * entries, range normalization) without owning parsing, undo, variable
 * registration, or editor cursor policy. Callers handle those domain-specific
 * concerns; mutations go through this one place for consistency and testability.
 *
 * Command store facade: ReplCommandStore wraps a pointer to the live command
 * array and its count, capacity, and edit-line index. Obtained via
 * repl_command_store_live(), which reads the current REPL state. All mutations
 * take a store reference and update the underlying array and related state
 * atomically.
 *
 * Only the insert helpers support REPL_COMMAND_STORE_ADJUST_EDIT_LINE, which
 * automatically adjusts the cursor position if mutations occur before/at the
 * cursor line. Delete and replace operations leave cursor adjustment to the
 * caller.
 *
 * Design: This module owns array mechanics (shifting, bounds checking,
 * capacity). Callers own parsing, undo snapshots, variable registration, cursor
 * policy, and error recovery. Mutations are atomic within the store layer;
 * compound operations (insert + declare variable + mark dirty) are coordinated
 * by callers.
 */
#ifndef REPL_COMMAND_STORE_H
#define REPL_COMMAND_STORE_H

#include "sample.h"

/* Facade over the live source-command array. Points to the current command
 * buffer, its count, capacity, and edit-line index. Obtained via
 * repl_command_store_live(). All mutations take a store reference and update
 * these atomically. */
typedef struct {
    GLCmd *cmds;
    int   *count;
    int    capacity;
    int   *edit_line;
} ReplCommandStore;

static inline void repl_command_store_source_to_line(char *dst, int dst_sz,
                                                     const char *source) {
    int len;

    if (!dst || dst_sz <= 0)
        return;

    if (!source)
        source = "";
    while (*source && isspace((unsigned char)*source))
        source++;

    len = (int)strlen(source);
    while (len > 0 && isspace((unsigned char)source[len - 1]))
        len--;
    while (len > 0 && source[len - 1] == ';')
        len--;
    while (len > 0 && isspace((unsigned char)source[len - 1]))
        len--;

    if (len >= dst_sz)
        len = dst_sz - 1;
    memcpy(dst, source, (size_t)len);
    dst[len] = '\0';
}

/* Flags for insert operations. REPL_COMMAND_STORE_ADJUST_EDIT_LINE
 * automatically adjusts edit_line if the mutation occurs before/at the cursor.
 * Delete and replace operations do not accept flags. */
enum {
    REPL_COMMAND_STORE_ADJUST_EDIT_LINE = 1 << 0
};

/* Obtain a facade over the live command array (g_cmds[], its count, capacity,
 * and edit-line index). Valid only in the current scope; re-fetch when needed.
 * Used by callers that own the mutation logic and want to commit changes. */
ReplCommandStore repl_command_store_live(void);

/* Query the current command count in the store. */
int  repl_command_store_count(const ReplCommandStore *store);

/* Query the capacity of the store (max commands). */
int  repl_command_store_capacity(const ReplCommandStore *store);

/* Check whether the store can accommodate count additional commands without
 * overflow. Returns 1 if there's space, 0 if full. Used for pre-flight checks
 * before insertion. */
int  repl_command_store_can_insert(const ReplCommandStore *store, int count);

/* Find the first non-CMD_VAR_DECLARE command index. Used by insertion logic to
 * enforce the rule that variable declarations always occupy the top of the
 * command array (before any other code). New non-decl code is inserted at this
 * index; new decls are inserted before it. */
int  repl_command_store_first_non_decl(const ReplCommandStore *store);

/* Normalize a (start, count) range into valid bounds. Clamps to array bounds
 * and returns the adjusted (out_start, out_count). Returns 1 if the resulting
 * range is non-empty, 0 if empty after clamping. Used for range validation
 * before delete/replace operations. */
int  repl_command_store_normalize_range(const ReplCommandStore *store,
                                        int start, int count,
                                        int *out_start, int *out_count);

/* Optional text-aware insertion helpers. When lines is non-NULL, the editor
 * buffer is updated from that parallel text array instead of re-reading
 * cmd->source after the array shift. */
int  repl_command_store_insert_many_with_lines(ReplCommandStore *store, int pos,
                                               const GLCmd *cmds, int count,
                                               int flags,
                                               const char *const *lines);

static inline int repl_command_store_insert_many_without_lines(
        ReplCommandStore *store, int pos, const GLCmd *cmds, int count,
        int flags) {
    return repl_command_store_insert_many_with_lines(store, pos, cmds, count,
                                                     flags, NULL);
}

/* Insert multiple commands at position pos. Shifts cmds[pos..count) to the right,
 * then copies the source commands into the freed space. flags control whether to
 * auto-adjust edit_line. Returns 1 on success, 0 on error (full or out of bounds).
 * Called by paste, load-from-file, and undo operations. */
#define REPL_COMMAND_STORE_INSERT_MANY_SELECT(_1, _2, _3, _4, _5, _6, NAME, ...) NAME
#define repl_command_store_insert_many(...) \
    REPL_COMMAND_STORE_INSERT_MANY_SELECT(__VA_ARGS__, \
                                          repl_command_store_insert_many_with_lines, \
                                          repl_command_store_insert_many_without_lines)(__VA_ARGS__)

int  repl_command_store_insert_one_with_line(ReplCommandStore *store, int pos,
                                             const GLCmd *cmd, int flags,
                                             const char *line);

static inline int repl_command_store_insert_one_without_line(
        ReplCommandStore *store, int pos, const GLCmd *cmd, int flags) {
    return repl_command_store_insert_one_with_line(store, pos, cmd, flags,
                                                   NULL);
}

/* Insert a single command. Convenience wrapper around insert_many(). Returns 1
 * on success, 0 on error. */
#define REPL_COMMAND_STORE_INSERT_ONE_SELECT(_1, _2, _3, _4, _5, NAME, ...) NAME
#define repl_command_store_insert_one(...) \
    REPL_COMMAND_STORE_INSERT_ONE_SELECT(__VA_ARGS__, \
                                         repl_command_store_insert_one_with_line, \
                                         repl_command_store_insert_one_without_line)(__VA_ARGS__)

int  repl_command_store_replace_one_with_line(ReplCommandStore *store, int pos,
                                              const GLCmd *cmd,
                                              const char *line);

static inline int repl_command_store_replace_one_without_line(
        ReplCommandStore *store, int pos, const GLCmd *cmd) {
    return repl_command_store_replace_one_with_line(store, pos, cmd, NULL);
}

/* Replace a single command at pos. Does not shift; overwrites in place.
 * Returns 1 on success, 0 if out of bounds. Used for editing existing lines
 * without changing array size. */
#define REPL_COMMAND_STORE_REPLACE_ONE_SELECT(_1, _2, _3, _4, NAME, ...) NAME
#define repl_command_store_replace_one(...) \
    REPL_COMMAND_STORE_REPLACE_ONE_SELECT(__VA_ARGS__, \
                                          repl_command_store_replace_one_with_line, \
                                          repl_command_store_replace_one_without_line)(__VA_ARGS__)

/* Replace the color args and source text of an existing color command in
 * place. Marks the flat program dirty. Returns 1 on success, 0 if cmd_idx is
 * out of range, the command type is not editable by the color picker, or the
 * formatted source text does not fit. */
int  repl_command_store_set_color(int cmd_idx,
                                  float r, float g, float b, float a,
                                  int has_alpha);

/* Variant for CMD_CLEAR_COLOR: clamps r/g/b to CP_CLEAR_MAX_V before
 * writing. Returns 1 on success, 0 on out-of-range or formatting failure. */
int  repl_command_store_set_clear_color(int cmd_idx,
                                        float r, float g, float b, float a);

/* Delete a range of commands. Shifts cmds[start+count..] left to fill the
 * deleted space. Returns 1 on success, 0 on error. Called by delete, cut, and
 * clear-all operations. */
int  repl_command_store_delete_range(ReplCommandStore *store, int start,
                                     int count);

/* Load a complete command array into the store (bulk replace). Clears the
 * current commands and copies in the new array, setting edit_line as specified.
 * Returns 1 on success, 0 if the array doesn't fit. Used by load-from-file,
 * undo, and example loading. */
int  repl_command_store_load_with_lines(ReplCommandStore *store,
                                        const GLCmd *cmds, int count,
                                        const char *const *lines,
                                        int edit_line);

static inline int repl_command_store_load_without_lines(
        ReplCommandStore *store, const GLCmd *cmds, int count, int edit_line) {
    return repl_command_store_load_with_lines(store, cmds, count, NULL,
                                              edit_line);
}

#define REPL_COMMAND_STORE_LOAD_SELECT(_1, _2, _3, _4, _5, NAME, ...) NAME
#define repl_command_store_load(...) \
    REPL_COMMAND_STORE_LOAD_SELECT(__VA_ARGS__, \
                                   repl_command_store_load_with_lines, \
                                   repl_command_store_load_without_lines)(__VA_ARGS__)

/* Clear all commands (set count to 0). Does not adjust edit_line; caller
 * must handle cursor repositioning. Used by Ctrl+L (clear all) and shutdown. */
void repl_command_store_clear(ReplCommandStore *store);

#endif /* REPL_COMMAND_STORE_H */
