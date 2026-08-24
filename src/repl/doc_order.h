/*
 * src/repl/doc_order.h - The canonical `.glr` document order, enforced.
 *
 * The format admits exactly one shape. Comments and blank lines go anywhere,
 * as in C89; everything else falls in four phases, and the phase index may
 * never decrease:
 *
 *     1 DECLS    `static float ...;` / `float ...;` at top level
 *     2 FUNCS    funcN(...) { ... } and named function definitions
 *                 then `display() {` when any function exists
 *     3 CAMERA   `@camera`-tagged transforms inside display()
 *     4 BODY     everything else the user wrote inside display()
 *
 * This is the exported C's own order - file-scope globals, then functions,
 * then the camera at the top of display() - so the two halves of one format
 * cannot disagree about line placement. It is a phase machine rather than a
 * list of forbidden pairs: a monotonic counter decides every case by
 * construction instead of inviting an argument about each one.
 *
 * **The phase machine runs on `.glr` only.** An exported `.c` is generated
 * output whose layout the exporter fixes, and that layout does not satisfy
 * these phases - reshape() and main() are emitted after display(), so a
 * machine that classified every line would reject every file this project
 * writes. A generated `.c` is canonical by construction and a hand-edited one
 * is already outside the format's guarantees.
 *
 * The camera *reader* is unaffected and works on both; only this checker is
 * `.glr`-scoped, which is why it lives here and not in camera_header.c: the
 * reader owns camera-line rules, the loader owns document shape.
 *
 * **Every loader runs it, or the format has two readings again.** The catalog
 * loader, the file importer (gated on a `.glr` source), the tutorial
 * setup-scaffold loader and `--lint-scenes` all offer their lines here. A
 * contract enforced at one entry point and not another is the same defect
 * this format exists to end, just relocated.
 */
#ifndef REPL_DOC_ORDER_H
#define REPL_DOC_ORDER_H

typedef enum {
    REPL_DOC_PHASE_NONE = 0,   /* comment or blank: no phase, legal anywhere */
    REPL_DOC_PHASE_DECLS,
    REPL_DOC_PHASE_FUNCS,
    REPL_DOC_PHASE_CAMERA,
    REPL_DOC_PHASE_BODY
} ReplDocPhase;

typedef enum {
    REPL_DOC_ORDER_OK = 0,
    REPL_DOC_ORDER_DECL_LATE,     /* declaration after functions/camera/body */
    REPL_DOC_ORDER_FUNC_LATE,     /* function definition after body code */
    REPL_DOC_ORDER_CAMERA_LATE,   /* camera row after body code */
    REPL_DOC_ORDER_DISPLAY_REQUIRED,
    REPL_DOC_ORDER_DISPLAY_UNEXPECTED,
    REPL_DOC_ORDER_DISPLAY_UNCLOSED,
    REPL_DOC_ORDER_CONTENT_AFTER_DISPLAY
} ReplDocOrderRule;

/* An ordering violation is a relationship between two places, so the report
 * names both: the line that broke the rule and the line that established the
 * phase it broke. Reporting only the first makes the author hunt for the
 * other, and 43 files' worth of hunting is most of a migration's cost. */
typedef void (*ReplDocOrderSink)(void *userdata, ReplDocOrderRule rule,
                                 int line_no, int conflict_line_no,
                                 const char *message);

typedef struct {
    ReplDocPhase     phase;              /* highest phase entered so far */
    int              phase_line[5];      /* line that established each phase */
    int              depth;              /* raw brace depth */
    int              in_block_comment;
    int              pending_line;       /* split-brace header awaiting its { */
    int              saw_function;
    int              display_open_line;
    int              display_close_line;
    int              in_display;
    int              display_required_reported;
    int              last_line_was_wrapper;
    int              violations;
    ReplDocOrderSink sink;
    void            *sink_userdata;
} ReplDocOrder;

void repl_doc_order_init(ReplDocOrder *ord);
void repl_doc_order_set_sink(ReplDocOrder *ord, ReplDocOrderSink sink,
                             void *userdata);

/* Offer one line, in order, exactly as it appears in the file.
 * `is_camera_row` is the camera reader's verdict for this line (non-zero when
 * repl_camera_header_offer consumed it), because only the reader knows a tag
 * when it sees one. Returns 1 when the line is in phase, 0 on a violation -
 * which is *reported and counted*, and the scan continues, so one pass
 * produces the whole worklist instead of one error per reload. */
int repl_doc_order_offer(ReplDocOrder *ord, const char *line, int line_no,
                         int is_camera_row);

/* Finish the stream-level checks that cannot be decided until EOF: a
 * function-only document still needs an explicit empty display frame, and an
 * opened frame must close. Returns 1 when the complete document is valid. */
int repl_doc_order_finish(ReplDocOrder *ord, int eof_line_no);

/* Shared lexical recognition for loaders. Wrapper rows are format syntax,
 * not editor commands; callers offer them to the order checker and camera
 * reader, then skip their ordinary compile/feed path. */
int repl_doc_order_line_is_display_open(const char *line);
int repl_doc_order_last_line_was_wrapper(const ReplDocOrder *ord);

const char *repl_doc_order_rule_text(ReplDocOrderRule rule);

#endif /* REPL_DOC_ORDER_H */
