/*
 * Synthetic profiler catalog for tests that must cross a 64-bit word
 * boundary without reserving fake rows in the production catalog.
 */
#ifndef TESTS_SUPPORT_PROF_SECTIONS_WIDE_H
#define TESTS_SUPPORT_PROF_SECTIONS_WIDE_H

#define PROF_SECTIONS_PROVIDED 1

typedef enum {
    PROF_TEST_LOW = 0,
    PROF_TEST_HIGH = 65,
    PROF_SECTION_COUNT = 67
} ProfSection;

#endif /* TESTS_SUPPORT_PROF_SECTIONS_WIDE_H */
