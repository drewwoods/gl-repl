/*
 * test_glr_telemetry.c - unit tests for the usage beacon's name sanitizer.
 *
 * The beacon itself only exists on the web build, but the rule about what may
 * become an event name is exactly the part worth asserting on the development
 * machine: these names become dimension values in a dashboard, and the two
 * ways to ruin that set are unbounded cardinality (a user-authored string
 * leaking in) and unstable spelling (the same example forking into several
 * series because its label carries punctuation).
 */
#include "app/glr_telemetry.h"
#include "support/test_harness.h"

#include <string.h>

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_STR(label, g, e)  TEST_ASSERT_STR(&g_harness, label, g, e)

static const char *slug(const char *src) {
    static char buf[GLR_TELEMETRY_NAME_MAX];
    glr_telemetry_slugify(src, buf, sizeof(buf));
    return buf;
}

static void test_plain_names_pass_through(void) {
    ASSERT_STR("simple name unchanged", slug("boot"), "boot");
    ASSERT_STR("slash separator kept", slug("example/Torus"), "example/Torus");
    ASSERT_STR("digits and underscore kept", slug("v2_x"), "v2_x");
    ASSERT_STR("dot kept", slug("error/webgl.lost"), "error/webgl.lost");
}

static void test_punctuation_collapses(void) {
    /* The catalog's real worst case: spaces and parens in one label. */
    ASSERT_STR("spaces and parens become single dashes",
               slug("Wave surface (analytic normals)"),
               "Wave-surface-analytic-normals");
    ASSERT_STR("runs collapse to one dash", slug("a   ---   b"), "a-b");
    ASSERT_STR("leading punctuation dropped", slug("   lead"), "lead");
    ASSERT_STR("trailing punctuation dropped", slug("trail!!!"), "trail");
    ASSERT_STR("punctuation only yields empty", slug("!!! ???"), "");
}

static void test_hostile_input_is_neutralized(void) {
    /* The name is concatenated into a URL by the shim. Nothing that could
     * add a query parameter or escape an attribute may survive. */
    ASSERT_STR("query separators stripped",
               slug("a&e=true&p=b"), "a-e-true-p-b");
    ASSERT_STR("quotes and angle brackets stripped",
               slug("x\"><script>"), "x-script");
    ASSERT_STR("percent escapes stripped", slug("a%2Fb"), "a-2Fb");
    ASSERT_STR("newlines stripped", slug("a\nb\tc"), "a-b-c");
    /* High bytes (a UTF-8 scene name) are non-ASCII, so they collapse
     * rather than reaching the wire half-escaped. */
    ASSERT_STR("non-ASCII collapses", slug("caf\xc3\xa9 scene"), "caf-scene");
}

static void test_bounds(void) {
    char tiny[4];
    char one[1];

    ASSERT_STR("NULL source yields empty", slug(NULL), "");
    ASSERT_STR("empty source yields empty", slug(""), "");

    glr_telemetry_slugify("abcdefgh", tiny, sizeof(tiny));
    ASSERT_STR("truncated to cap-1 and terminated", tiny, "abc");

    /* cap == 1 leaves room for the terminator only. */
    glr_telemetry_slugify("abc", one, sizeof(one));
    ASSERT_TRUE("cap of 1 yields empty string", one[0] == '\0');

    /* A pending dash must not be written as the last byte, leaving a name
     * that ends in separator noise. */
    glr_telemetry_slugify("ab cd", tiny, sizeof(tiny));
    ASSERT_TRUE("no trailing dash at the cut",
                tiny[strlen(tiny) - 1] != '-');

    /* Longer than the shared cap: truncated, never rejected. */
    {
        char big[GLR_TELEMETRY_NAME_MAX];
        char src[GLR_TELEMETRY_NAME_MAX * 3];
        memset(src, 'x', sizeof(src) - 1);
        src[sizeof(src) - 1] = '\0';
        glr_telemetry_slugify(src, big, sizeof(big));
        ASSERT_TRUE("oversize name truncated to cap-1",
                    strlen(big) == GLR_TELEMETRY_NAME_MAX - 1);
    }
}

static void test_join(void) {
    char buf[GLR_TELEMETRY_NAME_MAX];

    ASSERT_TRUE("join succeeds",
                glr_telemetry_join("example", "Torus", buf, sizeof(buf)) == 1);
    ASSERT_STR("join builds prefix/detail", buf, "example/Torus");

    ASSERT_TRUE("join slugifies the detail",
                glr_telemetry_join("example", "Lit cube!", buf, sizeof(buf)) == 1);
    ASSERT_STR("joined detail is slugged", buf, "example/Lit-cube");

    /* An example whose name is entirely punctuation would produce a bare
     * "example/" - drop it rather than report a prefix with no value. */
    ASSERT_TRUE("empty prefix rejected",
                glr_telemetry_join("", "x", buf, sizeof(buf)) == 0);
    ASSERT_TRUE("empty detail rejected",
                glr_telemetry_join("example", "", buf, sizeof(buf)) == 0);
    ASSERT_TRUE("NULL args rejected",
                glr_telemetry_join(NULL, NULL, buf, sizeof(buf)) == 0);
    ASSERT_STR("rejected join leaves an empty buffer", buf, "");
}

static void test_native_event_is_inert(void) {
    /* There is nothing to observe on native by design - the point of the
     * call is that it compiles, links, and returns without a transport.
     * Exercising the NULL/empty paths keeps the sanitizers under ASan here
     * rather than only in a browser. */
    glr_telemetry_event(NULL);
    glr_telemetry_event("");
    glr_telemetry_event("boot");
    glr_telemetry_event_detail(NULL, NULL);
    glr_telemetry_event_detail("example", "Wave surface (analytic normals)");
    ASSERT_TRUE("native event calls are safe no-ops", 1);
}

int main(void) {
    test_plain_names_pass_through();
    test_punctuation_collapses();
    test_hostile_input_is_neutralized();
    test_bounds();
    test_join();
    test_native_event_is_inert();

    return test_harness_report(&g_harness, "glr_telemetry");
}
