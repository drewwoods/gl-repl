/*
 * src/repl/bootstrap.h - Startup loading helpers.
 */
#ifndef REPL_BOOTSTRAP_H
#define REPL_BOOTSTRAP_H

/* Returns the post-load cursor target. `import_file` may be "-" to consume
 * stdin. Caller applies the value via editor_state_edit_line_set() above the
 * beta boundary. */
int repl_load_initial_commands(const char *import_file);

#endif /* REPL_BOOTSTRAP_H */
