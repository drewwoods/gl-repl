/* format.h - minimal REPL formatting helpers shared by core.c and tests. */
#ifndef REPL_FORMAT_H
#define REPL_FORMAT_H

/* Reindent a source line by extracting the existing leading whitespace from
 * parsed_source and applying it to raw_expr after stripping raw_expr's own
 * leading whitespace. */
void repl_format_reindent_from_parsed(const char *parsed_source, const char *raw_expr,
                               char *out, int out_sz);

#endif /* REPL_FORMAT_H */
