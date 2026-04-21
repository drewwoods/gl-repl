#ifndef REPL_COMMAND_STORE_H
#define REPL_COMMAND_STORE_H

#include "sample.h"

/* Source-command storage facade.
 *
 * This is intentionally small: it centralizes the mechanics of shifting and
 * replacing entries in g_cmds[] without taking over parsing, undo, variable
 * declaration registration, or editor cursor policy. Callers still own those
 * domain-specific decisions, but command-array mutations now go through one
 * place.
 */
typedef struct {
    GLCmd *cmds;
    int   *count;
    int    capacity;
    int   *edit_line;
} ReplCommandStore;

enum {
    REPL_COMMAND_STORE_ADJUST_EDIT_LINE = 1 << 0
};

ReplCommandStore repl_command_store_live(void);

int  repl_command_store_count(const ReplCommandStore *store);
int  repl_command_store_capacity(const ReplCommandStore *store);
int  repl_command_store_can_insert(const ReplCommandStore *store, int count);
int  repl_command_store_first_non_decl(const ReplCommandStore *store);
int  repl_command_store_normalize_range(const ReplCommandStore *store,
                                        int start, int count,
                                        int *out_start, int *out_count);

int  repl_command_store_insert_many(ReplCommandStore *store, int pos,
                                    const GLCmd *cmds, int count, int flags);
int  repl_command_store_insert_one(ReplCommandStore *store, int pos,
                                   const GLCmd *cmd, int flags);
int  repl_command_store_replace_one(ReplCommandStore *store, int pos,
                                    const GLCmd *cmd);
int  repl_command_store_delete_range(ReplCommandStore *store, int start,
                                     int count);
void repl_command_store_clear(ReplCommandStore *store);

#endif /* REPL_COMMAND_STORE_H */
