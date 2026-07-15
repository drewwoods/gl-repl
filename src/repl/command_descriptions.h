/*
 * src/repl/command_descriptions.h - Compiled-in GL command help lookup.
 *
 * The authored catalog lives in command_descriptions.txt. The build validates
 * it against CmdType and the glEnable/glDisable capability table, then embeds
 * generated string records in command_descriptions.c.
 */
#ifndef REPL_COMMAND_DESCRIPTIONS_H
#define REPL_COMMAND_DESCRIPTIONS_H

#include "repl/command.h"

typedef struct {
    const char *title;
    const char *body;
} ReplCommandDescription;

/* Resolve popup help for one committed command. glEnable/glDisable select a
 * capability record by args[0]; all other GL-family commands select by type.
 * Returns 1 and fills out on success, else returns 0 and clears out. */
int repl_command_description_lookup(const GLCmd *cmd,
                                    ReplCommandDescription *out);

#endif /* REPL_COMMAND_DESCRIPTIONS_H */
