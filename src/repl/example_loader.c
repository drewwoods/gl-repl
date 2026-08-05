/*
 * src/repl/example_loader.c -- Built-in example loading and metadata handling.
 */
#include "repl/load.h"           /* repl_load_apply_line */
#include "repl/export.h"         /* ReplExportCameraBridge */
#include "repl/command_store.h"
#include "repl/examples.h"
#include "repl/scenes.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "repl/example_loader.h"
#include "repl/host_effects.h"
#include "repl/state_notify.h"
#include "repl/state_owners.h"
#include "repl/text_helpers.h"
#include "source_document.h"     /* source_document_clear */
#include "support/cpuprof.h"     /* prof_histogram_reset on example switch */
#include "config.h"              /* REPL_DIAG_TEXT_MAX */
#include "repl/camera_header.h"
#include "repl/doc_order.h"

static void example_load_error(const char *msg);

/* Route one camera-reader diagnostic to the loader's status sink. The reader
 * is neutral code and knows neither the file name nor the severity policy, so
 * it hands back a rule and the caller writes the sentence. */
static void example_camera_diag(void *userdata, const ReplCameraDiag *diag,
                                const char *rule_text) {
    const char *name = (const char *)userdata;
    char msg[REPL_DIAG_TEXT_MAX];

    if (diag->rule == REPL_CAMERA_RULE_MISSING_ROLE)
        return;   /* collapsed into one message at finish() */
    snprintf(msg, sizeof(msg), "%s:%d: %s",
             name ? name : "example", diag->line_no, rule_text);
    if (repl_camera_rule_severity(diag->rule) == REPL_CAMERA_SEVERITY_REJECTION)
        example_load_error(msg);
    else
        repl_set_status(msg);
}

/* Route one document-order violation to the loader's status sink. Every
 * violation in the file is reported in one pass - a loader that stopped at
 * the first would turn a migration into one edit-reload cycle per line. */
static void example_order_diag(void *userdata, ReplDocOrderRule rule,
                               int line_no, int conflict_line_no,
                               const char *message) {
    const char *name = (const char *)userdata;
    char msg[REPL_DIAG_TEXT_MAX];

    (void)rule;
    (void)conflict_line_no;
    snprintf(msg, sizeof(msg), "%s:%d: %s", name ? name : "example",
             line_no, message);
    example_load_error(msg);
}

/* One message naming every missing pose role together. The accumulator keeps
 * one diagnostic per role - that is what a test can compare - but three
 * separate warnings on screen for one deliberate choice would make the common
 * hand-authored partial header the noisiest case. */
static void example_camera_report_missing(const ReplCameraFinish *fin,
                                          const char *name) {
    char roles[64];
    char msg[REPL_DIAG_TEXT_MAX];

    if (!repl_camera_header_format_missing_roles(fin, roles, sizeof(roles)))
        return;
    snprintf(msg, sizeof(msg), "%s: camera header: %s not set; keeping current",
             name ? name : "example", roles);
    repl_set_status(msg);
}

static void reset_example_presentation_defaults(int example_idx) {
    /* Presentation state lives in glr_state.c; the
     * controller-installed sink does the actual reset. The demo
     * leaves the sink unset and ships without example presentation
     * resets, which is fine because the demo doesn't load examples
     * via this loader.
     *
     * `example_idx` is forwarded opaquely to the host; the loader does
     * not interpret tags itself. The controller layers tag-specific
     * `@cfg` defaults (looked up by example index) on top of the global
     * reset before the example's own `@cfg` metadata is consumed below. */
    repl_dispatch_example_presentation_reset(example_idx);
}

static int example_cfg_extract_slug(const char *text,
                                    char *slug, int slug_sz) {
    const char *end_p = NULL;
    if (!repl_config_extract_slug(text, slug, (size_t)slug_sz, &end_p))
        return 0;
    while (*end_p && isspace((unsigned char)*end_p))
        end_p++;
    return *end_p == '=';
}

#include "repl/cfg_baseline.h"

static int example_cfg_slug_allowed(const char *slug) {
    const ReplConfigBridge *bridge = repl_config_bridge();
    return bridge && bridge->slug_is_scene_subset
        ? bridge->slug_is_scene_subset(slug)
        : 0;
}

static int consume_example_cfg_header(const char *const *lines) {
    int count = 0;

    while (lines && lines[count]) {
        char slug[32];

        if (!example_cfg_extract_slug(lines[count], slug, sizeof(slug)))
            break;
        if (example_cfg_slug_allowed(slug))
            repl_state_parse_workspace_header_line(lines[count]);
        count++;
    }

    return count;
}

/* ----- Body emission ----- */

static void example_load_error(const char *msg) {
    repl_set_status_error(msg);
    fprintf(stderr, "%s\n", msg);
}

static int apply_example_body_line(const char *line,
                                   int body_line_idx,
                                   int *edit_line_inout) {
    char err[REPL_DIAG_TEXT_MAX] = "";

    if (!repl_load_apply_line(line, err, sizeof(err), edit_line_inout)) {
        char msg[REPL_DIAG_TEXT_MAX];
        snprintf(msg, sizeof(msg),
                 "Example load failed at body line %d: %s",
                 body_line_idx + 1, err[0] ? err : "parse error");
        example_load_error(msg);
        return 0;
    }
    return 1;
}

static void reset_example_load_state(int example_idx) {
    ReplCommandStore store = repl_command_store_live();

    /* Ask the host to restore tutorial-mutated cfg before the example
     * overwrites presentation cfg. The standalone demo leaves this bridge
     * unset because it never starts tutorials. */
    repl_dispatch_tutorial_teardown();
    repl_command_store_load(&store, NULL, 0);
    source_document_clear();
    repl_state_flat_program_set_count(0);
    /* Editor-input cleanup (insert mode off, input buffer wipe, cursor
     * home, pending newline clear) routes through the controller-
     * installed sink so the REPL pipeline doesn't reach into
     * EditorState. */
    repl_dispatch_input_reset();
    /* Editor-side transient reset (camera drag / menu / picker /
     * code-panel-drag) remains the controller's responsibility; the
     * example-cycle and example-load handlers perform that reset. */
    repl_eval_init_predef_vars();
    /* Examples use bare funcN; clear any user-aliased names from the
     * outgoing scene so funcN free-slot allocation starts fresh. */
    repl_func_alias_clear_all();
    reset_example_presentation_defaults(example_idx);
}

static int load_example_lines(const char *const *lines,
                              int example_idx) {
    ReplCameraHeader camera;
    ReplCameraFinish camera_fin;
    ReplDocOrder     order;
    const char *name = repl_example_name(example_idx);
    int cfg_count = 0;
    int loader_edit_line = 0;
    int order_failed = 0;
    int body_lines = 0;
    int line_idx;

    reset_example_load_state(example_idx);

    if (lines) {
        cfg_count = consume_example_cfg_header(lines);
    }
    /* Drain the @cfg accumulator: the leading example metadata is
     * parsed into the bag by parse_workspace_header_line; the bridge
     * applies it to live state. */
    repl_export_apply_pending_cfg();

    /* Offer from index 0 - the @cfg header and the metadata blanks
     * included. The reader counts braces from what it is offered to know
     * which region a tag sits in, so a caller that filters first
     * desynchronizes it. Those lines carry no braces today, which is
     * exactly the kind of accidental correctness this format removes.
     *
     * A .glr catalog entry's array *is* the file's lines, so a 1-based
     * array index and a file line number are the same number - which is
     * what lets the differential test compare line numbers across the
     * catalog and file paths. */
    repl_camera_header_init(&camera);
    repl_camera_header_set_sink(&camera, example_camera_diag, (void *)name);
    repl_doc_order_init(&order);
    repl_doc_order_set_sink(&order, example_order_diag, (void *)name);

    for (line_idx = 0; lines && lines[line_idx]; line_idx++) {
        ReplCameraLineResult result =
            repl_camera_header_offer(&camera, lines[line_idx], line_idx + 1);

        /* The canonical order covers user code, not just camera rows, so the
         * loader owns it: the reader owns camera-line rules, the loader owns
         * document shape. Rejection, not a warning - the whole point of a
         * canonical form is that non-canonical files do not accumulate. */
        if (!repl_doc_order_offer(&order, lines[line_idx], line_idx + 1,
                                  result != REPL_CAMERA_LINE_NOT_CAMERA))
            order_failed = 1;

        if (line_idx < cfg_count)
            continue;                     /* metadata, already consumed */
        if (result != REPL_CAMERA_LINE_NOT_CAMERA)
            continue;                     /* camera row: consumed either way */
        /* Everything else is document content, blank rows included. The
         * loader used to eat the blank run between the @cfg header and the
         * body, which the file path keeps - so the same scene had one more
         * row when opened as a file than when loaded from the catalog. Source
         * order in, source order out, on both paths. */
        /* The cap is a *body* budget: metadata and camera rows are consumed
         * before this point, so counting the raw source index would let a
         * scene fail merely for carrying @cfg rows. */
        if (body_lines++ >= EXAMPLE_BODY_LINES_MAX) {
            char msg[REPL_DIAG_TEXT_MAX];
            snprintf(msg, sizeof(msg),
                     "Example load failed: body exceeds EXAMPLE_BODY_LINES_MAX=%d",
                     EXAMPLE_BODY_LINES_MAX);
            example_load_error(msg);
            reset_example_load_state(example_idx);
            repl_dispatch_input_reset();
            repl_mark_source_dirty();
            return 0;
        }
        if (!apply_example_body_line(lines[line_idx], line_idx,
                                     &loader_edit_line)) {
            reset_example_load_state(example_idx);
            repl_dispatch_input_reset();
            repl_mark_source_dirty();
            return 0;
        }
    }

    if (order_failed) {
        reset_example_load_state(example_idx);
        repl_dispatch_input_reset();
        repl_mark_source_dirty();
        return 0;
    }

    /* Nothing was applied while reading: the merged - now complete - pose
     * reaches the bridge exactly once, so a partial header is
     * unrepresentable rather than half-applied. */
    camera_fin = repl_camera_header_finish(&camera, REPL_CAMERA_APPLY_EXAMPLE);
    example_camera_report_missing(&camera_fin, name);

    /* Post-load editor cleanup mirrors the pre-load sink dispatch so a
     * stale input line or cursor doesn't survive the loaded body. */
    repl_dispatch_input_reset();
    repl_mark_source_dirty();
    /* Return the canonical post-load cursor: document end. The
     * caller applies this via editor_state_edit_line_set(). */
    return repl_state_document_count();
}

static int load_example_c_source(const char *const *lines,
                                 int example_idx,
                                 const char *name) {
    reset_example_load_state(example_idx);

    int ok = repl_export_load_from_lines(lines, name ? name : "example.c", NULL);
    repl_dispatch_input_reset();
    repl_mark_source_dirty();
    return ok ? repl_state_document_count() : 0;
}

static int load_example(int idx) {
    int count = repl_example_count();
    const char *const *lines;
    const char *name;

    if (idx < 0 || idx >= count) return 0;
    lines = repl_example_lines(idx);
    name = repl_example_name(idx);
    if (!lines || !name) return 0;

    /* Preserve the active user scene, if any, before overwriting live state
     * with an example. Browsing examples does not create a user-scene slot;
     * editing an example promotes it later through repl_promote_transient_if_needed. */
    repl_scenes_save_active_scene_if_any();
    /* Snapshot the user's pre-example presentation cfg so the next
     * transition out of example state can roll back any in-example
     * cfg toggles before applying the destination's saved cfg. */
    repl_scenes_capture_pre_example_cfg_if_entering();

    int new_edit_line;
    if (repl_example_source_format(idx) == REPL_EXAMPLE_SOURCE_C)
        new_edit_line = load_example_c_source(lines, idx, name);
    else
        new_edit_line = load_example_lines(lines, idx);
    if (new_edit_line <= 0) {
        /* The loader has already cleared the live document. Do not leave the
         * old user-scene slot marked active: a cycle may try another example,
         * whose pre-load save would otherwise overwrite that slot with the
         * failed, empty document. The original slot remains intact because
         * the save at the top of this function ran before the wipe. */
        repl_scenes_mark_example_active();
        return 0;
    }
    repl_state_scenes_set_active_example_idx(idx);
    repl_scenes_mark_example_active();
    char msg[REPL_DIAG_TEXT_MAX];
    snprintf(msg, sizeof(msg), "Example %d/%d: %s (F12 for next)",
             idx + 1, count, name);
    repl_set_status(msg);
    return new_edit_line;
}

int repl_load_example(int idx) {
    int edit_line = load_example(idx);
    if (edit_line > 0) {
        /* The timing histograms are cumulative and describe the *previous*
         * example's geometry - a heavy scene's spread would haunt a light one
         * for the rest of the session. The EMAs re-converge on their own, so
         * only the histograms need clearing. Done here rather than in the
         * callers because every example path (menu, F12 cycle, --example,
         * startup bootstrap) funnels through this entry point. */
        prof_histogram_reset();
    }
    return edit_line;
}

int repl_load_example_lines(const char *const *lines) {
    /* A caller holding only scene text (test, bench, repl_live_demo) has no
     * example-index context, so pass -1 - the controller's reset
     * still applies global defaults, just no tag-default overrides. */
    return load_example_lines(lines, -1);
}
