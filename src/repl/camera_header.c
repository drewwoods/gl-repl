/*
 * src/repl/camera_header.c - The one camera-header reader.
 *
 * See camera_header.h for the format and the caller contract. This TU owns
 * tag recognition, brace/region tracking, per-role parsing, validation,
 * diagnostics, and the deferred pose merge. It knows nothing about files,
 * line numbers, or the app - callers supply the numbering and the bridge
 * supplies the camera.
 */
#include "repl/camera_header.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "repl/export.h"   /* ReplExportCameraBridge */

/* ----- Line scanning ---------------------------------------------------- */

/* One pass over a line, tracking string/char literals and both comment
 * forms. Reports the net brace delta over the code portion, whether the
 * line holds executable code, and the first `@camera` occurrence inside a
 * comment. `in_block_comment` is in/out so a block comment can span lines.
 *
 * A brace inside a comment or a string literal must not move the depth
 * counter: the counter is what decides whether a tag sits at its region's
 * baseline, so a brace inside a block comment would otherwise mis-scope
 * every tag below it. */
static void cam_scan_line(const char *line, int *in_block_comment,
                          int *out_delta, int *out_has_code,
                          const char **out_tag) {
    int in_str = 0, in_chr = 0;
    int delta = 0, has_code = 0;
    const char *tag = NULL;
    int i;

    if (out_delta)    *out_delta = 0;
    if (out_has_code) *out_has_code = 0;
    if (out_tag)      *out_tag = NULL;
    if (!line || !in_block_comment)
        return;

    for (i = 0; line[i]; i++) {
        char c = line[i];

        if (*in_block_comment) {
            if (!tag && c == '@' && strncmp(line + i, "@camera", 7) == 0)
                tag = line + i + 7;
            if (c == '*' && line[i + 1] == '/') {
                *in_block_comment = 0;
                i++;
            }
            continue;
        }
        if (in_str || in_chr) {
            if (c == '\\' && line[i + 1]) { i++; continue; }
            if (in_str && c == '"')  in_str = 0;
            if (in_chr && c == '\'') in_chr = 0;
            continue;
        }
        if (c == '/' && line[i + 1] == '/') {
            if (!tag) {
                const char *at = strstr(line + i, "@camera");
                if (at) tag = at + 7;
            }
            break;              /* rest of the line is comment */
        }
        if (c == '/' && line[i + 1] == '*') {
            *in_block_comment = 1;
            i++;
            continue;
        }
        if (c == '"')  { in_str = 1; has_code = 1; continue; }
        if (c == '\'') { in_chr = 1; has_code = 1; continue; }
        if (c == '{') delta++;
        else if (c == '}') delta--;
        if (!isspace((unsigned char)c))
            has_code = 1;
    }

    /* A preprocessor directive is not executable code for phase purposes -
     * it carries no modelview effect and may legally sit anywhere. */
    if (has_code) {
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#')
            has_code = 0;
    }

    if (out_delta)    *out_delta = delta;
    if (out_has_code) *out_has_code = has_code;
    if (out_tag)      *out_tag = tag;
}

int repl_code_brace_delta(const char *line, int *in_block_comment) {
    int delta = 0;
    if (!in_block_comment)
        return 0;
    cam_scan_line(line, in_block_comment, &delta, NULL, NULL);
    return delta;
}

int repl_line_is_executable(const char *line, int in_block_comment) {
    int has_code = 0;
    int block = in_block_comment;
    cam_scan_line(line, &block, NULL, &has_code, NULL);
    return has_code;
}

/* ----- Role / rule vocabulary ------------------------------------------- */

static const struct {
    ReplCameraRole role;
    const char    *name;
} k_role_names[] = {
    /* role,                  name */
    { REPL_CAMERA_ROLE_DIST, "dist" },
    { REPL_CAMERA_ROLE_RX,   "rx"   },
    { REPL_CAMERA_ROLE_RY,   "ry"   },
    { REPL_CAMERA_ROLE_SPIN, "spin" },
    { REPL_CAMERA_ROLE_PAN,  "pan"  }
};

const char *repl_camera_role_name(ReplCameraRole role) {
    size_t i;
    for (i = 0; i < sizeof(k_role_names) / sizeof(k_role_names[0]); i++)
        if (k_role_names[i].role == role)
            return k_role_names[i].name;
    return "(none)";
}

/* Severity is derived from the rule, never stored: a settable field is how
 * two paths end up "differing" on something neither of them chose. */
ReplCameraSeverity repl_camera_rule_severity(ReplCameraRule rule) {
    switch (rule) {
    case REPL_CAMERA_RULE_MISSING_ROLE:  return REPL_CAMERA_SEVERITY_NOTE;
    case REPL_CAMERA_RULE_G_ANGLE_INIT:  return REPL_CAMERA_SEVERITY_WARNING;
    default:                             return REPL_CAMERA_SEVERITY_REJECTION;
    }
}

/* Each message names the *rule*, in the words the format documentation
 * uses, so searching for the text finds the section that explains it. */
const char *repl_camera_rule_text(ReplCameraRule rule) {
    switch (rule) {
    case REPL_CAMERA_RULE_UNKNOWN_ROLE:
        return "unknown @camera role (expected dist, rx, ry, spin or pan)";
    case REPL_CAMERA_RULE_WRONG_CALL:
        return "camera role is tagged on the wrong call "
               "(dist/pan want glTranslatef, rx/ry/spin want glRotatef)";
    case REPL_CAMERA_RULE_NON_LITERAL_ARG:
        return "camera transform arguments must be plain float literals";
    case REPL_CAMERA_RULE_DIST_OFFSET:
        return "the 'dist' transform must have x = y = 0 "
               "(a pan offset belongs on the 'pan' line)";
    case REPL_CAMERA_RULE_AXIS_MISMATCH:
        return "camera rotate axis does not match its role "
               "(rx is 1,0,0; ry and spin are 0,1,0)";
    case REPL_CAMERA_RULE_TRAILING_TEXT:
        return "a camera line must end after its call";
    case REPL_CAMERA_RULE_SPIN_ARG:
        return "the 'spin' hook's angle argument must be the bare "
               "g_angle identifier";
    case REPL_CAMERA_RULE_DUPLICATE_ROLE:
        return "duplicate @camera role (the first one wins)";
    case REPL_CAMERA_RULE_ROLE_ORDER:
        return "camera roles must appear in the order "
               "dist, rx, ry, spin, pan";
    case REPL_CAMERA_RULE_SPLIT_BY_CODE:
        return "camera row after body code "
               "(user geometry begins only once every camera row is behind it)";
    case REPL_CAMERA_RULE_NESTED_DEPTH:
        return "@camera tag inside a nested block";
    case REPL_CAMERA_RULE_IN_SNIPPET:
        return "@camera tag inside the geometry snippet";
    case REPL_CAMERA_RULE_AFTER_SNIPPET:
        return "@camera tag after the geometry snippet ended";
    case REPL_CAMERA_RULE_REGION_NOT_OPEN:
        return "@camera tag before display() opened its body";
    case REPL_CAMERA_RULE_MISSING_ROLE:
        return "camera role not set; keeping the current value";
    case REPL_CAMERA_RULE_G_ANGLE_INIT:
        return "g_angle's initializer is non-semantic and must stay 0.0f; "
               "the yaw belongs on the '@camera ry' line";
    default:
        return "camera header rule";
    }
}

/* ----- Diagnostics ------------------------------------------------------ */

static void cam_diag(ReplCameraHeader *hdr, ReplCameraRole role,
                     ReplCameraRule rule, int line_no) {
    ReplCameraDiag diag;

    diag.role    = role;
    diag.rule    = rule;
    diag.line_no = line_no;

    /* The stored array is a bounded inspection cache; the sink sees every
     * diagnostic, and the overflow count keeps the truncation comparable. */
    if (hdr->diag_count < REPL_CAMERA_MAX_DIAGS)
        hdr->diags[hdr->diag_count++] = diag;
    else
        hdr->diag_overflow++;

    if (hdr->sink)
        hdr->sink(hdr->sink_userdata, &diag, repl_camera_rule_text(rule));
}

void repl_camera_header_record(ReplCameraHeader *hdr, ReplCameraRole role,
                               ReplCameraRule rule, int line_no) {
    if (!hdr)
        return;
    cam_diag(hdr, role, rule, line_no);
}

const ReplCameraDiag *repl_camera_header_diags(const ReplCameraHeader *hdr) {
    return hdr ? hdr->diags : NULL;
}

/* ----- Per-role call parsing -------------------------------------------- */

static const char *cam_skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int cam_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Match `name(` at p, returning the position just past the paren. */
static const char *cam_match_call(const char *p, const char *name) {
    size_t len = strlen(name);

    p = cam_skip_ws(p);
    if (strncmp(p, name, len) != 0 || cam_ident_char(p[len]))
        return NULL;
    p = cam_skip_ws(p + len);
    if (*p != '(')
        return NULL;
    return p + 1;
}

/* One float literal, optionally `f`-suffixed. Deliberately strict: the
 * argument must end at the following `,` or `)`, so `20.0f * t` is a
 * diagnostic rather than a silently-truncated 20. A leading `-` is part of
 * the literal, not garbage in front of it. */
static const char *cam_read_literal(const char *p, float *out) {
    char *end = NULL;
    float v;

    p = cam_skip_ws(p);
    v = strtof(p, &end);
    if (!end || end == p)
        return NULL;
    p = end;
    if (*p == 'f' || *p == 'F')
        p++;
    *out = v;
    return p;
}

/* After the closing paren: an optional `;`, then nothing but whitespace and
 * comments.
 *
 * A block comment has to be skipped *through*, not merely recognised. The
 * whole line is consumed when a role is accepted, so treating a block-comment
 * opener as end-of-line would let a tagged call followed by a closed comment
 * and then a glClear swallow that glClear - a camera tag quietly eating
 * geometry, which is the failure this format exists to end. An *unterminated*
 * block comment does run to the end of the line, so that one really is the
 * end. */
static int cam_call_ends(const char *p) {
    p = cam_skip_ws(p);
    if (*p == ';')
        p = cam_skip_ws(p + 1);
    for (;;) {
        if (*p == '\0')
            return 1;
        if (p[0] == '/' && p[1] == '/')
            return 1;
        if (p[0] == '/' && p[1] == '*') {
            const char *close = strstr(p + 2, "*/");
            if (!close)
                return 1;           /* runs past end of line */
            p = cam_skip_ws(close + 2);
            continue;
        }
        return 0;
    }
}

/* Read `count` literal arguments and the call's tail. */
static ReplCameraRule cam_read_args(const char *p, float *vals, int count) {
    int i;

    for (i = 0; i < count; i++) {
        p = cam_read_literal(p, &vals[i]);
        if (!p)
            return REPL_CAMERA_RULE_NON_LITERAL_ARG;
        p = cam_skip_ws(p);
        if (i + 1 < count) {
            if (*p != ',')
                return REPL_CAMERA_RULE_NON_LITERAL_ARG;
            p++;
        } else {
            if (*p != ')')
                return REPL_CAMERA_RULE_NON_LITERAL_ARG;
            p++;
        }
    }
    return cam_call_ends(p) ? REPL_CAMERA_RULE_NONE
                            : REPL_CAMERA_RULE_TRAILING_TEXT;
}

static int cam_axis_is(const float *v, float x, float y, float z) {
    return fabsf(v[1] - x) <= 1e-4f &&
           fabsf(v[2] - y) <= 1e-4f &&
           fabsf(v[3] - z) <= 1e-4f;
}

/* The `spin` hook's angle argument, tokenized rather than string-compared:
 * `g_angle`, `( g_angle )` and any whitespace spelling pass, while
 * `g_angle2` and `g_angle + 1` do not. */
static const char *cam_read_g_angle(const char *p) {
    int opens = 0;

    p = cam_skip_ws(p);
    while (*p == '(') { opens++; p = cam_skip_ws(p + 1); }
    if (strncmp(p, "g_angle", 7) != 0 || cam_ident_char(p[7]))
        return NULL;
    p = cam_skip_ws(p + 7);
    while (opens > 0) {
        if (*p != ')')
            return NULL;
        opens--;
        p = cam_skip_ws(p + 1);
    }
    return p;
}

/* Parse one tagged line into `pose` (or, for spin, into nothing at all).
 * Returns REPL_CAMERA_RULE_NONE on success. */
static ReplCameraRule cam_parse_role_line(const char *line,
                                          ReplCameraRole role,
                                          ReplCameraPose *pose) {
    const char *p;
    float v[4];
    ReplCameraRule rule;

    switch (role) {
    case REPL_CAMERA_ROLE_DIST:
        p = cam_match_call(line, "glTranslatef");
        if (!p) return REPL_CAMERA_RULE_WRONG_CALL;
        rule = cam_read_args(p, v, 3);
        if (rule != REPL_CAMERA_RULE_NONE) return rule;
        if (fabsf(v[0]) > 1e-4f || fabsf(v[1]) > 1e-4f)
            return REPL_CAMERA_RULE_DIST_OFFSET;
        pose->dist = -v[2];
        return REPL_CAMERA_RULE_NONE;

    case REPL_CAMERA_ROLE_PAN:
        p = cam_match_call(line, "glTranslatef");
        if (!p) return REPL_CAMERA_RULE_WRONG_CALL;
        rule = cam_read_args(p, v, 3);
        if (rule != REPL_CAMERA_RULE_NONE) return rule;
        pose->tx = -v[0];
        pose->ty = -v[1];
        pose->tz = -v[2];
        return REPL_CAMERA_RULE_NONE;

    case REPL_CAMERA_ROLE_RX:
    case REPL_CAMERA_ROLE_RY:
        p = cam_match_call(line, "glRotatef");
        if (!p) return REPL_CAMERA_RULE_WRONG_CALL;
        rule = cam_read_args(p, v, 4);
        if (rule != REPL_CAMERA_RULE_NONE) return rule;
        if (role == REPL_CAMERA_ROLE_RX) {
            if (!cam_axis_is(v, 1.0f, 0.0f, 0.0f))
                return REPL_CAMERA_RULE_AXIS_MISMATCH;
            pose->rx = v[0];
        } else {
            if (!cam_axis_is(v, 0.0f, 1.0f, 0.0f))
                return REPL_CAMERA_RULE_AXIS_MISMATCH;
            pose->ry = v[0];
        }
        return REPL_CAMERA_RULE_NONE;

    case REPL_CAMERA_ROLE_SPIN:
        /* Write-only: validated, then discarded. A rejected spin is a
         * diagnostic about a line that was going to be thrown away. */
        p = cam_match_call(line, "glRotatef");
        if (!p) return REPL_CAMERA_RULE_WRONG_CALL;
        p = cam_read_g_angle(p);
        if (!p) return REPL_CAMERA_RULE_SPIN_ARG;
        if (*p != ',') return REPL_CAMERA_RULE_SPIN_ARG;
        rule = cam_read_args(p + 1, v, 3);
        if (rule != REPL_CAMERA_RULE_NONE) return rule;
        if (fabsf(v[0]) > 1e-4f || fabsf(v[1] - 1.0f) > 1e-4f ||
            fabsf(v[2]) > 1e-4f)
            return REPL_CAMERA_RULE_AXIS_MISMATCH;
        return REPL_CAMERA_RULE_NONE;

    default:
        return REPL_CAMERA_RULE_UNKNOWN_ROLE;
    }
}

/* The word after `@camera`, or ROLE_NONE when it names nothing. */
static ReplCameraRole cam_tag_role(const char *tag) {
    char word[16];
    size_t n = 0;
    size_t i;

    tag = cam_skip_ws(tag);
    while (n + 1 < sizeof(word) && cam_ident_char(tag[n])) {
        word[n] = tag[n];
        n++;
    }
    word[n] = '\0';
    for (i = 0; i < sizeof(k_role_names) / sizeof(k_role_names[0]); i++)
        if (strcmp(word, k_role_names[i].name) == 0)
            return k_role_names[i].role;
    return REPL_CAMERA_ROLE_NONE;
}

/* ----- Public reader ---------------------------------------------------- */

void repl_camera_header_init(ReplCameraHeader *hdr) {
    if (!hdr)
        return;
    memset(hdr, 0, sizeof(*hdr));
    hdr->region    = REPL_CAMERA_REGION_FILE;
    hdr->last_role = REPL_CAMERA_ROLE_NONE;
}

void repl_camera_header_set_sink(ReplCameraHeader *hdr,
                                 ReplCameraDiagSink sink, void *userdata) {
    if (!hdr)
        return;
    hdr->sink          = sink;
    hdr->sink_userdata = userdata;
}

void repl_camera_header_set_region(ReplCameraHeader *hdr,
                                   ReplCameraRegion region) {
    if (!hdr)
        return;
    hdr->region = region;
    if (region == REPL_CAMERA_REGION_DISPLAY) {
        /* Baseline capture is deferred rather than taken from the opener:
         * a hand-formatted file may put display()'s `{` on its own line, so
         * capturing here would record 0 and reject every line in the body. */
        hdr->baseline_pending = 1;
        hdr->baseline         = 0;
    } else {
        hdr->baseline_pending = 0;
        hdr->baseline         = 0;
    }
}

ReplCameraLineResult repl_camera_header_offer(ReplCameraHeader *hdr,
                                              const char *line,
                                              int line_no) {
    const char *tag = NULL;
    int delta = 0, has_code = 0;
    int depth_at_start;
    ReplCameraRole role;
    ReplCameraRule rule;
    ReplCameraLineResult result = REPL_CAMERA_LINE_NOT_CAMERA;

    if (!hdr || !line)
        return REPL_CAMERA_LINE_NOT_CAMERA;

    depth_at_start = hdr->depth;
    cam_scan_line(line, &hdr->in_block_comment, &delta, &has_code, &tag);

    if (hdr->baseline_pending && depth_at_start > 0) {
        hdr->baseline         = depth_at_start;
        hdr->baseline_pending = 0;
    }

    if (!tag) {
        /* Only code that follows a camera row closes the block; everything
         * ahead of the first tag is ordinary preamble. */
        if (has_code && hdr->last_role != REPL_CAMERA_ROLE_NONE)
            hdr->body_started = 1;
        goto done;
    }

    role = cam_tag_role(tag);

    /* Position is checked before the role vocabulary and before the call
     * shape: a tag in the wrong region is a camera line in the wrong place,
     * and it is consumed either way so it can never land as geometry. */
    if (hdr->region == REPL_CAMERA_REGION_SNIPPET)
        rule = REPL_CAMERA_RULE_IN_SNIPPET;
    else if (hdr->region == REPL_CAMERA_REGION_POST_SNIPPET)
        rule = REPL_CAMERA_RULE_AFTER_SNIPPET;
    else if (hdr->baseline_pending)
        rule = REPL_CAMERA_RULE_REGION_NOT_OPEN;
    else if (depth_at_start != hdr->baseline)
        rule = REPL_CAMERA_RULE_NESTED_DEPTH;
    else if (hdr->body_started)
        rule = REPL_CAMERA_RULE_SPLIT_BY_CODE;
    else if (role == REPL_CAMERA_ROLE_NONE)
        rule = REPL_CAMERA_RULE_UNKNOWN_ROLE;
    else if (role == REPL_CAMERA_ROLE_SPIN
                 ? hdr->spin_seen
                 : (hdr->seen_mask & REPL_CAMERA_MASK(role)) != 0)
        rule = REPL_CAMERA_RULE_DUPLICATE_ROLE;
    else if (role < hdr->last_role)
        rule = REPL_CAMERA_RULE_ROLE_ORDER;
    else
        rule = cam_parse_role_line(line, role, &hdr->pose);

    if (rule != REPL_CAMERA_RULE_NONE) {
        cam_diag(hdr, role, rule, line_no);
        result = REPL_CAMERA_LINE_REJECTED;
        goto done;
    }

    if (role == REPL_CAMERA_ROLE_SPIN)
        hdr->spin_seen = 1;
    else
        hdr->seen_mask |= REPL_CAMERA_MASK(role);
    hdr->last_role = role;
    result = REPL_CAMERA_LINE_ACCEPTED;

done:
    hdr->depth = depth_at_start + delta;
    /* Region exit is evaluated after the line's brace delta, so display()'s
     * own closing brace is outside the region. Without this a later function
     * body at the same raw depth would match a stale baseline. */
    if (hdr->region == REPL_CAMERA_REGION_DISPLAY &&
        !hdr->baseline_pending && hdr->depth < hdr->baseline) {
        hdr->region   = REPL_CAMERA_REGION_FILE;
        hdr->baseline = 0;
    }
    return result;
}

ReplCameraFinish repl_camera_header_finish(ReplCameraHeader *hdr,
                                           ReplCameraApplyMode mode) {
    ReplCameraFinish out;
    const ReplExportCameraBridge *bridge;
    ReplCameraPose merged;
    int role;

    memset(&out, 0, sizeof(out));
    if (!hdr)
        return out;

    out.seen_mask     = hdr->seen_mask;
    out.diag_count    = hdr->diag_count;
    out.diag_overflow = hdr->diag_overflow;

    /* No pose role at all: no bridge call, host defaults stand. A file
     * whose only camera row was `spin` carries no pose either. */
    if (hdr->seen_mask == 0)
        return out;

    bridge = repl_export_camera_bridge();

    /* The baseline is the camera's *destination*, not its live pose: a load
     * can land mid-ease, and merging against an interpolated frame would
     * bake an arbitrary step of an animation into the result. */
    memset(&merged, 0, sizeof(merged));
    if (bridge && bridge->capture_pose)
        bridge->capture_pose(&merged);

    if (hdr->seen_mask & REPL_CAMERA_MASK(REPL_CAMERA_ROLE_DIST))
        merged.dist = hdr->pose.dist;
    if (hdr->seen_mask & REPL_CAMERA_MASK(REPL_CAMERA_ROLE_RX))
        merged.rx = hdr->pose.rx;
    if (hdr->seen_mask & REPL_CAMERA_MASK(REPL_CAMERA_ROLE_RY))
        merged.ry = hdr->pose.ry;
    if (hdr->seen_mask & REPL_CAMERA_MASK(REPL_CAMERA_ROLE_PAN)) {
        merged.tx = hdr->pose.tx;
        merged.ty = hdr->pose.ty;
        merged.tz = hdr->pose.tz;
    }

    /* One note per missing role - that is what a test can compare. The
     * caller collapses them into a single human-facing message. */
    for (role = REPL_CAMERA_ROLE_DIST; role < REPL_CAMERA_ROLE_COUNT; role++) {
        if (role == REPL_CAMERA_ROLE_SPIN)
            continue;   /* write-only: absence is not a missing role */
        if ((hdr->seen_mask & REPL_CAMERA_MASK(role)) == 0)
            cam_diag(hdr, (ReplCameraRole)role,
                     REPL_CAMERA_RULE_MISSING_ROLE, 0);
    }

    out.pose          = merged;
    out.diag_count    = hdr->diag_count;
    out.diag_overflow = hdr->diag_overflow;

    if (bridge && bridge->apply_pose) {
        bridge->apply_pose(&merged, mode);
        out.pose_applied = 1;
    }
    return out;
}

int repl_camera_header_format_missing_roles(const ReplCameraFinish *finish,
                                            char *out, size_t out_size) {
    unsigned missing;
    size_t used = 0;
    int role;

    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!finish) return 0;

    if (finish->seen_mask == 0)
        return 0;
    missing = REPL_CAMERA_MASK_POSE & ~finish->seen_mask;
    if (!missing) return 0;

    for (role = REPL_CAMERA_ROLE_DIST; role < REPL_CAMERA_ROLE_COUNT; role++) {
        int written;
        if ((REPL_CAMERA_MASK(role) & missing) == 0)
            continue;
        written = snprintf(out + used, out_size - used, "%s%s",
                           used ? ", " : "",
                           repl_camera_role_name((ReplCameraRole)role));
        if (written < 0) {
            out[0] = '\0';
            return 0;
        }
        if ((size_t)written >= out_size - used) {
            out[out_size - 1] = '\0';
            return 1;
        }
        used += (size_t)written;
    }
    return used > 0;
}
