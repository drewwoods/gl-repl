#include "widgets/tutorial.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "editor/completion.h"
#include "editor/state.h"
#include "repl/core.h"
#include "repl/export.h"   /* repl_state_parse_workspace_header_line for @cfg */
#include "repl/load.h"
#include "repl/scenes.h"
#include "repl/state_owners.h"
#include "repl/tutorials.h"

static float clamp01(float value) {
    if (value <= 0.0f)
        return 0.0f;
    if (value >= 1.0f)
        return 1.0f;
    return value;
}

/* --- cfg baseline snapshot ----------------------------------------------- */

/* Restore-on-exit storage: a bag of (slug, value) pairs the active
 * tutorial promises to restore on teardown. Captured by
 * `tutorial_capture_cfg_baseline()` at `tutorial_start` BEFORE any
 * tutorial machinery (transient-scene entry, presentation reset, entry
 * `@cfg` apply, step SET writes) has touched live cfg — so the baseline
 * is the user's true pre-tutorial state. Written back by
 * `tutorial_teardown()` at every teardown path so a tutorial leaves no
 * scene-presentation footprint.
 *
 * Coverage:
 *   1) Every slug in the bridge's `fill_scene_subset` (the same set
 *      `src/repl/scenes.c::stash_live_state` captures for scene saves)
 *      — so presentation_reset's effects revert too, not just SET/REQUIRE.
 *   2) Plus any extra slugs the tutorial references via entry @cfg or
 *      step SET/REQUIRE (e.g. `view_mode`, which is intentionally OUTSIDE
 *      the scene subset since it isn't a per-scene property).
 *
 * The bag's set_int dedups by slug, so we can union (1) and (2) blindly. */
static ReplExportConfig g_tut_cfg_baseline_bag;
static int              g_tut_cfg_baseline_valid;

static void tutorial_cfg_baseline_clear(void) {
    repl_export_config_clear(&g_tut_cfg_baseline_bag);
    g_tut_cfg_baseline_valid = 0;
}

/* Parse `// @cfg <slug> = <val>` and write the slug to `out`. Returns 1
 * on success, 0 if the line doesn't have the expected shape. Tolerates
 * the same surface variations the export-side parse_cfg does
 * (whitespace, `// ` prefix). */
static int tutorial_extract_cfg_slug(const char *line, char *out, size_t out_sz) {
    if (!line || !out || out_sz == 0) return 0;
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p[0] != '/' || p[1] != '/') return 0;
    p += 2;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '@') return 0;
    p++;
    if (strncmp(p, "cfg", 3) != 0) return 0;
    p += 3;
    if (!isspace((unsigned char)*p)) return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t out_i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_') && out_i + 1 < out_sz)
        out[out_i++] = *p++;
    out[out_i] = '\0';
    return out_i > 0;
}

/* Helper: add (slug, current_value) to the baseline bag iff the bridge
 * recognises the slug. The bag's `set_int` already dedups by slug, so
 * unioning the scene-subset capture with tutorial-specific slugs is
 * blind-append-safe. */
static void tutorial_cfg_baseline_record_one(const char *slug) {
    if (!slug || !slug[0])     return;
    if (!repl_cfg_known(slug)) return;
    repl_export_config_set_int(&g_tut_cfg_baseline_bag, slug,
                               repl_cfg_get_int(slug, 0));
}

/* Snapshot the user's pre-tutorial cfg into the bag. Two coverage
 * sources, unioned via the bag's slug-dedup:
 *   - `fill_scene_subset` (every per-scene presentation slug — so
 *     `presentation_reset(0)` and any cascading defaults revert too);
 *   - Tutorial-specific slugs referenced by entry `@cfg` or step
 *     SET/REQUIRE (e.g. `view_mode`, which is intentionally outside
 *     the scene subset). */
static void tutorial_capture_cfg_baseline(int idx) {
    tutorial_cfg_baseline_clear();
    const ReplExportConfigBridge *b = repl_export_config_bridge();
    if (b && b->fill_scene_subset)
        b->fill_scene_subset(&g_tut_cfg_baseline_bag);

    char slug[REPL_EXPORT_CFG_KEY_MAX];
    const char *const *cfg = repl_tutorial_cfg_lines(idx);
    for (int i = 0; cfg && cfg[i]; i++) {
        if (tutorial_extract_cfg_slug(cfg[i], slug, sizeof slug))
            tutorial_cfg_baseline_record_one(slug);
    }
    int n = repl_tutorial_step_count(idx);
    for (int s = 0; s < n; s++) {
        TutorialStepKind k = repl_tutorial_step_kind(idx, s);
        if (k == TUTORIAL_STEP_KIND_SET || k == TUTORIAL_STEP_KIND_REQUIRE)
            tutorial_cfg_baseline_record_one(repl_tutorial_step_cfg_slug(idx, s));
    }
    g_tut_cfg_baseline_valid = 1;
}

/* Apply the captured bag back through the bridge, then clear it. The
 * cfg-write notify hook in glr_config_set fires per slug, but every
 * call hits its early returns (current step is not REQUIRE during
 * teardown, and we clear after restoring), so this is a quiet bulk
 * apply with no spurious tutorial advance. */
static void tutorial_cfg_baseline_restore(void) {
    if (!g_tut_cfg_baseline_valid) return;
    const ReplExportConfigBridge *b = repl_export_config_bridge();
    if (b && b->apply)
        b->apply(&g_tut_cfg_baseline_bag);
    tutorial_cfg_baseline_clear();
}

static void tutorial_store_result(TutorialMatchResult *dst,
                                  TutorialMatchResult result) {
    TutorialRuntimeState *state = tutorial_state_mut();

    if (dst)
        *dst = result;
    state->last_result = result;
}

static void tutorial_set_expected_message(TutorialMatchResult *result,
                                          TutorialMatchKind kind,
                                          const char *expected) {
    if (!result)
        return;

    result->kind = kind;
    result->arg_index = -1;
    if (!expected)
        expected = "";
    snprintf(result->message, sizeof(result->message), "expected: %s", expected);
}

/* Canonicalize a tutorial command for shape-only comparison: trim outer
 * whitespace, ignore a trailing semicolon, then remove all remaining
 * whitespace so formatting differences do not affect matching. */
static void tutorial_normalize_text(const char *src, char *dst, size_t dst_size) {
    size_t out = 0;
    const char *start = src ? src : "";
    const char *end = start + strlen(start);

    while (*start && isspace((unsigned char)*start))
        start++;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    if (end > start && end[-1] == ';') {
        end--;
        while (end > start && isspace((unsigned char)end[-1]))
            end--;
    }

    if (dst_size == 0)
        return;

    while (start < end && out + 1 < dst_size) {
        unsigned char ch = (unsigned char)*start++;
        if (isspace(ch))
            continue;
        dst[out++] = (char)ch;
    }
    dst[out] = '\0';
}

static int tutorial_append_locked_line(int line_idx) {
    TutorialRuntimeState *state = tutorial_state_mut();

    if (line_idx < 0)
        return 0;
    if (state->locked_line_count >= TUTORIAL_LOCKED_LINE_MAX)
        return 0;

    state->locked_lines[state->locked_line_count++] = line_idx;
    return 1;
}

static void tutorial_set_step_status(int tutorial_idx, int step) {
    int total = repl_tutorial_step_count(tutorial_idx);
    char msg[64];

    if (total <= 0)
        return;
    snprintf(msg, sizeof(msg), "Tutorial: step %d/%d", step + 1, total);
    repl_set_status(msg);
}

/* Shift any tutorial-tracked line at-or-after `pos` by `delta`. Used
 * before a runner-driven instruction-comment insert and after a
 * matched expected-command insert. Deliberately does NOT touch
 * pending.commit_line — that field is the immutable
 * snapshot of where the in-flight commit attempt targets, and the
 * success bookkeeping relies on reading it back unchanged after the
 * shift pass (the matched-commit path was added in Phase 3). */
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
    for (int i = 0; i < TUTORIAL_LOCKED_LINE_MAX; i++) {
        if (state->instruction_line_for_step[i] >= 0 &&
            state->instruction_line_for_step[i] >= pos)
            state->instruction_line_for_step[i] += delta;
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
     * supply the insertion index via &edit_line_inout; clear
     * insert mode; mark flat/normals dirty after loading. The
     * second `editor_state_edit_line_set(expected_commit_line)`
     * below overrides any post-load cursor advance, so the value
     * threaded here is purely for the insert position. */
    int loader_edit_line = instruction_line;
    editor_insert_mode_set(0);
    if (!repl_load_apply_line(comment, err, (int)sizeof(err), &loader_edit_line)) {
        /* Loader failed — undo the speculative shift so tracked
         * indices stay consistent with the unchanged document. */
        tutorial_shift_tracked_lines_from(instruction_line, -1);
        repl_set_status(err[0] ? err : "Tutorial instruction load failed");
        return 0;
    }

    repl_state_mark_flat_dirty();
    repl_state_mark_normals_dirty();
    state->fade_line_idx = instruction_line;
    state->fade_start_t = repl_state_variables().anim_time;
    /* Per-line duration at a fixed chars-per-second rate so every
     * instruction reveals at the same readable pace, regardless of
     * length. (line_len + SETTLE_CHARS) total slots / rate seconds —
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
    if (state->step >= 0 && state->step < TUTORIAL_LOCKED_LINE_MAX)
        state->instruction_line_for_step[state->step] = instruction_line;

    /* Cursor + expected_commit_line are set by the COMMAND branch in
     * tutorial_enter_step — SET/REQUIRE steps have no command for the
     * user to type and don't move the cursor / pin a commit row. */
    return 1;
}

/* Forward decls — used inside tutorial_enter_step / advance loop. */
static int tutorial_enter_step(int step);
static void tutorial_advance_loop(void);

/* Resolve where the next instruction comment for tutorial `idx`
 * step `step` should be inserted. For append placement that's the
 * current document_count; for label placement it's the row of the
 * target step's INSTRUCTION COMMENT — splicing above the comment
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
        *out_line = repl_state_document_count();
        return 1;
    }
    if (placement != TUTORIAL_STEP_LABEL) {
        repl_set_status("Tutorial step has unknown placement");
        return 0;
    }

    const char *target = repl_tutorial_step_target_label(tutorial_idx, step);
    if (!target || target[0] == '\0') {
        repl_set_status("Tutorial step target label is unresolved");
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
        repl_set_status("Tutorial step target label is unresolved");
        return 0;
    }

    TutorialRuntimeState state = tutorial_state_view();
    if (target_step >= TUTORIAL_LOCKED_LINE_MAX) {
        repl_set_status("Tutorial step target label is unresolved");
        return 0;
    }
    int line = state.instruction_line_for_step[target_step];
    if (line < 0 || line > repl_state_document_count()) {
        repl_set_status("Tutorial step target label is unresolved");
        return 0;
    }
    *out_line = line;
    return 1;
}

TutorialMatchResult tutorial_match(const char *expected, const char *got) {
    TutorialMatchResult result = {
        .kind = TUT_MATCH_OK,
        .arg_index = -1,
        .message = "",
    };
    char normalized_expected[256];
    char normalized_got[256];

    tutorial_normalize_text(expected, normalized_expected,
                            sizeof(normalized_expected));
    tutorial_normalize_text(got, normalized_got, sizeof(normalized_got));

    if (normalized_got[0] == '\0') {
        tutorial_set_expected_message(&result, TUT_MISMATCH_EMPTY, expected);
        return result;
    }
    if (strcmp(normalized_expected, normalized_got) == 0)
        return result;

    tutorial_set_expected_message(&result, TUT_MISMATCH_SHAPE, expected);
    return result;
}

/* Runtime slug validation: walk all SET/REQUIRE steps and every entry-
 * level @cfg slug; reject if the controller-installed config bridge
 * doesn't recognise one. Catches typos at tutorial_start time, before
 * any state has been mutated, so the runner never silently no-ops a
 * SET (or stalls a REQUIRE) on an unknown slug. Returns 1 on success;
 * on failure returns 0 and writes a diagnostic into `err`. */
static int tutorial_validate_slugs(int idx, char *err, int err_size) {
    char slug[REPL_EXPORT_CFG_KEY_MAX];
    const char *const *cfg = repl_tutorial_cfg_lines(idx);
    for (int i = 0; cfg && cfg[i]; i++) {
        if (!tutorial_extract_cfg_slug(cfg[i], slug, sizeof slug))
            continue;  /* mal-shaped @cfg line — repl parser will diag */
        if (!repl_cfg_known(slug)) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' @cfg uses unknown slug '%s'",
                         repl_tutorial_name(idx) ? repl_tutorial_name(idx) : "?",
                         slug);
            return 0;
        }
    }
    int n = repl_tutorial_step_count(idx);
    for (int s = 0; s < n; s++) {
        TutorialStepKind k = repl_tutorial_step_kind(idx, s);
        if (k != TUTORIAL_STEP_KIND_SET && k != TUTORIAL_STEP_KIND_REQUIRE)
            continue;
        const char *step_slug = repl_tutorial_step_cfg_slug(idx, s);
        if (!step_slug || !repl_cfg_known(step_slug)) {
            if (err_size > 0)
                snprintf(err, (size_t)err_size,
                         "tutorial '%s' step %d uses unknown cfg slug '%s'",
                         repl_tutorial_name(idx) ? repl_tutorial_name(idx) : "?",
                         s, step_slug ? step_slug : "(null)");
            return 0;
        }
    }
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

    /* Snapshot the user's true pre-tutorial cfg BEFORE any
     * tutorial-initiated mutation (transient-scene entry can flip cfg
     * via restore_pre_example_cfg_if_valid; presentation_reset(0) resets
     * scene presentation to example defaults; entry `@cfg` overrides
     * those; SET steps override further). `tutorial_teardown` writes
     * this back at every teardown path — exit, completion, workspace /
     * scene / example load, glr_app_reset_all — so the tutorial leaves
     * no scene-presentation footprint. */
    tutorial_capture_cfg_baseline(idx);

    repl_scenes_enter_transient_scene();
    repl_scenes_reset_for_transient();
    editor_completion_clear();
    tutorial_state_reset();

    /* Reset scene-presentation chrome to defaults, then apply any
     * tutorial leading `@cfg` lines through the export-config bridge —
     * same vocabulary and ordering the example loader uses, so a
     * tutorial is a self-contained scenario with predictable visual
     * state regardless of what the user was looking at before. Tag
     * mask 0 means no example-tag defaults are layered in. Both calls
     * degrade to no-ops when no host-effects sink / config bridge is
     * installed (some narrow REPL test environments). */
    repl_dispatch_example_presentation_reset(0);

    const char *const *cfg = repl_tutorial_cfg_lines(idx);
    for (int i = 0; cfg && cfg[i]; i++)
        repl_state_parse_workspace_header_line(cfg[i]);

    TutorialRuntimeState *state = tutorial_state_mut();
    state->active = 1;
    state->tutorial_idx = idx;
    state->step = 0;

    /* Enter step 0; the advance loop walks past any REQUIRE steps that
     * are already satisfied on entry (auto-advance) without recursion. */
    tutorial_advance_loop();
}

void tutorial_teardown(void) {
    if (!tutorial_active())
        return;
    tutorial_cfg_baseline_restore();
    tutorial_state_reset();
}

void tutorial_exit(void) {
    if (!tutorial_active())
        return;

    /* Set the status BEFORE teardown so it survives the reset and is
     * visible to the user. tutorial_teardown runs the cfg restore + state
     * reset together so a follow-on scene/workspace load (the next thing
     * the user usually does after exiting) doesn't stash a tutorial-
     * mutated cfg into the pre-workspace snapshot. */
    repl_set_status("Tutorial exited");
    tutorial_teardown();
    editor_completion_update();
}

int tutorial_handle_commit_attempt(const char *input, TutorialMatchResult *out) {
    TutorialMatchResult result;
    const char *expected = tutorial_current_expected_text();

    if (!tutorial_active() || !expected) {
        result = (TutorialMatchResult){
            .kind = TUT_MATCH_OK,
            .arg_index = -1,
            .message = "",
        };
        tutorial_store_result(out, result);
        return 1;
    }

    result = tutorial_match(expected, input);
    tutorial_store_result(out, result);
    return result.kind == TUT_MATCH_OK;
}

/* Enter the step at the CURRENT state->step. The advance loop owns the
 * step pointer; this function emits the instruction, applies any kind-
 * specific side effects, and returns one of:
 *    1  paused — waiting on user (COMMAND typing, SET ack key,
 *       REQUIRE state-change notify).
 *    0  auto-advance requested — REQUIRE step found already-satisfied.
 *       The loop bumps state->step and tries again.
 *   -1  terminal — either the catalog sentinel ("Tutorial complete") or
 *       an internal failure. teardown() has already run. */
static int tutorial_enter_step(int step) {
    TutorialRuntimeState *state = tutorial_state_mut();
    int idx = state->tutorial_idx;

    const char *comment = repl_tutorial_step_comment(idx, step);
    if (!comment) {
        /* Past the last step — tutorial complete. teardown restores
         * cfg + resets state; status set BEFORE teardown so it survives. */
        repl_set_status("Tutorial complete");
        tutorial_teardown();
        editor_completion_update();
        return -1;
    }

    int instruction_line = 0;
    if (!tutorial_step_instruction_line(idx, step, &instruction_line)) {
        tutorial_teardown();
        return -1;
    }
    if (!tutorial_emit_instruction_comment(comment, instruction_line)) {
        tutorial_teardown();
        return -1;
    }

    TutorialStepKind kind = repl_tutorial_step_kind(idx, step);
    switch (kind) {
    case TUTORIAL_STEP_KIND_COMMAND:
        /* Original typing setup: cursor on the row immediately below
         * the new instruction; insert mode iff mid-document. */
        state->expected_commit_line = instruction_line + 1;
        editor_state_edit_line_set(state->expected_commit_line);
        editor_insert_mode_set(state->expected_commit_line <
                               repl_state_document_count());
        tutorial_set_step_status(idx, step);
        /* Refresh autocomplete so the shadow ghost for the expected
         * command appears on the next frame, not the next keystroke. */
        editor_completion_update();
        return 1;
    case TUTORIAL_STEP_KIND_SET: {
        /* Showcase: apply the cfg so the user immediately sees the
         * effect, then wait for an ack key (Enter/Tab/Space). No
         * typing cursor — the document is read-only on this step
         * (tutorial_guard_source_change rejects non-COMMAND mutations).
         * Park the editor cursor on the virtual trailing row AFTER the
         * comment we just inserted; otherwise the editor renders the
         * empty input-buffer overlay on top of the comment row and the
         * instruction is invisible until the user presses a key. */
        const char *slug = repl_tutorial_step_cfg_slug(idx, step);
        int value = repl_tutorial_step_cfg_value(idx, step);
        repl_cfg_set_int(slug, value);
        state->expected_commit_line = -1;
        editor_state_edit_line_set(instruction_line + 1);
        editor_insert_mode_set((instruction_line + 1) <
                               repl_state_document_count());
        repl_set_status("Press Enter / Tab / Space to continue");
        editor_completion_update();
        return 1;
    }
    case TUTORIAL_STEP_KIND_REQUIRE: {
        /* Check: advance when the user themselves sets the slug to the
         * target. If already satisfied on entry, signal auto-advance to
         * the surrounding loop (no recursion — a chain of already-
         * satisfied REQUIREs can't blow the stack). */
        const char *slug = repl_tutorial_step_cfg_slug(idx, step);
        int target = repl_tutorial_step_cfg_value(idx, step);
        state->expected_commit_line = -1;
        /* Pick a fallback distinct from `target` so an unknown slug
         * (defensive — tutorial_start validated all slugs) can't be
         * mistaken for "already satisfied". */
        int probe_fb = (target == 0) ? -1 : 0;
        if (repl_cfg_get_int(slug, probe_fb) == target)
            return 0;  /* auto-advance via the loop */
        /* Park the cursor past the locked instruction-comment row so
         * the editor doesn't render the empty input overlay over it —
         * same fix as the SET branch above. */
        editor_state_edit_line_set(instruction_line + 1);
        editor_insert_mode_set((instruction_line + 1) <
                               repl_state_document_count());
        char msg[96];
        snprintf(msg, sizeof msg, "Set %s = %d to continue", slug, target);
        repl_set_status(msg);
        editor_completion_update();
        return 1;
    }
    }
    /* Unknown kind — validator should have rejected this. */
    tutorial_teardown();
    return -1;
}

/* Iterative advance: keep entering steps until one pauses for user input
 * (return 1) or the tutorial terminates (return -1). Replaces the prior
 * inline body of tutorial_advance_after_successful_commit so REQUIRE
 * auto-advance chains never recurse. */
static void tutorial_advance_loop(void) {
    TutorialRuntimeState *state = tutorial_state_mut();
    if (!tutorial_active()) return;
    while (1) {
        int r = tutorial_enter_step(state->step);
        if (r != 0) return;
        state->step++;
    }
}

void tutorial_advance_after_successful_commit(void) {
    TutorialRuntimeState *state;

    if (!tutorial_active())
        return;

    state = tutorial_state_mut();

    /* Label resolution anchors on the labeled step's instruction-comment
     * row (recorded at emit time), so no need to snapshot the just-
     * committed command row here. Clear the per-attempt scratch fields
     * and step++, then let the shared loop handle the next step. */
    state->pending.step_idx = -1;
    state->pending.commit_line = -1;
    state->pending.doc_count_before = -1;
    state->expected_commit_line = -1;
    state->step++;
    tutorial_advance_loop();
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
    if (repl_tutorial_step_kind(state.tutorial_idx, state.step) !=
        TUTORIAL_STEP_KIND_REQUIRE)
        return;
    const char *slug = repl_tutorial_step_cfg_slug(state.tutorial_idx, state.step);
    int target = repl_tutorial_step_cfg_value(state.tutorial_idx, state.step);
    if (!slug) return;
    int probe_fb = (target == 0) ? -1 : 0;
    if (repl_cfg_get_int(slug, probe_fb) != target)
        return;
    /* Match: advance. Clear pending (REQUIRE has no commit attempt in
     * flight, but be defensive). */
    TutorialRuntimeState *m = tutorial_state_mut();
    m->pending.step_idx = -1;
    m->pending.commit_line = -1;
    m->pending.doc_count_before = -1;
    m->expected_commit_line = -1;
    m->step++;
    tutorial_advance_loop();
}

int tutorial_handle_ack_key(unsigned char key) {
    if (!tutorial_active())
        return 0;
    if (tutorial_current_step_kind() != TUTORIAL_STEP_KIND_SET)
        return 0;
    if (key != '\r' && key != '\n' && key != '\t' && key != ' ')
        return 0;
    TutorialRuntimeState *m = tutorial_state_mut();
    m->pending.step_idx = -1;
    m->pending.commit_line = -1;
    m->pending.doc_count_before = -1;
    m->expected_commit_line = -1;
    m->step++;
    tutorial_advance_loop();
    return 1;
}

int tutorial_block_noncommand_commit(void) {
    if (!tutorial_active())
        return 0;
    TutorialStepKind k = tutorial_current_step_kind();
    if (k == TUTORIAL_STEP_KIND_SET) {
        repl_set_status("Press Enter / Tab / Space to continue");
        return 1;
    }
    if (k == TUTORIAL_STEP_KIND_REQUIRE) {
        TutorialRuntimeState s = tutorial_state_view();
        const char *slug = repl_tutorial_step_cfg_slug(s.tutorial_idx, s.step);
        int target = repl_tutorial_step_cfg_value(s.tutorial_idx, s.step);
        char msg[96];
        snprintf(msg, sizeof msg, "Set %s = %d to continue",
                 slug ? slug : "?", target);
        repl_set_status(msg);
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
 * takes one slot to fade in (alpha 0 → 1, bright white), and the
 * trailing W slots pace its color settling from white back to the
 * base comment color. The last character finishes settling exactly
 * at fade_start_t + fade_duration. */
static float tutorial_fade_slot_step(float fade_duration, int line_len) {
    int safe_len = line_len > 0 ? line_len : 1;
    int total_slots = safe_len + TUTORIAL_FADE_SETTLE_CHARS;
    return fade_duration / (float)total_slots;
}

int tutorial_step_fade_front(int line_idx, int line_len, float now) {
    TutorialRuntimeState state = tutorial_state_view();
    int safe_len;
    float step;
    float elapsed;
    int front;

    if (!state.active || line_idx != state.fade_line_idx)
        return -1;
    if (now >= state.fade_start_t + state.fade_duration)
        return -1;

    safe_len = line_len > 0 ? line_len : 1;
    step = tutorial_fade_slot_step(state.fade_duration, safe_len);
    if (step <= 0.0f)
        return -1;

    elapsed = now - state.fade_start_t;
    if (elapsed <= 0.0f)
        return 0;

    front = (int)(elapsed / step);
    if (front < 0)
        front = 0;
    if (front >= safe_len)
        return -1;
    return front;
}

float tutorial_step_fade_alpha(int line_idx, int char_idx, int line_len, float now) {
    TutorialRuntimeState state = tutorial_state_view();
    int safe_len;
    float step;
    float elapsed;

    if (!state.active || line_idx != state.fade_line_idx)
        return 1.0f;
    if (now >= state.fade_start_t + state.fade_duration)
        return 1.0f;

    safe_len = line_len > 0 ? line_len : 1;
    if (char_idx < 0)
        char_idx = 0;
    if (char_idx >= safe_len)
        char_idx = safe_len - 1;

    step = tutorial_fade_slot_step(state.fade_duration, safe_len);
    if (step <= 0.0f)
        return 1.0f;
    elapsed = (now - state.fade_start_t) - (float)char_idx * step;
    return clamp01(elapsed / step);
}

float tutorial_step_fade_settle(int line_idx, int char_idx, int line_len, float now) {
    TutorialRuntimeState state = tutorial_state_view();
    int safe_len;
    float step;
    float settle_duration;
    float elapsed;

    if (!state.active || line_idx != state.fade_line_idx)
        return 1.0f;
    if (now >= state.fade_start_t + state.fade_duration)
        return 1.0f;

    safe_len = line_len > 0 ? line_len : 1;
    if (char_idx < 0)
        char_idx = 0;
    if (char_idx >= safe_len)
        char_idx = safe_len - 1;

    step = tutorial_fade_slot_step(state.fade_duration, safe_len);
    settle_duration = step * (float)TUTORIAL_FADE_SETTLE_CHARS;
    if (settle_duration <= 0.0f)
        return 1.0f;
    /* Elapsed time since this char finished its reveal slot. */
    elapsed = (now - state.fade_start_t) - (float)(char_idx + 1) * step;
    return clamp01(elapsed / settle_duration);
}

int tutorial_line_is_fading(int line_idx, float now) {
    TutorialRuntimeState state = tutorial_state_view();

    return state.active && line_idx == state.fade_line_idx &&
           now < state.fade_start_t + state.fade_duration;
}

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

int tutorial_shadow_suffix(const char *input, char *out, size_t out_size) {
    const char *expected;
    size_t input_len;

    if (out && out_size > 0)
        out[0] = '\0';
    if (!out || out_size == 0)
        return 0;
    if (!tutorial_active())
        return 0;

    expected = tutorial_current_expected_text();
    if (!expected)
        return 0;

    input_len = input ? strlen(input) : 0;
    if (strncmp(expected, input ? input : "", input_len) != 0)
        return 0;

    snprintf(out, out_size, "%s", expected + input_len);
    return 1;
}

int tutorial_guard_source_change(int pos, int delete_count, int insert_count) {
    TutorialRuntimeState state = tutorial_state_view();

    if (!state.active)
        return 1;
    if (delete_count < 0 || insert_count < 0)
        return 0;
    if (delete_count == 0 && insert_count == 0)
        return 1;

    /* SET / REQUIRE steps freeze the document: no expected_commit_line
     * and no locked-line region to anchor to past the last instruction,
     * so paste/delete/comment-toggle/etc. would otherwise slip through
     * after the last locked row. Reject every non-no-op mutation while
     * a non-COMMAND step is active; the editor precheck pairs this with
     * a kind-aware commit hint (tutorial_block_noncommand_commit). */
    if (repl_tutorial_step_kind(state.tutorial_idx, state.step) !=
        TUTORIAL_STEP_KIND_COMMAND)
        return 0;

    /* Narrow allow-list for the in-flight matched expected commit:
     * only the matched step may insert at its captured commit row,
     * and only as a pure insert (delete_count == 0). Keying off the
     * immutable pending.commit_line — rather than the ambient
     * expected_commit_line — keeps the exception scoped to the one
     * commit attempt the precheck already authorized. */
    if (state.pending.step_idx >= 0 && delete_count == 0 &&
        insert_count > 0 && pos == state.pending.commit_line)
        return 1;

    /* Block any other mutation that would land at the row reserved
     * for the next expected user commit. Without this, a paste or
     * other non-precheck mutation (which never set `pending`) at
     * expected_commit_line would slip in untracked when the target
     * label refers to a step whose committed row has no later
     * locked instruction comment below it — e.g. when the
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
    state->pending.step_idx = -1;
    state->pending.commit_line = -1;
    state->pending.doc_count_before = -1;
}

void tutorial_note_expected_commit_applied(void) {
    TutorialRuntimeState *state = tutorial_state_mut();

    if (state->pending.step_idx < 0)
        return;

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

    state->pending.step_idx = -1;
    state->pending.commit_line = -1;
    state->pending.doc_count_before = -1;
}
