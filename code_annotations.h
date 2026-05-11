/*
 * code_annotations.h - Neutral code-panel annotation port.
 *
 * REPL pipeline TUs (today: repl_replay_annotations.c) emit annotation
 * rows the UI / editor renders below source lines (replay variable-
 * substitution view, evaluation values, etc). The REPL code calls
 * code_annotations_clear() + code_annotations_append() to push the
 * rows; the host implementation routes them to the appropriate
 * presentation layer:
 *
 *   Full app: glr_code_annotations.c forwards to EditorVirtualLineList
 *   Demo:     no-op (no UI to render to)
 *   Tests:    full-app adapter via the test link set
 *
 * Same shape as source_document.h — a neutral boundary header with no
 * REPL or editor headers. Sizes live in config.h so the host adapter
 * and the REPL caller agree on capacity without including each other.
 *
 * Phase 4 of feature/source-document-port.md.
 */
#ifndef CODE_ANNOTATIONS_H
#define CODE_ANNOTATIONS_H

/* Per-row capacity. Callers size their formatting buffers from these.
 * Host adapters must accommodate at least these bytes before storing. */
#ifndef CODE_ANNOTATION_TEXT_MAX
#define CODE_ANNOTATION_TEXT_MAX 256
#endif

#ifndef CODE_ANNOTATION_AUX_MAX
#define CODE_ANNOTATION_AUX_MAX  128
#endif

typedef enum {
    CODE_ANNOTATION_STYLE_REPLAY_SUBST,
    CODE_ANNOTATION_STYLE_REPLAY_EVAL,
} CodeAnnotationStyle;

/* Drop all queued annotations. Called once per frame before refilling. */
void code_annotations_clear(void);

/* Push one annotation row anchored after source line `after_line_idx`.
 * `text` is the primary annotation body (e.g., variable-substituted
 * source); `aux` is an optional trailing comment. NULL aux means "no
 * trailing comment". Returns 1 on success, 0 if the host's capacity
 * is exhausted. */
int  code_annotations_append(int after_line_idx,
                             CodeAnnotationStyle style,
                             const char *text,
                             const char *aux);

#endif /* CODE_ANNOTATIONS_H */
