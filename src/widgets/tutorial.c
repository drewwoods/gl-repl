#include "widgets/tutorial.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "editor/completion.h"
#include "editor/state.h"
#include "repl/core.h"
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
    tutorial_append_locked_line(instruction_line);

    /* Record where this step's instruction comment landed so a
     * later label-targeted step can splice ABOVE this comment
     * (rather than between the comment and the user's commit row),
     * keeping the original (instruction, command) pair adjacent. */
    if (state->step >= 0 && state->step < TUTORIAL_LOCKED_LINE_MAX)
        state->instruction_line_for_step[state->step] = instruction_line;

    /* Place the cursor on the row the user is expected to commit on
     * (the row immediately below the new instruction) and enable
     * insert mode iff that row is mid-document — otherwise it's the
     * trailing row and append mode is correct. */
    state->expected_commit_line = instruction_line + 1;
    editor_state_edit_line_set(state->expected_commit_line);
    editor_insert_mode_set(state->expected_commit_line <
                           repl_state_document_count());
    return 1;
}

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

void tutorial_start(int idx) {
    char err[TUTORIAL_STATUS_MAX] = "";

    if (idx < 0 || idx >= repl_tutorial_count()) {
        repl_set_status("Tutorial index out of range");
        return;
    }

    /* Validate the full catalog entry BEFORE mutating any state so a
     * malformed tutorial cannot leave the editor in a half-applied
     * transient scene. */
    if (!repl_tutorial_validate(idx, err, (int)sizeof(err))) {
        repl_set_status(err[0] ? err : "Tutorial catalog validation failed");
        return;
    }

    repl_scenes_enter_transient_scene();
    repl_scenes_reset_for_transient();
    editor_completion_clear();
    tutorial_state_reset();

    TutorialRuntimeState *state = tutorial_state_mut();
    state->active = 1;
    state->tutorial_idx = idx;
    state->step = 0;

    int instruction_line = 0;
    if (!tutorial_step_instruction_line(idx, 0, &instruction_line)) {
        tutorial_state_reset();
        return;
    }
    if (!tutorial_emit_instruction_comment(repl_tutorial_step_comment(idx, 0),
                                           instruction_line)) {
        tutorial_state_reset();
        return;
    }
    tutorial_set_step_status(idx, 0);
    /* Refresh the autocomplete provider so the shadow ghost text for
     * step 0's expected command appears on the very first frame
     * instead of waiting for the user's next keystroke. */
    editor_completion_update();
}

void tutorial_exit(void) {
    if (!tutorial_active())
        return;

    tutorial_state_reset();
    repl_set_status("Tutorial exited");
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

void tutorial_advance_after_successful_commit(void) {
    TutorialRuntimeState *state;
    const char *comment;

    if (!tutorial_active())
        return;

    state = tutorial_state_mut();

    /* Label resolution now anchors on the labeled step's
     * instruction-comment row (recorded at emit time, see
     * tutorial_emit_instruction_comment), so we no longer need to
     * snapshot the just-committed command row here. Just clear the
     * per-attempt scratch fields and advance. */
    state->pending.step_idx = -1;
    state->pending.commit_line = -1;
    state->pending.doc_count_before = -1;
    state->expected_commit_line = -1;

    state->step++;
    comment = repl_tutorial_step_comment(state->tutorial_idx, state->step);
    if (!comment) {
        tutorial_state_reset();
        repl_set_status("Tutorial complete");
        /* Refresh autocomplete so the lingering shadow ghost from the
         * final step clears immediately rather than persisting until
         * the user's next keystroke. */
        editor_completion_update();
        return;
    }

    int instruction_line = 0;
    if (!tutorial_step_instruction_line(state->tutorial_idx, state->step,
                                        &instruction_line)) {
        tutorial_state_reset();
        return;
    }
    if (!tutorial_emit_instruction_comment(comment, instruction_line))
        return;
    tutorial_set_step_status(state->tutorial_idx, state->step);
    /* The semicolon route called editor_completion_clear() before this
     * advance runs; without the explicit refresh, the next step's
     * shadow ghost wouldn't appear until the user types again. */
    editor_completion_update();
}

const char *tutorial_current_expected_text(void) {
    TutorialRuntimeState state = tutorial_state_view();

    if (!state.active)
        return NULL;
    return repl_tutorial_step_expected(state.tutorial_idx, state.step);
}

int tutorial_step_fade_front(int line_idx, int line_len, float now) {
    TutorialRuntimeState state = tutorial_state_view();
    int safe_len;
    float per_char_window;
    float elapsed;

    if (!state.active || line_idx != state.fade_line_idx)
        return -1;
    if (now >= state.fade_start_t + state.fade_duration)
        return -1;

    safe_len = line_len > 0 ? line_len : 1;
    per_char_window = state.fade_duration / (float)safe_len;
    if (per_char_window <= 0.0f)
        return -1;

    elapsed = now - state.fade_start_t;
    if (elapsed <= 0.0f)
        return 0;

    int front = (int)(elapsed / per_char_window);
    if (front < 0)
        front = 0;
    if (front >= safe_len)
        return -1;
    return front;
}

float tutorial_step_fade_alpha(int line_idx, int char_idx, int line_len, float now) {
    TutorialRuntimeState state = tutorial_state_view();
    int safe_len;
    float per_char_window;
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

    per_char_window = state.fade_duration / (float)safe_len;
    elapsed = (now - state.fade_start_t) - (float)char_idx * per_char_window;
    if (per_char_window <= 0.0f)
        return 1.0f;
    return clamp01(elapsed / per_char_window);
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
