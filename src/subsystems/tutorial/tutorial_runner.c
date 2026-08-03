#include "subsystems/tutorial/tutorial_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <math.h>               /* fabsf for REQUIRE_VAR epsilon check */

#include "repl/host_effects.h"
#include "repl/cfg_baseline.h" /* For repl_cfg_get_int, repl_cfg_set_int, and repl_cfg_known */
#include "repl/eval.h"          /* repl_eval_find_predef_var_idx + predef-vars view */
#include "repl/example_loader.h" /* repl_example_consume_camera_header (setup scaffold) */
#include "repl/load.h"
#include "repl/scenes.h"
#include "repl/state_owners.h"  /* repl_state_scenes_set_tutorial_origin_idx */
#include "repl/state_views.h"
#include "repl/text_helpers.h"  /* repl_extract_label_name (setup goto-label anchors) */
#include "source_document.h"    /* live line text for setup goto-label resolution */
#include "repl/state_notify.h" /* For repl_state_mark_flat_dirty, repl_state_mark_source_dirty, and repl_state_parse_workspace_header_line */
#include "repl/tutorials.h"
#include "repl/export.h"
#include "config.h"            /* REPL_DIAG_TEXT_MAX */
#include "keymap.h"



/* --- cfg baseline snapshot ----------------------------------------------- */

/* Restore-on-exit storage: a bag of (slug, value) pairs the active
 * tutorial promises to restore on teardown. Captured by
 * `tutorial_capture_cfg_baseline()` at `tutorial_start` BEFORE any
 * tutorial machinery (transient-scene entry, presentation reset, entry
 * `@cfg` apply, step SET writes) has touched live cfg - so the baseline
 * is the user's true pre-tutorial state. Written back by
 * `tutorial_teardown()` at every teardown path so a tutorial leaves no
 * scene-presentation footprint.
 *
 * Coverage:
 *   1) Every slug in the bridge's `fill_scene_subset` (the same set
 *      `src/repl/scenes.c::stash_live_state` captures for scene saves)
 *      - so presentation_reset's effects revert too, not just SET/REQUIRE.
 *   2) Plus any extra slugs the tutorial references via entry @cfg or
 *      step SET/REQUIRE (e.g. `view_mode`, which is intentionally OUTSIDE
 *      the scene subset since it isn't a per-scene property).
 *
 * The bag's set_int dedups by slug, so we can union (1) and (2) blindly. */

static void tutorial_cfg_baseline_clear(void) {
    TutorialRuntimeState *state = tutorial_state_mut();
    repl_config_bag_clear(&state->baseline_bag);
    state->baseline_valid = 0;
}

/* Helper: add (slug, current_value) to the baseline bag iff the bridge
 * recognises the slug. The bag's `set_int` already dedups by slug, so
 * unioning the scene-subset capture with tutorial-specific slugs is
 * blind-append-safe. */
static void tutorial_cfg_baseline_record_one(const char *slug) {
    if (!slug || !slug[0])     return;
    if (!repl_cfg_known(slug)) return;
    TutorialRuntimeState *state = tutorial_state_mut();
    repl_config_bag_set_int(&state->baseline_bag, slug,
                               repl_cfg_get_int(slug, 0));
}

/* Snapshot the user's pre-tutorial cfg into the bag. Two coverage
 * sources, unioned via the bag's slug-dedup:
 *   - `fill_scene_subset` (every per-scene presentation slug - so the
 *     tutorial presentation reset, including its CLOSE grid extent, and
 *     any cascading defaults revert too);
 *   - Tutorial-specific slugs referenced by entry `@cfg` or step
 *     SET/REQUIRE (e.g. `view_mode`, which is intentionally outside
 *     the scene subset). */
static void tutorial_baseline_capture(int idx) {
    tutorial_cfg_baseline_clear();
    const ReplConfigBridge *b = repl_config_bridge();
    TutorialRuntimeState *state = tutorial_state_mut();
    if (b && b->fill_scene_subset)
        b->fill_scene_subset(&state->baseline_bag);

    /* The presentation reset (run for every tutorial start) always
     * touches `view_mode`/ortho_mode, but view_mode is
     * intentionally outside the scene-subset (it's a global, not a
     * per-scene property). Without an explicit record, a tutorial whose
     * @cfg / SET steps don't mention view_mode silently leaks the
     * presentation_reset's 3D mode past teardown - e.g. starting "Color &
     * Transform" from 2D would exit in 3D. Capture it unconditionally. */
    tutorial_cfg_baseline_record_one("view_mode");

    char slug[REPL_CFG_KEY_MAX];
    const char *const *cfg = repl_tutorial_cfg_lines(idx);
    for (int i = 0; cfg && cfg[i]; i++) {
        if (repl_config_extract_slug(cfg[i], slug, sizeof slug, NULL))
            tutorial_cfg_baseline_record_one(slug);
    }
    /* The setup scaffold's leading `// @cfg` header uses the same
     * vocabulary and is applied the same way, so its slugs join the
     * restore baseline too. Only the leading contiguous run counts -
     * a later `// @cfg` inside the body is an ordinary comment. */
    const char *const *setup = repl_tutorial_setup_lines(idx);
    for (int i = 0; setup && setup[i] &&
                    repl_config_extract_slug(setup[i], slug,
                                             sizeof slug, NULL); i++)
        tutorial_cfg_baseline_record_one(slug);
    int n = repl_tutorial_step_count(idx);
    for (int s = 0; s < n; s++) {
        TutorialStepKind k = repl_tutorial_step_kind(idx, s);
        if (k == TUTORIAL_STEP_KIND_SET || k == TUTORIAL_STEP_KIND_REQUIRE ||
            k == TUTORIAL_STEP_KIND_SET_QUIET)
            tutorial_cfg_baseline_record_one(repl_tutorial_step_cfg_slug(idx, s));
    }
    state->baseline_valid = 1;
}

static void tutorial_baseline_apply(int idx) {
    /* Reset scene-presentation chrome, camera, and grid extent to the
     * tutorial start state (see `tutorial_presentation_reset` in
     * host_effects.h), then apply any tutorial leading `@cfg` lines on
     * top. No tag mask is involved: tutorial and example tag namespaces
     * are disjoint, so the example tag-default table must not run here. */
    repl_dispatch_tutorial_presentation_reset();

    const char *const *cfg = repl_tutorial_cfg_lines(idx);
    for (int i = 0; cfg && cfg[i]; i++)
        repl_state_parse_workspace_header_line(cfg[i]);
    repl_export_apply_pending_cfg();
}

/* Restore captured baseline configuration. Order sensitivity: tutorial_teardown
 * deactivates the active flag BEFORE this runs, so restoring slug values
 * doesn't trigger state-change auto-advancement mid-teardown. */
static void tutorial_baseline_restore(void) {
    TutorialRuntimeState state = tutorial_state_view();
    if (!state.baseline_valid) return;
    const ReplConfigBridge *b = repl_config_bridge();
    if (b && b->apply)
        b->apply(&state.baseline_bag);
    tutorial_cfg_baseline_clear();
}



/* Canonicalize a tutorial command for shape-only comparison: trim outer
 * whitespace, ignore a trailing semicolon, then remove all remaining
 * whitespace so formatting differences do not affect matching. */


static int tutorial_append_locked_line(int line_idx) {
    TutorialRuntimeState *state = tutorial_state_mut();

    if (line_idx < 0)
        return 0;
    /* Dedup: block steps can nominate a row that is already locked
     * (e.g. a comment-less `}` step's auto-record targets the auto-end
     * row the matching open step locked). A duplicate entry would be
     * harmless for lookups but would double-shift nothing and waste
     * capacity, so skip it. */
    for (int i = 0; i < state->locked_line_count; i++) {
        if (state->locked_lines[i] == line_idx)
            return 1;
    }
    if (state->locked_line_count >= TUTORIAL_LOCKED_LINE_MAX)
        return 0;

    state->locked_lines[state->locked_line_count++] = line_idx;
    return 1;
}

/* Width the status formats reserve for the prefix. It is only non-empty on
 * step 0 ("[n/m] <name> Tutorial: "), and capping it there is what keeps the
 * instruction after it from being the part that gets truncated away. It is
 * also the bound GCC needs to see, so applying it as a "%.*s" precision at
 * every call site silences -Wformat-truncation. */
#define TUTORIAL_PREFIX_CLIP 48

static void get_tutorial_prefix(char *out, size_t out_size) {
    TutorialRuntimeState state = tutorial_state_view();
    if (state.active && state.step == 0) {
        const char *name = repl_tutorial_name(state.tutorial_idx);
        int curr = state.tutorial_idx + 1;
        int total = repl_tutorial_count();
        snprintf(out, out_size, "[%d/%d] %s Tutorial: ", curr, total, name);
    } else {
        out[0] = '\0';
    }
}

static void format_step_entry_hint(int step, int total,
                                   char *out, size_t out_size) {
    char prefix[TUTORIAL_STATUS_MAX];
    get_tutorial_prefix(prefix, sizeof(prefix));
    snprintf(out, out_size,
             "%.*sstep %d/%d - type the command or press Tab to autocomplete",
             TUTORIAL_PREFIX_CLIP, prefix, step + 1, total);
}

static void format_step_commit_hint(int step, int total,
                                    char *out, size_t out_size) {
    char prefix[TUTORIAL_STATUS_MAX];
    get_tutorial_prefix(prefix, sizeof(prefix));
    snprintf(out, out_size,
             "%.*sstep %d/%d - press Enter or ';' to commit",
             TUTORIAL_PREFIX_CLIP, prefix, step + 1, total);
}

/* Status emitted on COMMAND-step entry. The trailing affordance hint
 * teaches the user the two ways to fill the input row (type, or Tab-
 * accept the autocomplete ghost). The controller's per-frame tick
 * keeps this visible by re-emitting it while no other status owns the
 * slot - see tutorial_status_hint and the glr_ctrl_tick refresh. */
static void tutorial_set_step_status(int tutorial_idx, int step) {
    int total = repl_tutorial_step_count(tutorial_idx);
    char msg[REPL_DIAG_TEXT_MAX];

    if (total <= 0)
        return;
    format_step_entry_hint(step, total, msg, sizeof msg);
    repl_set_status(msg);
}

/* Shift any tutorial-tracked line at-or-after `pos` by `delta`. Used
 * before a runner-driven instruction-comment insert and after a
 * matched expected-command insert. Deliberately does NOT touch
 * pending.commit_line - that field is the immutable
 * snapshot of where the in-flight commit attempt targets, and the
 * success bookkeeping reads it back unchanged after this shift. */
static void tutorial_shift_tracked_lines_from(int pos, int delta) {
    TutorialRuntimeState *state = tutorial_state_mut();
    int skip_expected_commit_shift;

    if (delta == 0)
        return;
    for (int i = 0; i < state->locked_line_count; i++) {
        if (state->locked_lines[i] >= pos)
            state->locked_lines[i] += delta;
    }
    skip_expected_commit_shift = (state->pending.step_idx >= 0 &&
                                  pos == state->pending.commit_line);
    if (state->fade_line_idx >= pos)
        state->fade_line_idx += delta;
    if (state->expected_commit_line >= 0 &&
        state->expected_commit_line >= pos &&
        /* `expected_commit_line` already names the row the in-flight
         * user commit will occupy. When the shift is describing that
         * exact insert, leave the row untouched so we do not
         * double-shift the pending target. */
        !skip_expected_commit_shift) {
        state->expected_commit_line += delta;
    }
    for (int i = 0; i < TUTORIAL_MAX_STEPS; i++) {
        if (state->instruction_line_for_step[i] >= 0 &&
            state->instruction_line_for_step[i] >= pos)
            state->instruction_line_for_step[i] += delta;
    }
    /* Each active slot names the auto-`}` row for one nested tutorial
     * block. Inserts at the body insertion point shift that end row just
     * like locked_lines; tracking it here keeps the next in-block append
     * independent of wherever the user has moved the editor cursor. */
    for (int i = 0;
         i < state->block_depth && i < TUTORIAL_MAX_STEPS;
         i++) {
        if (state->block_end_lines[i] >= pos)
            state->block_end_lines[i] += delta;
    }
}

static int tutorial_emit_instruction_comment(const char *comment,
                                             int instruction_line) {
    char err[TUTORIAL_STATUS_MAX] = "";
    TutorialRuntimeState *state = tutorial_state_mut();

    if (!comment || !comment[0])
        return 0;
    if (instruction_line < 0 ||
        instruction_line > repl_state_document_count()) {
        repl_set_status("Tutorial instruction line out of range");
        return 0;
    }

    /* Shift tracked tutorial lines at-or-after the insertion site by
     * +1 BEFORE the load so the loader's own apply doesn't see stale
     * tracked indices. */
    tutorial_shift_tracked_lines_from(instruction_line, 1);

    /* repl_load_apply_line caller contract (src/repl/load.h):
     * Programmatic scene-setup/comment injection by the tutorial
     * subsystem is a programmatic load, not user-driven editing,
     * so it intentionally bypasses the editor commit transaction
     * and its undo history generation.
     * supply the insertion index via &edit_line_inout; clear
     * insert mode; mark flat/normals dirty after loading. The
     * second `editor_state_edit_line_set(expected_commit_line)`
     * below overrides any post-load cursor advance, so the value
     * threaded here is purely for the insert position. */
    int loader_edit_line = instruction_line;
    repl_dispatch_insert_mode_off();
    if (!repl_load_apply_line(comment, err, (int)sizeof(err), &loader_edit_line)) {
        /* Loader failed - undo the speculative shift so tracked
         * indices stay consistent with the unchanged document. */
        tutorial_shift_tracked_lines_from(instruction_line, -1);
        repl_set_status(err[0] ? err : "Tutorial instruction load failed");
        return 0;
    }

    repl_state_mark_flat_dirty();
    repl_state_mark_source_dirty();
    state->fade_line_idx = instruction_line;
    state->fade_start_t = repl_state_variables().anim_time;
    /* Per-line duration at a fixed chars-per-second rate so every
     * instruction reveals at the same readable pace, regardless of
     * length. (line_len + SETTLE_CHARS) total slots / rate seconds -
     * the +SETTLE_CHARS pad keeps the trailing settle wave within the
     * animation window the renderer queries. */
    {
        int n = (int)strlen(comment);
        if (n < 1) n = 1;
        state->fade_duration =
            (float)(n + TUTORIAL_FADE_SETTLE_CHARS) /
            TUTORIAL_FADE_CHARS_PER_SEC;
    }
    tutorial_append_locked_line(instruction_line);

    /* Record where this step's instruction comment landed so a
     * later label-targeted step can splice ABOVE this comment
     * (rather than between the comment and the user's commit row),
     * keeping the original (instruction, command) pair adjacent. */
    if (state->step >= 0 && state->step < TUTORIAL_MAX_STEPS)
        state->instruction_line_for_step[state->step] = instruction_line;

    /* Cursor + expected_commit_line are set by the COMMAND branch in
     * tutorial_enter_step - SET/REQUIRE steps have no command for the
     * user to type and don't move the cursor / pin a commit row. */
    return 1;
}

typedef enum {
    TUTORIAL_STEP_AUTOADVANCE = 0,
    TUTORIAL_STEP_PAUSED = 1,
    TUTORIAL_STEP_TERMINAL = -1
} TutorialStepResult;

/* Forward decls - used inside tutorial_enter_step / advance loop. */
static TutorialStepResult tutorial_enter_step(int step);
static void tutorial_advance_loop(void);

static void tutorial_pending_reset(TutorialRuntimeState *state) {
    state->pending.step_idx = -1;
    state->pending.commit_line = -1;
    state->pending.doc_count_before = -1;
}

static void tutorial_advance_step(TutorialRuntimeState *state) {
    tutorial_pending_reset(state);
    state->expected_commit_line = -1;
    state->step++;
    tutorial_advance_loop();
}

static void tutorial_set_status_ack_set(void) {
    char prefix[TUTORIAL_STATUS_MAX];
    char msg[TUTORIAL_STATUS_MAX];
    get_tutorial_prefix(prefix, sizeof(prefix));
    snprintf(msg, sizeof(msg), "%.*sPress Enter / Tab / Space to continue",
             TUTORIAL_PREFIX_CLIP, prefix);
    repl_set_status(msg);
}

static void tutorial_set_status_require(const char *slug, int target) {
    char prefix[TUTORIAL_STATUS_MAX];
    char msg[TUTORIAL_STATUS_MAX];
    get_tutorial_prefix(prefix, sizeof(prefix));
    snprintf(msg, sizeof(msg), "%.*sSet %s = %d to continue",
             TUTORIAL_PREFIX_CLIP, prefix, slug ? slug : "?", target);
    repl_set_status(msg);
}

static void tutorial_set_status_require_var(const char *name, float target) {
    char prefix[TUTORIAL_STATUS_MAX];
    char msg[TUTORIAL_STATUS_MAX];
    get_tutorial_prefix(prefix, sizeof(prefix));
    snprintf(msg, sizeof(msg),
             "%.*sSet %s = %g (type %s = ...; or drag the slider) to continue",
             TUTORIAL_PREFIX_CLIP, prefix, name ? name : "?", (double)target, name ? name : "?");
    repl_set_status(msg);
}

/* Status for a REQUIRE_VAR declaration step (the watched variable does
 * not exist yet). The full line to type - `float name = target;` plus
 * the catalog comment as a trailing comment - rides the autocomplete
 * ghost (see tutorial_shadow_suffix), so the status only names the
 * affordance: type it, or Tab to autocomplete the ghost. */
static void tutorial_set_status_declare_var(const char *name) {
    char prefix[TUTORIAL_STATUS_MAX];
    char msg[TUTORIAL_STATUS_MAX];
    get_tutorial_prefix(prefix, sizeof(prefix));
    snprintf(msg, sizeof(msg),
             "%.*sDeclare %s: type the line shown (or press Tab) to continue",
             TUTORIAL_PREFIX_CLIP, prefix, name ? name : "?");
    repl_set_status(msg);
}

/* Resolve where the next instruction comment for tutorial `idx`
 * step `step` should be inserted. For append placement that's the
 * current document_count; for label placement it's the row of the
 * target step's INSTRUCTION COMMENT - splicing above the comment
 * (rather than between the comment and its committed command)
 * keeps the original (instruction, command) pair adjacent in the
 * final document. Returns 1 on success and writes the insertion
 * line into *out_line. Returns 0 (and sets a status message) on
 * internal failure. tutorial_start validates the catalog up front
 * so this should never fail at runtime unless something has gone
 * catastrophically wrong with the tracked-line bookkeeping. */
static int tutorial_step_instruction_line(int tutorial_idx, int step,
                                          int *out_line) {
    if (!out_line)
        return 0;

    TutorialStepPlacementKind placement =
        repl_tutorial_step_placement(tutorial_idx, step);
    if (placement == TUTORIAL_STEP_APPEND) {
        /* Inside an open block the append point is not document_count
         * (that would fall BELOW the auto-inserted `}` row). Use the
         * tutorial-owned row of the innermost auto-`}`; the tracked-line
         * shift path keeps it current as comments, bodies, nested blocks,
         * and branches are inserted. Do not derive this from the editor
         * cursor: NOTE / SET / REQUIRE steps permit navigation, and an
         * already-satisfied REQUIRE auto-advances without a cursor park. */
        TutorialRuntimeState state = tutorial_state_view();
        if (state.block_depth > 0) {
            int level = state.block_depth - 1;
            int line;
            int doc = repl_state_document_count();

            if (level < 0 || level >= TUTORIAL_MAX_STEPS ||
                state.block_end_lines[level] < 0) {
                repl_set_status("Tutorial block insertion row unavailable");
                return 0;
            }
            line = state.block_end_lines[level];
            if (line > doc)
                line = doc;
            *out_line = line;
        } else {
            *out_line = repl_state_document_count();
        }
        return 1;
    }
    if (placement != TUTORIAL_STEP_LABEL) {
        repl_set_status("Tutorial step has unknown placement");
        return 0;
    }

    const char *target = repl_tutorial_step_target_label(tutorial_idx, step);
    if (!target || target[0] == '\0') {
        repl_set_status("Tutorial step has empty target label");
        return 0;
    }

    /* Walk earlier steps to find the one carrying this label. */
    int target_step = -1;
    for (int i = 0; i < step; i++) {
        const char *lbl = repl_tutorial_step_label(tutorial_idx, i);
        if (lbl && lbl[0] && strcmp(lbl, target) == 0) {
            target_step = i;
            break;
        }
    }
    if (target_step < 0) {
        /* Not a step label - try the setup scaffold's `:name` goto
         * labels. Resolved against the LIVE document at step-entry
         * time (not a row recorded at load), so rows shifted by
         * earlier splices are handled by construction. The validator
         * guarantees the label exists in setup and doesn't collide
         * with any step label. */
        SourceTextView text = source_document_view();
        int n = repl_state_document_count();
        const GLCmd *cmds = repl_state_document_cmds();
        for (int row = 0; row < n; row++) {
            char row_label[REPL_GOTO_LABEL_MAX];
            if (cmds[row].type != CMD_GOTO_LABEL)
                continue;
            if (repl_extract_label_name(source_text_line(text, row),
                                        row_label, sizeof(row_label)) &&
                strcmp(row_label, target) == 0) {
                *out_line = row;
                return 1;
            }
        }
        repl_set_status("Tutorial step target label not found");
        return 0;
    }

    TutorialRuntimeState state = tutorial_state_view();
    if (target_step >= TUTORIAL_MAX_STEPS) {
        repl_set_status("Tutorial step target step index out of bounds");
        return 0;
    }
    int line = state.instruction_line_for_step[target_step];
    if (line < 0 || line > repl_state_document_count()) {
        repl_set_status("Tutorial step target line out of document bounds");
        return 0;
    }
    *out_line = line;
    return 1;
}



/* Returns 1 iff the entry-level `@cfg <slug> = <value>` line carries
 * a symbolic value that the bridge cannot resolve. `cfg_line` is the
 * raw catalog string (e.g. "// @cfg grid = GRID_THEME_OFF").
 * `slug` is the already-extracted slug. The output buffer receives a
 * diagnostic on failure. */
static int tutorial_cfg_line_value_resolves(const char *cfg_line,
                                            const char *slug,
                                            char *value_out,
                                            size_t value_out_sz) {
    const char *p = NULL;
    char tmp[REPL_CFG_VALUE_MAX];
    if (!repl_config_extract_slug(cfg_line, tmp, sizeof tmp, &p))
        return 1;  /* shouldn't happen - caller already extracted the slug */
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '=') return 1;  /* slug-only line, nothing to validate */
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t vi = 0;
    while (*p && !isspace((unsigned char)*p) && vi < sizeof tmp - 1)
        tmp[vi++] = *p++;
    tmp[vi] = '\0';
    if (vi == 0) return 1;
    /* Numeric literals always resolve via the strtol fallback -
     * only identifier-shaped values go through resolve_text. */
    if (!(isalpha((unsigned char)tmp[0]) || tmp[0] == '_'))
        return 1;
    int dummy;
    if (repl_cfg_resolve_text(slug, tmp, &dummy))
        return 1;
    if (value_out && value_out_sz > 0) {
        snprintf(value_out, value_out_sz, "%s", tmp);
    }
    return 0;
}

/* Runtime slug + value validation: walk every entry-level @cfg line
 * and every SET / REQUIRE step. Reject the tutorial if the
 * controller-installed config bridge doesn't recognise a slug, or if
 * a symbolic value name (cfg_value_name on a step, or the right-hand
 * side of an entry-level @cfg line) can't be resolved to an int.
 *
 * Catches both typo'd slugs (`gird` for `grid`) and typo'd enum
 * names (`GRID_THEME_RADRA`) at tutorial_start time, before any
 * state has been mutated, so the runner never silently no-ops a SET
 * or stalls a REQUIRE - and never silently lands the showcase at the
 * default `*_OFF` value via the strtol fallback path. Returns 1 on
 * success; on failure returns 0 and writes a diagnostic into `err`. */
int tutorial_validate_entry_against_bridge(const TutorialEntry *entry,
                                           char *err, int err_size) {
    if (!entry) {
        if (err_size > 0)
            snprintf(err, (size_t)err_size,
                     "tutorial entry is NULL");
        return 0;
    }
    const char *name = entry->name ? entry->name : "?";
    char slug[REPL_CFG_KEY_MAX];
    const char *const *cfg = entry->cfg;
    for (int i = 0; cfg && cfg[i]; i++) {
        if (!repl_config_extract_slug(cfg[i], slug, sizeof slug, NULL))
            continue;  /* mal-shaped @cfg line - repl parser will diag */
        if (!repl_cfg_known(slug)) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' @cfg uses unknown slug '%s'",
                         name, slug);
            return 0;
        }
        char bad_value[REPL_CFG_VALUE_MAX] = "";
        if (!tutorial_cfg_line_value_resolves(cfg[i], slug,
                                              bad_value, sizeof bad_value)) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' @cfg '%s = %s' has unknown symbolic value",
                         name, slug, bad_value);
            return 0;
        }
    }
    /* The setup scaffold's leading `// @cfg` header run gets the same
     * slug + symbolic-value validation - it is applied through the
     * same bridge at tutorial_start. */
    const char *const *setup = entry->setup;
    for (int i = 0; setup && setup[i] &&
                    repl_config_extract_slug(setup[i], slug,
                                             sizeof slug, NULL); i++) {
        if (!repl_cfg_known(slug)) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' setup @cfg uses unknown slug '%s'",
                         name, slug);
            return 0;
        }
        char bad_value[REPL_CFG_VALUE_MAX] = "";
        if (!tutorial_cfg_line_value_resolves(setup[i], slug,
                                              bad_value, sizeof bad_value)) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' setup @cfg '%s = %s' has unknown "
                         "symbolic value",
                         name, slug, bad_value);
            return 0;
        }
    }
    if (!entry->steps) return 1;
    for (int s = 0; !repl_tutorial_step_is_sentinel(&entry->steps[s]); s++) {
        const TutorialStep *step = &entry->steps[s];
        if (step->kind != TUTORIAL_STEP_KIND_SET &&
            step->kind != TUTORIAL_STEP_KIND_REQUIRE &&
            step->kind != TUTORIAL_STEP_KIND_SET_QUIET)
            continue;
        const char *step_slug = step->cfg_slug;
        if (!step_slug || !repl_cfg_known(step_slug)) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' step %d uses unknown cfg slug '%s'",
                         name, s, step_slug ? step_slug : "(null)");
            return 0;
        }
        const char *value_name = step->cfg_value_name;
        if (value_name && *value_name) {
            int dummy;
            if (!repl_cfg_resolve_text(step_slug, value_name, &dummy)) {
                if (err_size > 0)
                    snprintf(err, (size_t)err_size,
                             "tutorial '%s' step %d uses unknown symbolic "
                             "value '%s' for slug '%s'",
                             name, s, value_name, step_slug);
                return 0;
            }
        }
    }
    return 1;
}

static int tutorial_validate_slugs(int idx, char *err, int err_size) {
    return tutorial_validate_entry_against_bridge(repl_tutorial_entry(idx),
                                                  err, err_size);
}

static int setup_line_is_blank(const char *line) {
    if (!line)
        return 1;
    while (*line && isspace((unsigned char)*line))
        line++;
    return *line == '\0';
}

/* Every tutorial scene opens with a locked glClear so the scene rect is
 * cleared each frame. Nothing clears it on the program's behalf - the
 * scene-rect clear is program-owned (see glr_ctrl_clear_chrome),
 * identical to the exported C and every built-in example scene, all of
 * which lead with this same call. Without it the render3d scene never
 * clears and animated or orbited frames smear. A one-line comment rides
 * just above it so the learner sees why the locked line is there. Both
 * rows load ahead of any setup scaffold (rows 0-1 of the transient
 * scene) and are locked like the rest of the preloaded rows. */
#define TUTORIAL_SCENE_CLEAR_COMMENT \
    "// Clear the color and depth buffers so each frame starts fresh."
#define TUTORIAL_SCENE_CLEAR_LINE \
    "glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)"

/* The prelude rows injected ahead of every tutorial's steps and setup
 * scaffold, in document order: the explanatory comment then the glClear
 * it describes. Kept as a table so the load loop and the locked-line
 * budget in repl_tutorial_validate() agree on the count
 * (TUTORIAL_SCENE_PRELUDE_ROWS). */
static const char *const g_tutorial_scene_prelude[] = {
    TUTORIAL_SCENE_CLEAR_COMMENT,
    TUTORIAL_SCENE_CLEAR_LINE,
};
STATIC_ASSERT((int)(sizeof(g_tutorial_scene_prelude) /
                    sizeof(g_tutorial_scene_prelude[0])) ==
                  TUTORIAL_SCENE_PRELUDE_ROWS,
              "scene prelude row table out of sync with "
              "TUTORIAL_SCENE_PRELUDE_ROWS");

/* Preload the tutorial's scene prelude into the just-reset transient
 * scene, before step 0: first the mandatory scene-clear rows
 * (g_tutorial_scene_prelude - the explanatory comment and its glClear,
 * every tutorial), then the optional setup scaffold (TutorialEntry.setup).
 * The scaffold honors the example header vocabulary: a leading contiguous
 * `// @cfg` run (parsed into the pending bag, applied through the bridge),
 * optional blank spacing, an optional 5-line `// camera` block, then body
 * lines fed through the non-editor loader. Every loaded row is locked.
 * Returns 1 on success; 0 (with a status message set) on any load failure
 * - tutorial_start unwinds via the baseline restore. Runs BEFORE
 * `state->active` is set so the cfg writes cannot trigger step
 * auto-advancement, mirroring tutorial_baseline_apply. */
static int tutorial_load_scene_prelude(int idx) {
    const char *const *lines = repl_tutorial_setup_lines(idx);
    char slug[REPL_CFG_KEY_MAX];
    char err[TUTORIAL_STATUS_MAX] = "";
    int loader_edit_line = 0;
    int pos = 0;

    repl_dispatch_insert_mode_off();

    /* Rows 0-1: the scene-clear comment and glClear, unconditionally, for
     * every tutorial. Loaded through the non-editor loader like setup body
     * lines so they land ahead of any scaffold and are locked below. */
    for (size_t i = 0; i < TUTORIAL_SCENE_PRELUDE_ROWS; i++) {
        if (!repl_load_apply_line(g_tutorial_scene_prelude[i], err,
                                  (int)sizeof(err), &loader_edit_line)) {
            char msg[TUTORIAL_STATUS_MAX];
            snprintf(msg, sizeof msg, "Tutorial clear prelude failed: %s",
                     err[0] ? err : g_tutorial_scene_prelude[i]);
            repl_set_status(msg);
            return 0;
        }
    }

    if (lines) {
        while (lines[pos] &&
               repl_config_extract_slug(lines[pos], slug, sizeof slug, NULL)) {
            repl_state_parse_workspace_header_line(lines[pos]);
            pos++;
        }
        repl_export_apply_pending_cfg();

        while (lines[pos] && setup_line_is_blank(lines[pos]))
            pos++;
        if (lines[pos])
            pos += repl_example_consume_camera_header(lines + pos);

        for (; lines[pos]; pos++) {
            if (setup_line_is_blank(lines[pos]))
                continue;
            if (!repl_load_apply_line(lines[pos], err, (int)sizeof(err),
                                      &loader_edit_line)) {
                char msg[TUTORIAL_STATUS_MAX];
                snprintf(msg, sizeof msg, "Tutorial setup line failed: %s",
                         err[0] ? err : lines[pos]);
                repl_set_status(msg);
                return 0;
            }
        }
    }

    /* Lock the whole prelude - the glClear plus the scaffold - read-only
     * for the tutorial's duration, like instruction rows. The range
     * covers every loaded row even where the loader reordered them (float
     * decls auto-promote to the document top). Capacity is validator-
     * guaranteed (1 clear + setup lines + steps <= TUTORIAL_LOCKED_LINE_MAX). */
    int rows = repl_state_document_count();
    for (int r = 0; r < rows; r++)
        tutorial_append_locked_line(r);

    repl_state_mark_flat_dirty();
    repl_state_mark_source_dirty();
    return 1;
}

void tutorial_start(int idx) {
    char err[TUTORIAL_STATUS_MAX] = "";

    if (idx < 0 || idx >= repl_tutorial_count()) {
        repl_set_status("Tutorial index out of range");
        return;
    }

    /* Validate the full catalog entry BEFORE mutating any state so a
     * malformed tutorial cannot leave the editor in a half-applied
     * transient scene. Structural validation first (kind/expected/cfg_slug
     * shape), then runtime slug-known validation against the live bridge. */
    if (!repl_tutorial_validate(idx, err, (int)sizeof(err))) {
        repl_set_status(err[0] ? err : "Tutorial catalog validation failed");
        return;
    }
    if (!tutorial_validate_slugs(idx, err, (int)sizeof(err))) {
        repl_set_status(err[0] ? err : "Tutorial cfg slug validation failed");
        return;
    }

    /* Tear down any predecessor first so the true original configuration
     * baseline is restored before we snapshot a new one. Unconditional
     * (not gated on tutorial_active) because a *finished* tutorial leaves
     * its baseline pending - without the flush, chaining lesson B off
     * lesson A would capture A's presentation as "the user's" and strand
     * the real pre-tutorial config forever. Idempotent when there is
     * neither an active tutorial nor a pending baseline. */
    tutorial_teardown();

    /* Snapshot the user's true pre-tutorial config baseline before making any
     * tutorial mutations, so we can restore it cleanly on teardown/exit. */
    tutorial_baseline_capture(idx);

    repl_scenes_enter_transient_scene();
    repl_scenes_reset_for_transient();
    repl_dispatch_completion_clear();
    tutorial_state_reset_except_baseline();

    tutorial_baseline_apply(idx);

    /* Preload the scene prelude (the row-0 glClear plus the setup
     * scaffold, if any) into the fresh transient scene before step 0 -
     * still before `active = 1`, so its cfg writes cannot auto-advance a
     * REQUIRE step 0. On failure unwind the pieces teardown would (cfg
     * baseline restore + state reset); active was never set, so
     * tutorial_teardown itself would no-op. */
    if (!tutorial_load_scene_prelude(idx)) {
        tutorial_baseline_restore();
        tutorial_state_reset();
        return;
    }

    /* No predef pre-declaration: a REQUIRE_VAR step whose variable does
     * not exist yet is treated as a DECLARATION step (see
     * tutorial_enter_step), so the user declares it themselves with
     * `float n = 5;`. Pre-declaring would both make `n` exist before the
     * user types anything (misleading) and turn the declaration into an
     * "already declared" error. */
    TutorialRuntimeState *state = tutorial_state_mut();
    state->active = 1;
    state->tutorial_idx = idx;
    state->step = 0;

    /* Enter step 0; the advance loop walks past any REQUIRE steps that
     * are already satisfied on entry (auto-advance) without recursion. */
    tutorial_advance_loop();
}

void tutorial_teardown(void) {
    /* Drop the post-tutorial promotion marker unconditionally, ahead of
     * the early-out. Teardown is the flush every wholesale document
     * replacement funnels through (scene / example / workspace load,
     * reset-all, the next tutorial_start, and the promotion transaction
     * itself once it has captured the live document), so the retained
     * post-tutorial identity dies here in every one of those cases.
     * Unconditional because the marker must never outlive the baseline it
     * travels with - see ReplSceneRuntimeState.tutorial_origin_idx. */
    repl_state_scenes_set_tutorial_origin_idx(-1);
    /* Also runs with no active tutorial when a finished one left a
     * pending baseline (tutorial_end_keep_view) - this is the flush. */
    if (!tutorial_active() && !tutorial_state_view().baseline_valid)
        return;
    /* A COMPLETED tutorial parks its index here so F11 can continue from
     * that lesson (tutorial_end_keep_view). Promoting the retained document
     * into a user scene runs this teardown as its baseline flush, and the
     * reset below would take the parked index with it - so pressing Enter
     * once more after finishing a lesson (which is all promotion takes) sent
     * F11 back to tutorial 1. Teardown is a document-replacement flush, not
     * an abandonment of the user's place in the catalog, so carry the index
     * across the reset.
     *
     * Only for an already-INACTIVE tutorial. An explicit exit has already
     * zeroed the index via tutorial_end_keep_view(0), so it stays lost; a
     * still-ACTIVE tutorial is being torn down mid-lesson (scene / example /
     * workspace load, reset-all, the next tutorial_start), and those keep
     * clearing it - tutorial_start overwrites the index immediately after
     * anyway. */
    int retained_idx = tutorial_active()
                           ? -1
                           : tutorial_state_view().tutorial_idx;

    /* Deactivate active status BEFORE restoring config to prevent step
     * auto-advancement side effects during config restore writes. */
    tutorial_state_mut()->active = 0;
    tutorial_baseline_restore();
    tutorial_state_reset();
    if (retained_idx >= 0)
        tutorial_state_mut()->tutorial_idx = retained_idx;
}

/* End a tutorial the user saw through - completed, or exited on purpose -
 * WITHOUT restoring the cfg baseline. Snapping the presentation back the
 * instant the last step lands reads as the tutorial undoing itself, and
 * throws away the very settings a SET/REQUIRE lesson just taught. The
 * learner stays in the tutorial's transient scene, so its view stays too;
 * the baseline survives in the bag as a pending restore that the next
 * tutorial_teardown() flushes - i.e. when the document is next replaced
 * wholesale (scene / example / workspace load, reset-all, or the next
 * tutorial_start). Only the internal-failure paths tear down immediately:
 * a tutorial that broke mid-step has no view worth keeping.
 *
 * `tutorial_state_reset_except_baseline` clears `active` along with the
 * rest of the runtime state, so the tutorial is fully over either way.
 * A naturally completed tutorial retains its index so F11 can advance from
 * that lesson; an explicit exit remains an unselected tutorial.
 *
 * This is also the ONLY place a post-tutorial scene origin is established.
 * The retained document has neither an active user scene nor an active
 * example, so without a marker the undo hook's promotion pass would decline
 * it and the user's post-tutorial edits would be discarded by the next scene
 * switch. Recording the index here - rather than at tutorial_start - is what
 * keeps an ACTIVE tutorial unpromotable: its own step commits run through
 * editor_undo_push_snapshot() and would otherwise promote (and tear down the
 * lesson) on step 0. The marker travels with the pending cfg baseline: both
 * are cleared together by tutorial_teardown(). */
static void tutorial_end_keep_view(int keep_tutorial_idx) {
    int tutorial_idx = tutorial_state_view().tutorial_idx;

    if (!tutorial_active())
        return;
    tutorial_state_mut()->active = 0;
    /* Capture the origin before the reset clears `tutorial_idx`; both
     * completion and tutorial_stop() land here, so a stopped lesson leaves
     * an equally promotable document. */
    repl_state_scenes_set_tutorial_origin_idx(tutorial_idx);
    tutorial_state_reset_except_baseline();
    if (keep_tutorial_idx)
        tutorial_state_mut()->tutorial_idx = tutorial_idx;
}

void tutorial_stop(void) {
    if (!tutorial_active())
        return;

    /* Set status before ending so it is visible to the user. */
    repl_set_status("Tutorial exited");
    tutorial_end_keep_view(0);
    repl_dispatch_completion_update();
}

int tutorial_handle_commit_attempt(const char *input, TutorialMatchResult *out) {
    TutorialMatchResult result;
    const char *expected = tutorial_current_expected_text();

    if (!tutorial_active() || !expected) {
        result = (TutorialMatchResult){
            .kind = TUT_MATCH_OK,
            .message = "",
        };
        if (out)
            *out = result;
        return 1;
    }

    result = tutorial_match(expected, input);
    if (out)
        *out = result;
    return result.kind == TUT_MATCH_OK;
}

/* When a COMMAND step's input is a complete match for the expected
 * command (Tab-accepted or fully typed), refresh the status to remind
 * the user how to commit. Called from the completion provider on every
 * input change while the cursor is on the expected commit line, so the
 * hint keeps its TTL fresh as long as the user holds at the matched
 * state. No-op for inactive / SET / REQUIRE / partial-input - those
 * paths let any existing status fade naturally. Uses the same
 * tutorial_match normalization the commit path uses, so trailing
 * whitespace or an extra ';' on the input still counts as matched. */
void tutorial_refresh_input_hint(const char *input) {
    TutorialRuntimeState state;
    const char *expected;
    TutorialMatchResult r;
    int total;
    char msg[REPL_DIAG_TEXT_MAX];

    if (!tutorial_active())
        return;
    state = tutorial_state_view();
    if (repl_tutorial_step_kind(state.tutorial_idx, state.step) !=
        TUTORIAL_STEP_KIND_COMMAND)
        return;
    expected = tutorial_current_expected_text();
    if (!expected)
        return;
    r = tutorial_match(expected, input ? input : "");
    if (r.kind != TUT_MATCH_OK)
        return;
    total = repl_tutorial_step_count(state.tutorial_idx);
    if (total <= 0)
        return;
    format_step_commit_hint(state.step, total, msg, sizeof msg);
    repl_set_status(msg);
}

/* Compute the COMMAND-step status hint the controller should keep
 * visible right now. Returns 1 (with the hint written into `out`) when
 * an active COMMAND step wants a hint; 0 (with `out` cleared) for
 * inactive / SET / REQUIRE. Picks the commit-reminder variant when the
 * cursor sits on the expected commit line AND the input fully matches
 * the expected command; picks the entry "type or Tab" variant
 * otherwise. The line-aware check keeps the commit reminder from
 * appearing while the user is on an unrelated row whose buffer
 * coincidentally matches. */
int tutorial_status_hint(char *out, size_t out_size) {
    TutorialRuntimeState state;
    const char *expected;
    int total;

    if (out && out_size > 0)
        out[0] = '\0';
    if (!out || out_size == 0)
        return 0;
    if (!tutorial_active())
        return 0;
    state = tutorial_state_view();
    if (repl_tutorial_step_kind(state.tutorial_idx, state.step) !=
        TUTORIAL_STEP_KIND_COMMAND)
        return 0;
    expected = tutorial_current_expected_text();
    if (!expected)
        return 0;
    total = repl_tutorial_step_count(state.tutorial_idx);
    if (total <= 0)
        return 0;

    int show_commit = 0;
    if (state.expected_commit_line >= 0 &&
        repl_dispatch_edit_line_get() == state.expected_commit_line) {
        const char *inp_text = repl_dispatch_host_input_get();
        TutorialMatchResult r = tutorial_match(expected, inp_text);
        show_commit = (r.kind == TUT_MATCH_OK);
    }
    if (show_commit)
        format_step_commit_hint(state.step, total, out, out_size);
    else
        format_step_entry_hint(state.step, total, out, out_size);
    return 1;
}

/* Sentinel-prefix predicate the controller uses to decide whether the
 * current status text is owned by the tutorial hint system (entry or
 * commit variant) and therefore safe to refresh. */
int tutorial_status_is_hint(const char *text) {
    if (!text)
        return 0;
    if (!tutorial_active())
        return 0;
    char prefix[REPL_STATUS_TEXT_MAX];
    get_tutorial_prefix(prefix, sizeof(prefix));
    char full_prefix[REPL_STATUS_TEXT_MAX + 8];
    snprintf(full_prefix, sizeof(full_prefix), "%.*sstep ",
             TUTORIAL_PREFIX_CLIP, prefix);
    return strncmp(text, full_prefix, strlen(full_prefix)) == 0;
}

static TutorialStepResult tutorial_enter_step_command(int idx, int step, int commit_line, TutorialRuntimeState *state) {
    /* Typing setup: cursor on the row the expected command should
     * commit at (below the instruction comment when the step emitted
     * one, the insertion row itself for comment-less steps); insert
     * mode iff mid-document. */
    state->expected_commit_line = commit_line;
    repl_dispatch_host_cursor_park(state->expected_commit_line,
                                   state->expected_commit_line <
                                   repl_state_document_count());
    tutorial_set_step_status(idx, step);
    /* Refresh autocomplete so the shadow ghost for the expected
     * command appears on the next frame, not the next keystroke. */
    repl_dispatch_completion_update();
    return TUTORIAL_STEP_PAUSED;
}

static TutorialStepResult tutorial_enter_step_note(int instruction_line, TutorialRuntimeState *state) {
    /* Comment-only showcase: the instruction comment was already
     * emitted; wait for an ack key (Enter/Tab/Space). Same frozen-
     * document, park-past-the-comment flow as SET, minus the cfg
     * write. */
    state->expected_commit_line = -1;
    repl_dispatch_host_cursor_park(instruction_line + 1,
                                   (instruction_line + 1) <
                                   repl_state_document_count());
    tutorial_set_status_ack_set();
    repl_dispatch_completion_update();
    return TUTORIAL_STEP_PAUSED;
}

static TutorialStepResult tutorial_enter_step_set(int idx, int step, int instruction_line, TutorialRuntimeState *state) {
    /* Showcase: apply the cfg so the user immediately sees the
     * effect, then wait for an ack key (Enter/Tab/Space). No
     * typing cursor - the document is read-only on this step
     * (tutorial_guard_source_change rejects non-COMMAND mutations).
     * Park the editor cursor on the virtual trailing row AFTER the
     * comment we just inserted; otherwise the editor renders the
     * empty input-buffer overlay on top of the comment row and the
     * instruction is invisible until the user presses a key. */
    const char *slug       = repl_tutorial_step_cfg_slug(idx, step);
    const char *value_name = repl_tutorial_step_cfg_value_name(idx, step);
    int         value      = repl_tutorial_step_cfg_value(idx, step);

    if (value_name)
        /* Symbolic value (e.g. "GRID_THEME_RADAR"). Bridge resolves
         * the name to int via resolve_text inside apply. */
        repl_cfg_set_text(slug, value_name);
    else
        repl_cfg_set_int(slug, value);

    state->expected_commit_line = -1;
    repl_dispatch_host_cursor_park(instruction_line + 1,
                                   (instruction_line + 1) <
                                   repl_state_document_count());
    tutorial_set_status_ack_set();
    repl_dispatch_completion_update();
    return TUTORIAL_STEP_PAUSED;
}

/* Staging sibling of tutorial_enter_step_set: apply the cfg and hand the
 * advance loop AUTOADVANCE, so a run of these collapses into a single
 * frame with no instruction row, no cursor park and no ack. Nothing here
 * touches the document, so there is no locked line to track and no
 * expected_commit_line to clear. */
static TutorialStepResult tutorial_enter_step_set_quiet(int idx, int step) {
    const char *slug       = repl_tutorial_step_cfg_slug(idx, step);
    const char *value_name = repl_tutorial_step_cfg_value_name(idx, step);
    int         value      = repl_tutorial_step_cfg_value(idx, step);

    if (value_name)
        repl_cfg_set_text(slug, value_name);
    else
        repl_cfg_set_int(slug, value);

    return TUTORIAL_STEP_AUTOADVANCE;
}

static int tutorial_cfg_matches_target(const char *slug, int target) {
    if (!slug || !repl_cfg_known(slug))
        return 0;
    return repl_cfg_get_int(slug, 0) == target;
}

static int tutorial_var_matches_target(const char *name, float target) {
    if (!name || !name[0])
        return 0;
    int idx = repl_eval_find_predef_var_idx(name);
    if (idx < 0)
        return 0;
    return fabsf(repl_eval_predef_view().vars[idx].value - target) <=
           TUTORIAL_VAR_EPS;
}

static TutorialStepResult tutorial_enter_step_require(int idx, int step, int instruction_line, TutorialRuntimeState *state) {
    /* Check: advance when the user themselves sets the slug to the
     * target. If already satisfied on entry, signal auto-advance to
     * the surrounding loop (no recursion - a chain of already-
     * satisfied REQUIREs can't blow the stack). */
    const char *slug       = repl_tutorial_step_cfg_slug(idx, step);
    const char *value_name = repl_tutorial_step_cfg_value_name(idx, step);
    int target = repl_tutorial_step_cfg_value(idx, step);
    if (value_name) {
        int resolved;
        if (repl_cfg_resolve_text(slug, value_name, &resolved))
            target = resolved;
    }
    state->expected_commit_line = -1;
    if (tutorial_cfg_matches_target(slug, target))
        return TUTORIAL_STEP_AUTOADVANCE;  /* auto-advance via the loop */
    /* Park the cursor past the locked instruction-comment row so
     * the editor doesn't render the empty input overlay over it -
     * same fix as the SET branch above. */
    repl_dispatch_host_cursor_park(instruction_line + 1,
                                   (instruction_line + 1) <
                                   repl_state_document_count());
    tutorial_set_status_require(slug, target);
    repl_dispatch_completion_update();
    return TUTORIAL_STEP_PAUSED;
}

static TutorialStepResult tutorial_enter_step_require_var(int idx, int step,
                                                          int instruction_line,
                                                          int declare_step,
                                                          TutorialRuntimeState *state) {
    /* Check: advance when the named predefined variable reaches the
     * target value (typed `name = expr;` / `float name = ...;` commit OR
     * slider drag - both land through repl_apply_predef_ops, which the
     * editor commit path notifies after). Auto-advance on entry if
     * already satisfied.
     *
     * Unlike SET/REQUIRE, the document stays writable: the user must be
     * able to type the assignment and have it commit.
     *
     * `declare_step` is set by tutorial_enter_step when the variable does
     * not exist yet. No separate instruction comment is emitted for it:
     * the satisfying `float name = ...;` is a declaration, which the
     * compiler relocates to the document top, so a locked comment line
     * above it would be stranded. Instead the instruction rides the
     * autocomplete ghost `float name = target; <catalog comment>`
     * (synthesized in tutorial_shadow_suffix), so the catalog comment
     * commits as a TRAILING comment on the decl line and travels with it.
     * The cursor parks on the trailing row (== instruction_line, since
     * nothing was inserted); a normal REQUIRE_VAR step parks at
     * instruction_line+1, just below the comment it did emit. */
    const char *name   = repl_tutorial_step_var_name(idx, step);
    float       target = repl_tutorial_step_var_target(idx, step);

    state->expected_commit_line = -1;
    if (tutorial_var_matches_target(name, target))
        return TUTORIAL_STEP_AUTOADVANCE;

    int park = declare_step ? instruction_line : instruction_line + 1;
    repl_dispatch_host_cursor_park(park, park < repl_state_document_count());

    if (declare_step)
        tutorial_set_status_declare_var(name);
    else
        tutorial_set_status_require_var(name, target);
    repl_dispatch_completion_update();
    return TUTORIAL_STEP_PAUSED;
}

/* Enter the step at the CURRENT state->step. The advance loop owns the
 * step pointer; this function emits the instruction, applies any kind-
 * specific side effects, and returns one of:
 *    TUTORIAL_STEP_PAUSED       paused - waiting on user (COMMAND typing, SET ack key,
 *                               REQUIRE state-change notify).
 *    TUTORIAL_STEP_AUTOADVANCE  auto-advance requested - REQUIRE step found already-satisfied.
 *                               The loop bumps state->step and tries again.
 *    TUTORIAL_STEP_TERMINAL     terminal - either the catalog sentinel ("Tutorial complete") or
 *                               an internal failure. teardown() has already run. */
static TutorialStepResult tutorial_enter_step(int step) {
    TutorialRuntimeState *state = tutorial_state_mut();
    int idx = state->tutorial_idx;

    if (!repl_tutorial_step_get(idx, step)) {
        /* Past the last step - tutorial complete. The lesson's view is
         * kept (see tutorial_end_keep_view); status set BEFORE ending so
         * it survives. (Keyed on the step lookup, not a NULL comment -
         * comment-less COMMAND steps legitimately have no comment.) */
        char shortcut[KEYMAP_SHORTCUT_LABEL_MAX];
        char status_msg[REPL_STATUS_TEXT_MAX];
        keymap_binding_to_string(shortcut, sizeof(shortcut),
                                 KM_KEY(GLR_NEXT_TUTORIAL),
                                 KM_MODS(GLR_NEXT_TUTORIAL), 1);
        snprintf(status_msg, sizeof(status_msg),
                 "Tutorial complete - press %s to advance or edit to continue", shortcut);
        repl_set_status(status_msg);
        tutorial_end_keep_view(1);
        repl_dispatch_completion_update();
        return TUTORIAL_STEP_TERMINAL;
    }

    const char *comment = repl_tutorial_step_comment(idx, step);
    int have_comment = (comment != NULL && comment[0] != '\0');

    int instruction_line = 0;
    if (!tutorial_step_instruction_line(idx, step, &instruction_line)) {
        tutorial_teardown();
        return TUTORIAL_STEP_TERMINAL;
    }

    TutorialStepKind kind = repl_tutorial_step_kind(idx, step);

    /* A REQUIRE_VAR step whose watched variable does not exist yet is a
     * DECLARATION step: the satisfying `float name = ...;` is a decl,
     * which the compiler relocates to the top of the document (above any
     * comment - see compile_insert_pos / decl_pos in src/repl/compile.c).
     * A locked instruction comment emitted there would be stranded above
     * the user's own declaration and would desync the tutorial's
     * locked-line tracking, so skip the code comment for declaration
     * steps; tutorial_enter_step_require_var delivers the instruction via
     * the status line + autocomplete ghost instead. Once the variable
     * exists (step 1+) the satisfying `name = expr;` is an ordinary
     * assignment that appends below a normal locked comment. */
    int declare_step = (kind == TUTORIAL_STEP_KIND_REQUIRE_VAR &&
                        repl_eval_find_predef_var_idx(
                            repl_tutorial_step_var_name(idx, step)) < 0);

    /* Comment-less COMMAND steps (have_comment == 0) emit nothing: the
     * expected command commits directly at instruction_line, taught by
     * the autocomplete ghost + status hint alone. The validator
     * guarantees every non-COMMAND kind carries a non-empty comment. */
    if (!declare_step && have_comment &&
        !tutorial_emit_instruction_comment(comment, instruction_line)) {
        tutorial_teardown();
        return TUTORIAL_STEP_TERMINAL;
    }

    switch (kind) {
    case TUTORIAL_STEP_KIND_COMMAND:
        /* Commit row: below the instruction comment when one was
         * emitted; the insertion row itself when the step has none. */
        return tutorial_enter_step_command(idx, step,
                                           instruction_line +
                                           (have_comment ? 1 : 0),
                                           state);
    case TUTORIAL_STEP_KIND_NOTE:
        return tutorial_enter_step_note(instruction_line, state);
    case TUTORIAL_STEP_KIND_SET:
        return tutorial_enter_step_set(idx, step, instruction_line, state);
    case TUTORIAL_STEP_KIND_SET_QUIET:
        return tutorial_enter_step_set_quiet(idx, step);
    case TUTORIAL_STEP_KIND_REQUIRE:
        return tutorial_enter_step_require(idx, step, instruction_line, state);
    case TUTORIAL_STEP_KIND_REQUIRE_VAR:
        return tutorial_enter_step_require_var(idx, step, instruction_line,
                                               declare_step, state);
    }
    /* Unknown kind - validator should have rejected this. */
    tutorial_teardown();
    return TUTORIAL_STEP_TERMINAL;
}

/* Iterative advance: keep entering steps until one pauses for user input
 * (return 1) or the tutorial terminates (return -1). Replaces the prior
 * inline body of tutorial_advance_after_successful_commit so REQUIRE
 * auto-advance chains never recurse. */
static void tutorial_advance_loop(void) {
    TutorialRuntimeState *state = tutorial_state_mut();
    if (!tutorial_active()) return;
    while (1) {
        TutorialStepResult r = tutorial_enter_step(state->step);
        if (r != TUTORIAL_STEP_AUTOADVANCE) return;
        state->step++;
    }
}

void tutorial_advance_after_successful_commit(void) {
    if (!tutorial_active())
        return;
    /* REQUIRE_VAR uses the predef-writeback notify hook as its sole
     * advance signal - a successful commit by itself does NOT mean
     * the watched variable reached its target (e.g. the user typed
     * `m = 5;` while the step watches `n`). The notify hook fires
     * inside apply_compiled_change_full and advances iff the match
     * holds; this commit-side path must stay a no-op for REQUIRE_VAR
     * so unrelated commits cannot skip the step. */
    if (tutorial_current_step_kind() == TUTORIAL_STEP_KIND_REQUIRE_VAR)
        return;
    tutorial_advance_step(tutorial_state_mut());
}

TutorialStepKind tutorial_current_step_kind(void) {
    TutorialRuntimeState state = tutorial_state_view();
    if (!state.active)
        return TUTORIAL_STEP_KIND_COMMAND;  /* safe default */
    return repl_tutorial_step_kind(state.tutorial_idx, state.step);
}

void tutorial_notify_state_changed(void) {
    if (!tutorial_active())
        return;
    TutorialRuntimeState state = tutorial_state_view();
    TutorialStepKind kind = repl_tutorial_step_kind(state.tutorial_idx, state.step);
    int matched = 0;
    if (kind == TUTORIAL_STEP_KIND_REQUIRE) {
        const char *slug = repl_tutorial_step_cfg_slug(state.tutorial_idx, state.step);
        int target = repl_tutorial_step_cfg_value(state.tutorial_idx, state.step);
        matched = tutorial_cfg_matches_target(slug, target);
    } else if (kind == TUTORIAL_STEP_KIND_REQUIRE_VAR) {
        const char *name = repl_tutorial_step_var_name(state.tutorial_idx, state.step);
        float target = repl_tutorial_step_var_target(state.tutorial_idx, state.step);
        matched = tutorial_var_matches_target(name, target);
    } else {
        return;
    }
    if (!matched)
        return;
    /* Match: advance. Clear pending (REQUIRE / REQUIRE_VAR have no
     * commit attempt in flight, but be defensive). */
    tutorial_advance_step(tutorial_state_mut());
}

int tutorial_handle_ack_key(unsigned char key) {
    if (!tutorial_active())
        return 0;
    TutorialStepKind k = tutorial_current_step_kind();
    if (k != TUTORIAL_STEP_KIND_SET && k != TUTORIAL_STEP_KIND_NOTE)
        return 0;
    if (key != '\r' && key != '\n' && key != '\t' && key != ' ')
        return 0;
    tutorial_advance_step(tutorial_state_mut());
    return 1;
}

int tutorial_reject_noncommand_commit_with_hint(void) {
    if (!tutorial_active())
        return 0;
    TutorialStepKind k = tutorial_current_step_kind();
    if (k == TUTORIAL_STEP_KIND_SET || k == TUTORIAL_STEP_KIND_NOTE) {
        tutorial_set_status_ack_set();
        return 1;
    }
    if (k == TUTORIAL_STEP_KIND_REQUIRE) {
        TutorialRuntimeState s = tutorial_state_view();
        const char *slug = repl_tutorial_step_cfg_slug(s.tutorial_idx, s.step);
        int target = repl_tutorial_step_cfg_value(s.tutorial_idx, s.step);
        tutorial_set_status_require(slug, target);
        return 1;
    }
    return 0;
}

const char *tutorial_current_expected_text(void) {
    TutorialRuntimeState state = tutorial_state_view();

    if (!state.active)
        return NULL;
    return repl_tutorial_step_expected(state.tutorial_idx, state.step);
}

/* Time budget per character slot. The animation fills line_len +
 * TUTORIAL_FADE_SETTLE_CHARS slots over fade_duration: each character
 * takes one slot to fade in (alpha 0 -> 1, bright white), and the
 * trailing W slots pace its color settling from white back to the
 * base comment color. The last character finishes settling exactly
 * at fade_start_t + fade_duration. */










int tutorial_line_is_locked(int line_idx) {
    TutorialRuntimeState state = tutorial_state_view();

    if (!state.active || line_idx < 0)
        return 0;
    for (int i = 0; i < state.locked_line_count; i++) {
        if (state.locked_lines[i] == line_idx)
            return 1;
    }
    return 0;
}



int tutorial_guard_source_change(int pos, int delete_count, int insert_count) {
    TutorialRuntimeState state = tutorial_state_view();

    if (!state.active)
        return 1;
    if (delete_count < 0 || insert_count < 0)
        return 0;
    if (delete_count == 0 && insert_count == 0)
        return 1;

    /* SET / REQUIRE / NOTE steps freeze the document: no
     * expected_commit_line and no locked-line region to anchor to past
     * the last instruction, so paste/delete/comment-toggle/etc. would
     * otherwise slip through after the last locked row. Reject every
     * non-no-op mutation while a non-writable step is active; the
     * editor precheck pairs this with a kind-aware commit hint
     * (tutorial_reject_noncommand_commit_with_hint).
     *
     * REQUIRE_VAR is the exception - the step is satisfied either by a
     * slider drag (no source mutation) OR by a typed `name = expr;`
     * commit (a source mutation we must allow). The locked-line /
     * expected_commit_line checks below still apply, so writes are
     * permitted only past the last locked instruction comment. */
    TutorialStepKind step_kind =
        repl_tutorial_step_kind(state.tutorial_idx, state.step);
    if (step_kind != TUTORIAL_STEP_KIND_COMMAND &&
        step_kind != TUTORIAL_STEP_KIND_REQUIRE_VAR)
        return 0;

    /* Narrow allow-list for the in-flight matched expected commit:
     * only the matched step may insert at its captured commit row,
     * and only as a pure insert (delete_count == 0). Keying off the
     * immutable pending.commit_line - rather than the ambient
     * expected_commit_line - keeps the exception scoped to the one
     * commit attempt the precheck already authorized. */
    if (state.pending.step_idx >= 0 && delete_count == 0 &&
        insert_count > 0 && pos == state.pending.commit_line)
        return 1;

    /* Block any other mutation that would land at the row reserved
     * for the next expected user commit. Without this, a paste or
     * other non-precheck mutation (which never set `pending`) at
     * expected_commit_line would slip in untracked when the target
     * label refers to a step whose committed row has no later
     * locked instruction comment below it - e.g. when the
     * label-targeted step's anchor IS the most-recently-committed
     * command. The locked-line check below would see no later
     * locked row and let the insert through. */
    if (state.expected_commit_line >= 0 &&
        pos == state.expected_commit_line)
        return 0;

    for (int i = 0; i < state.locked_line_count; i++) {
        if (pos <= state.locked_lines[i])
            return 0;
    }
    return 1;
}

int tutorial_expected_commit_line(void) {
    return tutorial_state_view().expected_commit_line;
}

void tutorial_begin_expected_commit_attempt(void) {
    TutorialRuntimeState *state = tutorial_state_mut();

    if (!state->active)
        return;
    state->pending.step_idx = state->step;
    state->pending.commit_line = state->expected_commit_line;
    state->pending.doc_count_before = repl_state_document_count();
}

void tutorial_cancel_pending(void) {
    TutorialRuntimeState *state = tutorial_state_mut();

    /* Idempotent: bail when no pending record is set so callers can
     * dispatch this unconditionally on every rejection path. */
    if (state->pending.step_idx < 0)
        return;
    /* expected_commit_line intentionally survives cancellation so the
     * user can retry editing and committing the correct expected command
     * on the designated insertion row. If we cleared it, the active
     * step would be locked out from being completed. */
    tutorial_pending_reset(state);
}

int tutorial_note_expected_commit_applied(void) {
    TutorialRuntimeState *state = tutorial_state_mut();

    /* No pending record means this commit was NOT a matched COMMAND
     * expected-command attempt (e.g. it was a free-form REQUIRE_VAR
     * commit, whose advance is driven by the predef-writeback notify
     * hook, not the commit). Report 0 so the caller's commit-side
     * advance stays a no-op - otherwise, when the notify already
     * advanced from a REQUIRE_VAR step onto a COMMAND step during this
     * same commit, a second commit-side advance would skip that COMMAND
     * step entirely (its instruction comment emitted, its command never
     * typed). */
    if (state->pending.step_idx < 0)
        return 0;

    int delta = repl_state_document_count() - state->pending.doc_count_before;
    if (delta > 0) {
        /* Shift existing tracked lines (locked instruction comments,
         * fade row, instruction-line records for prior steps) so
         * they keep referring to the same source content after the
         * user's commit grew the document. The shift skips the
         * in-flight pending row, but instruction_line_for_step
         * entries always sit ABOVE pending.commit_line (the
         * instruction is at expected_commit_line - 1), so they
         * never need to shift for an in-flight commit. */
        tutorial_shift_tracked_lines_from(state->pending.commit_line, delta);
    }

    /* Comment-less COMMAND steps recorded no instruction row at entry
     * (nothing was emitted). Record the committed command row instead,
     * so a later label-targeted step can anchor on this step, and lock
     * it - with no instruction comment emitted above later rows, the
     * transitive locked-line protection the comment-full flow relies
     * on doesn't cover this row. Recorded AFTER the shift above so the
     * value isn't itself shifted (the commit row is where the command
     * now lives). */
    if (state->pending.step_idx >= 0 &&
        state->pending.step_idx < TUTORIAL_MAX_STEPS &&
        state->instruction_line_for_step[state->pending.step_idx] < 0 &&
        state->pending.commit_line >= 0) {
        state->instruction_line_for_step[state->pending.step_idx] =
            state->pending.commit_line;
        tutorial_append_locked_line(state->pending.commit_line);
    }

    /* Block-shape bookkeeping (indices are final: the delta shift above
     * already ran). An OPEN commit inserted TWO rows - the header at
     * commit_line and the auto `}` at commit_line + 1; lock both so the
     * block's frame is read-only while its body is typed (the guard's
     * pending-commit exception still lets matched body inserts through
     * at the `}` row). A BRANCH commit inserted its separator row at
     * commit_line (the old `}` shifted below it); lock it. A CLOSE
     * commit changed nothing (the editor's matched-existing branch just
     * moves the cursor past the already-locked `}`), so only the depth
     * changes. */
    {
        TutorialExpectedShape shape = repl_tutorial_expected_shape(
            repl_tutorial_step_expected(state->tutorial_idx,
                                        state->pending.step_idx));
        if (shape == TUTORIAL_EXPECTED_BLOCK_OPEN) {
            /* Existing outer end rows were shifted above. Record the new
             * innermost auto-end in the next stack slot before increasing
             * depth, so later insertions shift every active level. */
            if (state->block_depth < 0 ||
                state->block_depth >= TUTORIAL_MAX_STEPS) {
                repl_set_status("Tutorial block nesting exceeds runtime capacity");
                tutorial_pending_reset(state);
                return 0;
            }
            state->block_end_lines[state->block_depth] =
                state->pending.commit_line + 1;
            state->block_depth++;
            tutorial_append_locked_line(state->pending.commit_line);
            tutorial_append_locked_line(state->pending.commit_line + 1);
        } else if (shape == TUTORIAL_EXPECTED_BLOCK_BRANCH) {
            tutorial_append_locked_line(state->pending.commit_line);
        } else if (shape == TUTORIAL_EXPECTED_BLOCK_CLOSE) {
            if (state->block_depth > 0) {
                state->block_depth--;
                state->block_end_lines[state->block_depth] = -1;
            }
        }
    }

    tutorial_pending_reset(state);
    return 1;
}
