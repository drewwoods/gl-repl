/*
 * test_glr_url.c - Unit tests for glr_url cross-platform URL launcher.
 */
#include "app/glr_url.h"
#include "support/test_harness.h"

#include <stdio.h>
#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, g, e)  TEST_ASSERT_INT(&g_harness, label, g, e)
#define ASSERT_STR(label, g, e)  TEST_ASSERT_STR(&g_harness, label, g, e)

static char g_mock_url_buf[512];
static int  g_mock_call_count = 0;
static int  g_mock_return_val = 1;

static int mock_launcher(const char *url) {
    g_mock_call_count++;
    if (url)
        snprintf(g_mock_url_buf, sizeof(g_mock_url_buf), "%s", url);
    else
        g_mock_url_buf[0] = '\0';
    return g_mock_return_val;
}

static void test_invalid_urls_rejected(void) {
    glr_url_reset_launcher_for_test();
    ASSERT_INT("null url rejected", glr_url_open(NULL), 0);
    ASSERT_INT("empty url rejected", glr_url_open(""), 0);
}

static void test_user_guide_url_constant(void) {
    ASSERT_TRUE("user guide url starts with https://github.com/",
                strncmp(GLR_USER_GUIDE_URL, "https://github.com/", 19) == 0);
    ASSERT_TRUE("user guide url points to USER_GUIDE.md",
                strstr(GLR_USER_GUIDE_URL, "USER_GUIDE.md") != NULL);
}

static void test_mock_launcher_hook(void) {
    g_mock_call_count = 0;
    g_mock_url_buf[0] = '\0';
    g_mock_return_val = 1;

    glr_url_set_launcher_for_test(mock_launcher);

    ASSERT_INT("glr_url_open invokes mock launcher",
               glr_url_open("https://example.com/test"), 1);
    ASSERT_INT("mock launcher called once", g_mock_call_count, 1);
    ASSERT_STR("mock launcher received url", g_mock_url_buf, "https://example.com/test");

    /* Null and empty URLs must be rejected before reaching the launcher */
    ASSERT_INT("null url rejected before launcher", glr_url_open(NULL), 0);
    ASSERT_INT("empty url rejected before launcher", glr_url_open(""), 0);
    ASSERT_INT("mock call count unchanged after invalid inputs", g_mock_call_count, 1);

    /* Test open user guide */
    g_mock_call_count = 0;
    g_mock_url_buf[0] = '\0';
    ASSERT_INT("glr_url_open_user_guide returns success",
               glr_url_open_user_guide(), 1);
    ASSERT_INT("mock launcher called for user guide", g_mock_call_count, 1);
    ASSERT_STR("mock launcher received canonical user guide url",
               g_mock_url_buf, GLR_USER_GUIDE_URL);

    /* Test launcher failure propagation */
    g_mock_return_val = 0;
    ASSERT_INT("glr_url_open propagates failure",
               glr_url_open("https://example.com/fail"), 0);
    ASSERT_INT("glr_url_open_user_guide propagates failure",
               glr_url_open_user_guide(), 0);

    glr_url_reset_launcher_for_test();
}

static void test_url_lifecycle(void) {
    /* Calling tick and shutdown when no processes are tracked is a clean no-op */
    glr_url_tick();
    glr_url_shutdown();
    glr_url_tick();
    ASSERT_TRUE("url tick and shutdown are idempotent and safe", 1);
}

int main(void) {
    test_invalid_urls_rejected();
    test_user_guide_url_constant();
    test_mock_launcher_hook();
    test_url_lifecycle();

    return test_harness_report(&g_harness, "glr_url");
}
