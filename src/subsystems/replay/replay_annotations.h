/*
 * src/subsystems/replay/replay_annotations.h - Replay-time source annotations and variable substitution.
 *
 * During replay, the code panel shows annotated versions of source lines:
 * variable values substituted into expressions and the result of evaluating
 * the line's expressions. This module fills a ReplReplayAnnotationOutput each
 * frame and leaves publication to the caller's presentation layer. The REPL
 * pipeline does not call into the editor's virtual-line API, so demos and
 * other non-UI hosts can link this TU without editor rendering code. The
 * output shape is a return-value struct supplied by the caller.
 *
 * Public surface:
 *   - replay_annotations_prepare() - fills the output. Idempotent
 *     within a frame; safe to call from the controller, layout, and
 *     tests. The full app uses glr_publish_replay_annotations() (in
 *     app/glr_ctrl_replay_annotations.h) to copy the rows into
 *     editor_state_virtual_lines.
 *   - replay_code_panel_get_command_display_text() returns the
 *     in-line annotated source text for a command (no extra rows).
 *
 * The substitution / evaluation builders and the per-source flat-cmd
 * lookup are file-private; they are only invoked from the prepare
 * helper and the on-line display-text helper.
 */
#ifndef REPL_REPLAY_ANNOTATIONS_H
#define REPL_REPLAY_ANNOTATIONS_H

#include "source_document.h"  /* caller-supplied SourceTextView */
#include "repl/eval.h"        /* REPL_FUNC_NAME_MAX, REPL_PREDEF_NAME_MAX, MAX_EXPR_VARS */
#include "repl/flatten.h"     /* MAX_FLATTEN_CALL_DEPTH */

/* Per-row capacity. Callers reading the output struct should treat the
 * arrays as NUL-terminated within these bounds. Sized to match the
 * full-app's editor virtual-line storage so the publish step copies
 * without truncation; the host may impose its own further bound. */
#ifndef REPL_REPLAY_ANNOTATION_TEXT_MAX
#define REPL_REPLAY_ANNOTATION_TEXT_MAX 256
#endif
#ifndef REPL_REPLAY_ANNOTATION_AUX_MAX
#define REPL_REPLAY_ANNOTATION_AUX_MAX  128
#endif

typedef enum {
    REPL_REPLAY_ANNOTATION_KIND_PATH,   /* call-frame breadcrumb (Verbose) */
    REPL_REPLAY_ANNOTATION_KIND_SUBST,  /* variable-substituted source line */
    REPL_REPLAY_ANNOTATION_KIND_EVAL,   /* `glVertex3f(value, ...);` evaluated form */
} ReplReplayAnnotationKind;

/* Structured PATH payload. One per prepare(), published beside the
 * virtual-line list. The UI formatter must not call back into REPL
 * parsing: every name and value the row needs is here. v1 omits @line
 * labels; call_src_cmd_idx is kept so that work needs no transport change. */
#ifndef REPL_REPLAY_PATH_RUNG_MAX
#define REPL_REPLAY_PATH_RUNG_MAX MAX_FLATTEN_CALL_DEPTH
#endif

typedef struct {
    char  name[REPL_PREDEF_NAME_MAX];
    float value;
} ReplReplayPathLoop;

typedef struct {
    char  func_name[REPL_FUNC_NAME_MAX];
    int   arg_count;
    char  param_names[MAX_EXPR_VARS][REPL_PREDEF_NAME_MAX];
    float args[MAX_EXPR_VARS];
    int   call_src_cmd_idx;
} ReplReplayPathRung;

typedef struct {
    ReplReplayPathLoop loops[MAX_EXPR_VARS];
    int                loop_count;
    ReplReplayPathRung rungs[REPL_REPLAY_PATH_RUNG_MAX];
    int                rung_count;
    int                overflow; /* frame table latched; chain may be incomplete */
    int                valid;    /* 1 when loops or rungs are present */
} ReplReplayPathSnapshot;

typedef struct {
    int                      after_line_idx;
    ReplReplayAnnotationKind kind;
    char                     text[REPL_REPLAY_ANNOTATION_TEXT_MAX];
    char                     aux[REPL_REPLAY_ANNOTATION_AUX_MAX];  /* "" when unused */
} ReplReplayAnnotation;

/* At most PATH + SUBST + EVAL = 3 annotations per replay frame, anchored
 * to the source line currently under the program counter. */
#ifndef REPL_REPLAY_ANNOTATION_MAX
#define REPL_REPLAY_ANNOTATION_MAX 3
#endif

typedef struct {
    ReplReplayAnnotation     items[REPL_REPLAY_ANNOTATION_MAX];
    int                      count;
    ReplReplayPathSnapshot   path;  /* filled when a PATH row is present */
} ReplReplayAnnotationOutput;

/* Build the annotation list for the current replay frame.
 *
 * `text` is the source-text view the caller assembled this frame; the
 * module caches it internally so file-private helpers can read source
 * text without reaching back into globals. `out` is required (the
 * function memsets it to a clean state on entry). `out->count == 0`
 * means "no annotations apply" (replay is off, the line has no vars,
 * etc.). */
void replay_annotations_prepare(SourceTextView text,
                                     ReplReplayAnnotationOutput *out);

/* Get the inline annotated display text for a source line during replay
 * (the source body, without the extra annotation rows). Writes up to
 * out_size bytes into out. `text` is the source-text view the caller
 * built for this frame. */
int  replay_code_panel_get_command_display_text(SourceTextView text,
                                                     int cmd_idx,
                                                     char *out, int out_size);

#endif /* REPL_REPLAY_ANNOTATIONS_H */
