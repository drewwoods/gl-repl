/*
 * src/repl/reformat.h - Whole-document REPL source reformatter.
 */
#ifndef REPL_REFORMAT_H
#define REPL_REFORMAT_H

/* Pure REPL pass: walks every command and rewrites the canonical
 * line text + GLCmd in place. Does not save/restore editor input;
 * the editor's Ctrl+\ wrapper (`editor_reformat_commands`) layers
 * that on top. */
void repl_reformat_program(void);

#endif /* REPL_REFORMAT_H */
