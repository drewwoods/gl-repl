#include "editor/state.h"
#include "editor/completion.h"
#include "support/test_harness.h"
#include <stddef.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) \
    TEST_ASSERT_TRUE(&g_harness, label, cond)

#define ASSERT_PTR_EQ(label, got, exp) \
    TEST_ASSERT_TRUE(&g_harness, label, got == exp)

static int g_update_called = 0;
static int g_update_selected_preview_called = 0;
static int g_clear_called = 0;

static void test_update(void) { g_update_called++; }
static void test_update_selected_preview(void) { g_update_selected_preview_called++; }
static void test_clear(void) { g_clear_called++; }

static EditorCompletionProvider g_test_provider = {
    .update = test_update,
    .update_selected_preview = test_update_selected_preview,
    .clear = test_clear
};

int main() {
    printf("--- editor_completion tests ---\n");

    /* 1. Initial state */
    {
        editor_completion_register(NULL);
        ASSERT_PTR_EQ("initial provider is NULL", editor_completion_provider(), NULL);
    }

    /* 2. Registration */
    {
        editor_completion_register(&g_test_provider);
        ASSERT_PTR_EQ("registered provider matches", editor_completion_provider(), &g_test_provider);
    }

    /* 3. Dispatch: update */
    {
        g_update_called = 0;
        editor_completion_update();
        ASSERT_TRUE("update called on provider", g_update_called == 1);
    }

    /* 4. Dispatch: update_selected_preview */
    {
        g_update_selected_preview_called = 0;
        editor_completion_update_selected_preview();
        ASSERT_TRUE("update_selected_preview called on provider", g_update_selected_preview_called == 1);
    }

    /* 5. Dispatch: clear */
    {
        g_clear_called = 0;
        editor_completion_clear();
        ASSERT_TRUE("clear called on provider", g_clear_called == 1);
    }

    /* 6. NULL safety */
    {
        editor_completion_register(NULL);
        g_update_called = 0;
        g_update_selected_preview_called = 0;
        g_clear_called = 0;
        
        editor_completion_update();
        editor_completion_update_selected_preview();
        editor_completion_clear();
        
        ASSERT_TRUE("update not called when NULL", g_update_called == 0);
        ASSERT_TRUE("update_selected_preview not called when NULL", g_update_selected_preview_called == 0);
        ASSERT_TRUE("clear not called when NULL", g_clear_called == 0);
    }

    /* 7. Partial provider (NULL functions) */
    {
        EditorCompletionProvider partial = {0};
        editor_completion_register(&partial);
        
        /* Should not crash */
        editor_completion_update();
        editor_completion_update_selected_preview();
        editor_completion_clear();
        ASSERT_TRUE("partial provider handled safely", 1);
    }

    printf("\n");
    return test_harness_report(&g_harness, "test_editor_completion");
}
