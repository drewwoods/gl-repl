#ifndef REPL_TEST_HARNESS_H
#define REPL_TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

typedef struct {
    int run;
    int passed;
    int failed;
} TestHarness;

#define TEST_HARNESS_INIT { 0, 0, 0 }

#define TEST_ASSERT_TRUE(harness_ptr, label, cond) do { \
    TestHarness *_h = (harness_ptr); \
    _h->run++; \
    if (cond) { \
        _h->passed++; \
    } else { \
        _h->failed++; \
        printf("FAIL [%s] (line %d)\n", (label), __LINE__); \
    } \
} while (0)

#define TEST_ASSERT_INT(harness_ptr, label, got, expected) do { \
    TestHarness *_h = (harness_ptr); \
    int _got = (int)(got); \
    int _expected = (int)(expected); \
    _h->run++; \
    if (_got == _expected) { \
        _h->passed++; \
    } else { \
        _h->failed++; \
        printf("FAIL [%s] got %d, expected %d (line %d)\n", \
               (label), _got, _expected, __LINE__); \
    } \
} while (0)

#define TEST_ASSERT_STR(harness_ptr, label, got, expected) do { \
    TestHarness *_h = (harness_ptr); \
    const char *_got = (got); \
    const char *_expected = (expected); \
    _h->run++; \
    if (strcmp(_got, _expected) == 0) { \
        _h->passed++; \
    } else { \
        _h->failed++; \
        printf("FAIL [%s] got \"%s\", expected \"%s\" (line %d)\n", \
               (label), _got, _expected, __LINE__); \
    } \
} while (0)

static inline int test_harness_report(const TestHarness *harness,
                                      const char *suite_name) {
    printf("%s: %d/%d passed", suite_name, harness->passed, harness->run);
    if (harness->failed > 0)
        printf(", %d FAILED", harness->failed);
    printf("\n");
    return harness->failed == 0 ? 0 : 1;
}

#endif /* REPL_TEST_HARNESS_H */
