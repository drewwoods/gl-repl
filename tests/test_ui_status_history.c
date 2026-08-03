/*
 * test_ui_status_history.c - Recent-message history ring + messages-button
 * geometry/hit-test for the recent-message status panel.
 *
 * GL_STUBS-only: the geometry/render bits drive the stub GL path, the ring
 * bits are pure state.
 */
#ifdef GL_STUBS
#include "app/glr_state.h"
#include "app/glr_ctrl.h"
#include "ui/app/panels.h"
#include "ui/app/state.h"
#include "ui/app/hit.h"
#include "ui/app/layout.h"
#include "ui/app/snapshot.h"
#include "ui/core/metrics.h"
#include "support/test_harness.h"
#include <GL/gl_stub_counts.h>
#endif

#include <stdio.h>
#include <string.h>

#ifdef GL_STUBS
static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT_EQ(label, got, exp) \
    TEST_ASSERT_INT(&g_harness, label, got, exp)
#define ASSERT_STR_EQ(label, got, exp) \
    TEST_ASSERT_STR(&g_harness, label, got, exp)

/* Newest -> oldest helpers over the value-copied ring. */
static const UiStatusEntry *newest(const UiStatusHistory *h) {
    int idx = ui_status_history_index(h, h->count - 1);
    return idx < 0 ? NULL : &h->entries[idx];
}
static const UiStatusEntry *oldest(const UiStatusHistory *h) {
    int idx = ui_status_history_index(h, 0);
    return idx < 0 ? NULL : &h->entries[idx];
}

static void test_ring_order_and_cap(void) {
    UiStatusHistory h;
    char buf[32];
    int i;

    ui_state_reset();
    for (i = 0; i < UI_STATUS_HISTORY_CAP + 4; i++) {
        snprintf(buf, sizeof(buf), "msg-%02d", i);
        ui_state_status_set(buf);
    }

    h = ui_state_status_history();
    ASSERT_INT_EQ("ring caps at capacity", h.count, UI_STATUS_HISTORY_CAP);

    /* Oldest survivor is msg-04 (0..3 evicted); newest is the last push. */
    ASSERT_STR_EQ("oldest after overflow", oldest(&h)->text, "msg-04");
    snprintf(buf, sizeof(buf), "msg-%02d", UI_STATUS_HISTORY_CAP + 3);
    ASSERT_STR_EQ("newest is last push", newest(&h)->text, buf);

    /* seq is monotonically increasing oldest -> newest. */
    {
        int ok = 1, k;
        for (k = 1; k < h.count; k++) {
            int a = ui_status_history_index(&h, k - 1);
            int b = ui_status_history_index(&h, k);
            if (h.entries[b].seq <= h.entries[a].seq)
                ok = 0;
        }
        ASSERT_TRUE("seq increases oldest->newest", ok);
    }
}

static void test_consecutive_dedup(void) {
    UiStatusHistory h;

    /* A message re-set while the previous identical one is still live is a
     * refresh (no new entry); a re-occurrence after it has expired
     * (ttl==0) collapses into the same entry with a bumped count. We force
     * the expiry directly since there's no controller tick in the test. */
    ui_state_reset();
    ui_state_status_set("same");
    ui_state_status_mut()->ttl = 0;
    ui_state_status_set("same");
    ui_state_status_mut()->ttl = 0;
    ui_state_status_set("same");

    h = ui_state_status_history();
    ASSERT_INT_EQ("repeat occurrences collapse to one entry", h.count, 1);
    ASSERT_INT_EQ("dup_count counts the occurrences", (int)newest(&h)->dup_count, 3);

    /* A live re-emit of the same text does NOT add an entry or bump count. */
    ui_state_status_set("same");   /* ttl is live here */
    h = ui_state_status_history();
    ASSERT_INT_EQ("live refresh adds no entry", h.count, 1);
    ASSERT_INT_EQ("live refresh does not bump count",
                  (int)newest(&h)->dup_count, 3);

    /* A different message breaks the run; a return to "same" is a new entry. */
    ui_state_status_set("other");
    ui_state_status_set("same");
    h = ui_state_status_history();
    ASSERT_INT_EQ("non-consecutive dup is a fresh entry", h.count, 3);
    ASSERT_INT_EQ("fresh entry dup_count resets", (int)newest(&h)->dup_count, 1);
}

static void test_kind_preserved(void) {
    UiStatusHistory h;

    ui_state_reset();
    ui_state_status_set("info-line");
    ui_state_status_set_error("error-line");

    h = ui_state_status_history();
    ASSERT_INT_EQ("two entries with distinct text", h.count, 2);
    ASSERT_INT_EQ("newest is ERROR", newest(&h)->kind, UI_STATUS_ERROR);
    ASSERT_INT_EQ("oldest is INFO", oldest(&h)->kind, UI_STATUS_INFO);

    /* Same text but different kind does NOT dedup. */
    ui_state_status_set("dual");
    ui_state_status_set_error("dual");
    h = ui_state_status_history();
    ASSERT_INT_EQ("same text different kind: two entries", h.count, 4);
}

static void test_refresh_preserves_age(void) {
    /* The telescope animation keys off status.age; a same-text live refresh
     * must keep it (so a per-frame re-emit stays extended), a real change
     * must reset it. */
    ui_state_reset();
    ui_state_status_set("hint");
    ui_state_status_mut()->age = 50;        /* pretend the controller aged it */

    ui_state_status_set("hint");            /* live refresh */
    ASSERT_INT_EQ("refresh keeps age", ui_state_status().age, 50);
    ASSERT_TRUE("refresh keeps ttl alive", ui_state_status().ttl > 0);

    ui_state_status_set("different");        /* real change */
    ASSERT_INT_EQ("new message resets age", ui_state_status().age, 0);

    /* Kind change with same text also counts as new. */
    ui_state_status_mut()->age = 30;
    ui_state_status_set_error("different");
    ASSERT_INT_EQ("kind change resets age", ui_state_status().age, 0);
}

static void test_reset_clears(void) {
    UiStatusHistory h;
    ui_state_reset();
    ui_state_status_set("x");
    ui_state_status_history_set_open(1);
    ui_state_reset();
    h = ui_state_status_history();
    ASSERT_INT_EQ("reset clears the ring", h.count, 0);
    ASSERT_INT_EQ("reset closes the list", h.open, 0);
}

static void test_toggle(void) {
    ui_state_reset();
    ASSERT_INT_EQ("starts closed", ui_state_status_history().open, 0);
    ui_state_status_history_toggle();
    ASSERT_INT_EQ("toggle opens", ui_state_status_history().open, 1);
    ui_state_status_history_toggle();
    ASSERT_INT_EQ("toggle closes", ui_state_status_history().open, 0);
}

/* Build a minimal snapshot with a live scene layout, the way test_ui_panels
 * does, so the button geometry helper has a real scene rect. */
static void prime_layout(void) {
    glr_ctrl_reset_all();
    ui_state_reset();
    ui_state_viewport_set_size(800, 600);
    glr_state_presentation_mut()->code_panel_layout = CODE_PANEL_LAYOUT_LEFT;
    glr_ctrl_sync_ui_chrome();
    ui_state_code_panel_mut()->panel_frac = 0.4f;
}

static void fill_snap(UiRenderSnapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->viewport = ui_state_viewport();
    snap->code_panel = ui_state_code_panel();
    snap->status_history = ui_state_status_history();
}

static void test_button_geometry(void) {
    UiRenderSnapshot snap;
    int sc_x, sc_y, sc_w, sc_h;
    int bx, by, bw, bh;

    prime_layout();
    ui_state_status_set("seed");   /* button only appears once history exists */
    fill_snap(&snap);

    ASSERT_TRUE("button rect exists for a live scene",
                ui_panels_status_history_button_rect(&snap, &bx, &by, &bw, &bh));

    ui_layout_scene_rect(&sc_x, &sc_y, &sc_w, &sc_h);
    ASSERT_INT_EQ("button flush to scene right edge", bx + bw, sc_x + sc_w);
    ASSERT_INT_EQ("button sits on the status band", by, sc_y);
    ASSERT_INT_EQ("button height is the status bar", bh, STATUSBAR_H);
    ASSERT_TRUE("button has positive width", bw > 0);

    /* Suppressed while a modal prompt owns the bottom strip. */
    snap.rename_active = 1;
    ASSERT_TRUE("no button during rename modal",
                !ui_panels_status_history_button_rect(&snap, &bx, &by, &bw, &bh));
    snap.rename_active = 0;
    snap.file_prompt_active = 1;
    ASSERT_TRUE("no button during file prompt",
                !ui_panels_status_history_button_rect(&snap, &bx, &by, &bw, &bh));

    /* Empty history -> no button at all. */
    snap.file_prompt_active = 0;
    snap.status_history.count = 0;
    ASSERT_TRUE("no button when history is empty",
                !ui_panels_status_history_button_rect(&snap, &bx, &by, &bw, &bh));
}

static void test_button_hit_and_render(void) {
    UiRenderSnapshot snap;
    int bx, by, bw, bh, mx, my;
    UiHit hit;

    prime_layout();
    ui_state_status_set("hello");
    fill_snap(&snap);

    ASSERT_TRUE("button rect exists",
                ui_panels_status_history_button_rect(&snap, &bx, &by, &bw, &bh));

    /* Center of the button, in window (top-left) coords. */
    mx = bx + bw / 2;
    my = snap.viewport.window_h - (by + bh / 2);
    hit = ui_panels_hit_test(&snap, mx, my, 0);
    ASSERT_INT_EQ("button click classified as status history",
                  hit.kind, UI_HIT_STATUS_HISTORY);
    ASSERT_INT_EQ("button hit carries the toggle marker", hit.item_idx, 1);

    /* The button draws unconditionally (even with no active banner). */
    gl_stub_counts_reset();
    ui_panels_render_scene_status(&snap);
    ASSERT_TRUE("button render emits a rect",
                gl_stub_counts[GL_STUB_glRectf] > 0);
    ASSERT_TRUE("button render draws its label",
                gl_stub_counts[GL_STUB_glutBitmapString] > 0);
}

static void test_open_list_renders_and_consumes(void) {
    UiRenderSnapshot snap;
    int i;

    prime_layout();
    for (i = 0; i < 5; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "line-%d", i);
        ui_state_status_set(buf);
    }
    ui_state_status_history_set_open(1);
    fill_snap(&snap);

    gl_stub_counts_reset();
    ui_panels_render_scene_status(&snap);
    ASSERT_TRUE("open list draws multiple rects (panel + button)",
                gl_stub_counts[GL_STUB_glRectf] >= 2);
}

int main(void) {
    test_ring_order_and_cap();
    test_consecutive_dedup();
    test_kind_preserved();
    test_refresh_preserves_age();
    test_reset_clears();
    test_toggle();
    test_button_geometry();
    test_button_hit_and_render();
    test_open_list_renders_and_consumes();
    return test_harness_report(&g_harness, "test_ui_status_history");
}
#else
int main(void) {
    printf("test_ui_status_history: SKIPPED (needs GL_STUBS)\n");
    return 0;
}
#endif
