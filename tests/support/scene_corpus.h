#ifndef TESTS_SUPPORT_SCENE_CORPUS_H
#define TESTS_SUPPORT_SCENE_CORPUS_H

#include <stdlib.h>   /* getenv */
#include <string.h>   /* strcmp */

/* Opt-in gate for the scene corpora under tests/scenes.
 *
 * examples/scenes is the shipped catalog and always runs - a regression
 * there is a regression users see. The corpora under tests/scenes are
 * deliberately large and grow freely (they are where corner cases get
 * parked), and each scene costs a full export plus a cc invocation, so
 * they would otherwise stretch the default `make test` for coverage that
 * is not on any user's path. `make test-scenes` sets this; `make test-full`
 * runs that target. Nothing else needs the variable's exact spelling -
 * both corpus tests read it through here. */
static inline int repl_test_scene_corpus_enabled(void) {
    const char *env = getenv("REPL_SCENE_CORPUS");
    return env && env[0] && strcmp(env, "0") != 0;
}

/* The corpora themselves, NULL-terminated. Four tests walk these now
 * (test_repl_core_examples, test_camera_header_parity,
 * test_export_trace_parity, test_repl_call_frames), so the paths live
 * here rather than in each of them. */
static inline const char *const *repl_test_scene_corpus_dirs(void) {
    static const char *const dirs[] = {
        "tests/scenes/stress",
        "tests/scenes/general",
        NULL
    };
    return dirs;
}

/* Last path component, used as the short corpus tag in assertion labels and
 * export filenames ("tests/scenes/stress" -> "stress"). */
static inline const char *repl_test_scene_corpus_tag(const char *dir) {
    const char *slash = dir ? strrchr(dir, '/') : NULL;
    return slash && slash[1] ? slash + 1 : (dir ? dir : "scene");
}

#endif /* TESTS_SUPPORT_SCENE_CORPUS_H */
