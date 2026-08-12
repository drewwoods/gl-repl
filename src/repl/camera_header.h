/*
 * src/repl/camera_header.h - The one camera-header reader.
 *
 * A scene's camera is carried by ordinary GL transform lines that
 * self-identify through a trailing role tag:
 *
 *     glTranslatef(0.0000f, 0.0000f, -10.0000f);   // @camera dist
 *     glRotatef(15.0000f, 1.0f, 0.0f, 0.0f);       // @camera rx
 *     glRotatef(20.0000f, 0.0f, 1.0f, 0.0f);       // @camera ry
 *     glRotatef(g_angle, 0.0f, 1.0f, 0.0f);        // @camera spin  (.c only)
 *     glTranslatef(-0.0000f, 2.5000f, -0.0000f);   // @camera pan
 *
 * Exported C rewrites the `//` comments into C89 block comments, so the
 * same block reads `@camera dist` inside a block comment there; both
 * spellings are accepted.
 *
 * The tag - never a line's position, never a `// camera` marker, never a
 * parser state variable - is what makes a line a camera line. `// camera`
 * is an ordinary comment this reader never inspects.
 *
 * Every consumer (file import, catalog/example load, tutorial setup
 * scaffolds, the standalone demo) offers every line, in order, to one
 * ReplCameraHeader and applies nothing until repl_camera_header_finish().
 * Offering *every* line is a contract, not an optimisation target: the
 * reader counts braces from what it is offered to know which region a tag
 * sits in, and a caller that pre-filters silently mis-scopes.
 */
#ifndef REPL_CAMERA_HEADER_H
#define REPL_CAMERA_HEADER_H

#include <stddef.h>

/* Resolved camera pose. Field-for-field the orbit camera's own state:
 * `dist` is the eye distance (the dist line writes -dist), rx/ry the
 * pitch/yaw in degrees, tx/ty/tz the pan target (the pan line writes
 * the negated components). */
typedef struct {
    float dist;
    float rx;
    float ry;
    float tx;
    float ty;
    float tz;
} ReplCameraPose;

/* What a resolved pose is applied *for*. Two independent axes -
 * transition and scene default - that used to be side effects of which
 * parser ran, made explicit so they cannot drift apart again.
 *
 *   IMPORT   snap, adopt as scene default   file / workspace load
 *   RESTORE  snap, leave scene default      scene snapshot restore
 *   EXAMPLE  ease, adopt as scene default   example / tutorial scaffold
 *
 * The fourth combination (ease without adopting) has no caller and is
 * deliberately not defined. */
typedef enum {
    REPL_CAMERA_APPLY_IMPORT = 0,
    REPL_CAMERA_APPLY_RESTORE,
    REPL_CAMERA_APPLY_EXAMPLE
} ReplCameraApplyMode;

/* Roles, in canonical emit order. `spin` is write-only: it carries no
 * pose data (its argument is the exported C's `g_angle` animation hook),
 * is accepted and discarded, and never counts toward the seen mask or the
 * missing-role notes. It never appears in a `.glr`. */
typedef enum {
    REPL_CAMERA_ROLE_NONE = 0,
    REPL_CAMERA_ROLE_DIST,
    REPL_CAMERA_ROLE_RX,
    REPL_CAMERA_ROLE_RY,
    REPL_CAMERA_ROLE_SPIN,
    REPL_CAMERA_ROLE_PAN,
    REPL_CAMERA_ROLE_COUNT
} ReplCameraRole;

/* Seen-mask bits. Pose roles only - `spin` has no bit. */
#define REPL_CAMERA_MASK(role) (1u << (unsigned)(role))
#define REPL_CAMERA_MASK_POSE                                   \
    (REPL_CAMERA_MASK(REPL_CAMERA_ROLE_DIST) |                  \
     REPL_CAMERA_MASK(REPL_CAMERA_ROLE_RX)   |                  \
     REPL_CAMERA_MASK(REPL_CAMERA_ROLE_RY)   |                  \
     REPL_CAMERA_MASK(REPL_CAMERA_ROLE_PAN))

/* Every rule the reader can report. The rule fully determines severity
 * (repl_camera_rule_severity) so nothing carries a settable severity
 * field that two paths could disagree about. */
typedef enum {
    REPL_CAMERA_RULE_NONE = 0,
    REPL_CAMERA_RULE_UNKNOWN_ROLE,      /* `@camera <word>` names no role */
    REPL_CAMERA_RULE_WRONG_CALL,        /* role wants glTranslatef/glRotatef */
    REPL_CAMERA_RULE_NON_LITERAL_ARG,   /* `20.0f * t`, an identifier, ... */
    REPL_CAMERA_RULE_DIST_OFFSET,       /* dist line's x/y are not zero */
    REPL_CAMERA_RULE_AXIS_MISMATCH,     /* rotate axis is not the role's */
    REPL_CAMERA_RULE_TRAILING_TEXT,     /* code after the call's `);` */
    REPL_CAMERA_RULE_SPIN_ARG,          /* spin's argument is not `g_angle` */
    REPL_CAMERA_RULE_DUPLICATE_ROLE,    /* role already supplied; first wins */
    REPL_CAMERA_RULE_ROLE_ORDER,        /* out of dist, rx, ry, spin, pan */
    REPL_CAMERA_RULE_SPLIT_BY_CODE,     /* tag after user code has started */
    REPL_CAMERA_RULE_NESTED_DEPTH,      /* tag below the region's baseline */
    REPL_CAMERA_RULE_IN_SNIPPET,        /* tag inside a snippet region */
    REPL_CAMERA_RULE_AFTER_SNIPPET,     /* tag past the snippet region */
    REPL_CAMERA_RULE_REGION_NOT_OPEN,   /* display region never opened a brace */
    REPL_CAMERA_RULE_MISSING_ROLE,      /* note: merged from the baseline */
    REPL_CAMERA_RULE_G_ANGLE_INIT,      /* warning: importer-local, see import.c */
    REPL_CAMERA_RULE_COUNT
} ReplCameraRule;

typedef enum {
    REPL_CAMERA_SEVERITY_NOTE = 0,
    REPL_CAMERA_SEVERITY_WARNING,
    REPL_CAMERA_SEVERITY_REJECTION
} ReplCameraSeverity;

/* (role, rule, line_no) is the whole comparable key - the differential
 * test compares ordered lists of these plus the overflow count. */
typedef struct {
    ReplCameraRole role;
    ReplCameraRule rule;
    int            line_no;
} ReplCameraDiag;

/* A per-load inspection cache, not a bound on the diagnostic stream:
 * every diagnostic still reaches the sink as it happens. */
#define REPL_CAMERA_MAX_DIAGS 8

typedef enum {
    REPL_CAMERA_LINE_NOT_CAMERA = 0,  /* untagged - caller handles as usual */
    REPL_CAMERA_LINE_ACCEPTED,        /* consumed, role accumulated */
    REPL_CAMERA_LINE_REJECTED         /* consumed, do NOT insert; see diags */
} ReplCameraLineResult;

/* Which region the offered lines are currently in. Only the importer ever
 * signals anything but FILE; the example and tutorial loaders are always
 * at baseline depth 0. */
typedef enum {
    REPL_CAMERA_REGION_FILE = 0,   /* file scope: baseline depth 0 */
    REPL_CAMERA_REGION_DISPLAY,    /* inside display(): baseline captured */
    REPL_CAMERA_REGION_SNIPPET,    /* inside a snippet: tags rejected */
    REPL_CAMERA_REGION_POST_SNIPPET
} ReplCameraRegion;

typedef void (*ReplCameraDiagSink)(void *userdata,
                                   const ReplCameraDiag *diag,
                                   const char *rule_text);

typedef struct {
    ReplCameraPose   pose;         /* roles seen so far, unmerged */
    unsigned         seen_mask;    /* pose roles only */
    int              spin_seen;
    ReplCameraRole   last_role;    /* canonical-order watermark */
    int              body_started; /* executable line seen after a tag */

    int              depth;            /* brace depth of the next line */
    int              in_block_comment; /* block comment open across lines */
    ReplCameraRegion region;
    int              baseline;
    int              baseline_pending;

    ReplCameraDiag   diags[REPL_CAMERA_MAX_DIAGS];
    int              diag_count;
    int              diag_overflow;

    ReplCameraDiagSink sink;
    void              *sink_userdata;
} ReplCameraHeader;

/* Explicit outputs, so nothing is communicated by side effect alone.
 * `pose` is populated whether or not a bridge was installed, which is
 * what lets a bridge-less test assert the resolved pose directly. */
typedef struct {
    int            pose_applied;
    ReplCameraPose pose;
    unsigned       seen_mask;
    int            diag_count;
    int            diag_overflow;
} ReplCameraFinish;

/* Format the missing pose roles from a completed header for a loader-level
 * diagnostic. Returns 1 when at least one pose role was omitted, 0 when the
 * header carried no pose or all four pose roles. The caller supplies the
 * surrounding source label because the reader does not know file names. */
int repl_camera_header_format_missing_roles(const ReplCameraFinish *finish,
                                            char *out, size_t out_size);

void repl_camera_header_init(ReplCameraHeader *hdr);
void repl_camera_header_set_sink(ReplCameraHeader *hdr,
                                 ReplCameraDiagSink sink, void *userdata);

/* Signal a region transition. Call on the line that opens display() or
 * the snippet; the display baseline is captured lazily at the first
 * offered line deeper than 0, so a `{` on its own line still works. */
void repl_camera_header_set_region(ReplCameraHeader *hdr,
                                   ReplCameraRegion region);

/* Offer one line. `line_no` is the caller's own numbering (1-based file
 * or array line; 0 when it genuinely has none) and is only stamped onto
 * diagnostics - the reader never interprets it. */
ReplCameraLineResult repl_camera_header_offer(ReplCameraHeader *hdr,
                                              const char *line,
                                              int line_no);

/* Resolve the accumulated roles into a complete pose and apply it
 * through the installed camera bridge. No pose role seen -> no bridge
 * call at all; some seen -> the bridge's destination pose is captured
 * and the seen roles overwrite it, with one note per missing role. */
ReplCameraFinish repl_camera_header_finish(ReplCameraHeader *hdr,
                                           ReplCameraApplyMode mode);

/* Record a diagnostic the reader could not have seen itself (the
 * importer's non-zero `g_angle` initializer warning). */
void repl_camera_header_record(ReplCameraHeader *hdr, ReplCameraRole role,
                               ReplCameraRule rule, int line_no);

ReplCameraSeverity repl_camera_rule_severity(ReplCameraRule rule);
const char        *repl_camera_rule_text(ReplCameraRule rule);
const char        *repl_camera_role_name(ReplCameraRole role);

/* Net { - } over a line's code portion, ignoring braces inside string and
 * char literals, a `//` line comment, and block comments.
 * Block-comment state must survive across lines, so it is an in/out
 * parameter rather than a static: two consumers with independent state is
 * the whole point. `in_block_comment` must not be NULL - a caller that
 * opts out of block-comment tracking is a caller that mis-scopes. */
int repl_code_brace_delta(const char *line, int *in_block_comment);

/* Is `line` executable code - any non-whitespace outside a comment that
 * is not a preprocessor directive? `in_block_comment` is read, not
 * written (use repl_code_brace_delta to advance it). */
int repl_line_is_executable(const char *line, int in_block_comment);

#endif /* REPL_CAMERA_HEADER_H */
