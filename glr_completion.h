/*
 * glr_completion.h - Controller-side completion provider entry points.
 *
 * glr_completion.c is the cross-domain adapter that translates REPL
 * grammar knowledge (command spec, predefined-variable table, source
 * CMD_FUNC_DEF entries) into the editor's autocomplete state. The
 * editor invokes the matcher through the EditorCompletionProvider
 * seam in src/editor/completion.h; only two symbols need to be
 * visible to callers outside the provider itself:
 *
 *  - `glr_completion_register_provider()` is called once at startup
 *    so the editor knows about the REPL grammar.
 *  - `accept_autocomplete()` is called by the editor input dispatch
 *    when the user accepts the current ghost (Tab / Enter on the
 *    popup). The acceptance behavior — append ghost text to the input
 *    buffer, advance the cursor, clear the popup — knows enough about
 *    REPL grammar (mode-specific suffixes, function parameter hints)
 *    that it lives here rather than inside the generic editor
 *    dispatch.
 */
#ifndef GLR_COMPLETION_H
#define GLR_COMPLETION_H

/* Commit the current ghost (selected match suffix) into the editor
 * input buffer, advance the cursor, and clear the popup state. */
void accept_autocomplete(void);

/* Register glr_completion as the editor's EditorCompletionProvider.
 * Called once at startup before the editor processes input. */
void glr_completion_register_provider(void);

#endif /* GLR_COMPLETION_H */
