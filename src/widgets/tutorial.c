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

static int tutorial_emit_instruction_comment(const char *comment) {
    char err[TUTORIAL_STATUS_MAX] = "";
    TutorialRuntimeState *state = tutorial_state_mut();

    if (!comment || !comment[0])
        return 0;

    /* repl_load_apply_line caller contract (src/repl/load.h): set
     * edit_line to document_count, clear insert mode, then mark both
     * flat and normals dirty after loading. */
    repl_state_edit_line_set(repl_state_document_count());
    editor_insert_mode_set(0);
    if (!repl_load_apply_line(comment, err, (int)sizeof(err))) {
        repl_set_status(err[0] ? err : "Tutorial instruction load failed");
        return 0;
    }

    repl_state_mark_flat_dirty();
    repl_state_mark_normals_dirty();
    state->fade_line_idx = repl_state_document_count() - 1;
    state->fade_start_t = repl_state_variables().anim_time;
    tutorial_append_locked_line(state->fade_line_idx);
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
    if (idx < 0 || idx >= repl_tutorial_count()) {
        repl_set_status("Tutorial index out of range");
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

    if (!tutorial_emit_instruction_comment(repl_tutorial_step_comment(idx, 0))) {
        tutorial_state_reset();
        return;
    }
}

void tutorial_exit(void) {
    if (!tutorial_active())
        return;

    tutorial_state_reset();
    repl_set_status("Tutorial exited");
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
    state->step++;
    comment = repl_tutorial_step_comment(state->tutorial_idx, state->step);
    if (!comment) {
        tutorial_state_reset();
        repl_set_status("Tutorial complete");
        return;
    }

    tutorial_emit_instruction_comment(comment);
}

const char *tutorial_current_expected_text(void) {
    TutorialRuntimeState state = tutorial_state_view();

    if (!state.active)
        return NULL;
    return repl_tutorial_step_expected(state.tutorial_idx, state.step);
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

int tutorial_guard_source_change(int pos, int delete_count, int insert_count) {
    TutorialRuntimeState state = tutorial_state_view();

    if (!state.active)
        return 1;
    if (delete_count < 0 || insert_count < 0)
        return 0;
    if (delete_count == 0 && insert_count == 0)
        return 1;

    for (int i = 0; i < state.locked_line_count; i++) {
        if (pos <= state.locked_lines[i])
            return 0;
    }
    return 1;
}
