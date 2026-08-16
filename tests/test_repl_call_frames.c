/*
 * tests/test_repl_call_frames.c -- interned call-frame provenance.
 *
 * Stage 1 of docs/plans/active/call-frame-provenance.md. Offline only:
 * flatten interns a topology table + argument arena, --dump-flat / --call-tree
 * print it, and a drift test treats the four legacy provenance fields as the
 * oracle for every indexed command. Overflow is a soft latch.
 */
#include <stdio.h>
#include <string.h>

#include "app/glr_ctrl.h"
#include "app/glr_debug.h"
#include "editor/input.h"
#include "repl/command.h"
#include "repl/eval.h"
#include "repl/example_loader.h"
#include "repl/examples.h"
#include "repl/flatten.h"
#include "repl/pipeline.h"
#include "repl/state.h"
#include "source_document.h"
#include "support/scene_corpus.h"
#include "support/test_harness.h"

static TestHarness g_harness = TEST_HARNESS_INIT;

#define ASSERT_TRUE(label, cond) TEST_ASSERT_TRUE(&g_harness, label, cond)
#define ASSERT_INT(label, got, exp) TEST_ASSERT_INT(&g_harness, label, got, exp)
#define ASSERT_FLOAT(label, got, exp) \
    TEST_ASSERT_FLOAT(&g_harness, label, got, exp, 1e-5f)

static void load_scene(const char *const *lines) {
    glr_ctrl_reset_all();
    for (int i = 0; lines[i]; i++)
        editor_feed_line(lines[i]);
    repl_flatten_commands(0);
}

static void assert_indexed_drift_ex(const char *tag, FlatProgramView view,
                                    int require_indexed) {
    int mismatches = 0;
    int indexed = 0;

    for (int i = 0; i < view.cmd_count; i++) {
        ReplCallFrameDerivedProv derived;
        int frame = repl_flat_cmd_call_frame(&view, i);
        char label[192];

        if (frame == REPL_CALL_FRAME_NONE)
            continue;
        indexed++;
        snprintf(label, sizeof(label), "%s flat %d derive", tag, i);
        ASSERT_INT(label, repl_call_frame_derive_prov(view, frame, &derived), 1);
        if (derived.call_src_cmd_idx != view.cmds[i].call_src_cmd_idx ||
            derived.root_call_src_cmd_idx != view.cmds[i].root_call_src_cmd_idx ||
            derived.call_depth != view.cmds[i].call_depth ||
            derived.func_scope_mask != view.cmds[i].func_scope_mask) {
            mismatches++;
            printf("  drift[%s] flat=%d frame=%d "
                   "derived(src=%d root=%d depth=%d mask=0x%x) "
                   "stored(src=%d root=%d depth=%d mask=0x%x)\n",
                   tag, i, frame,
                   derived.call_src_cmd_idx, derived.root_call_src_cmd_idx,
                   derived.call_depth, derived.func_scope_mask,
                   view.cmds[i].call_src_cmd_idx,
                   view.cmds[i].root_call_src_cmd_idx,
                   view.cmds[i].call_depth, view.cmds[i].func_scope_mask);
        }
    }
    {
        char label[128];
        snprintf(label, sizeof(label), "%s: indexed commands drifted", tag);
        ASSERT_INT(label, mismatches, 0);
        if (require_indexed) {
            snprintf(label, sizeof(label), "%s: saw indexed commands", tag);
            ASSERT_TRUE(label, indexed > 0);
        }
    }
}

static void assert_indexed_drift(const char *tag, FlatProgramView view) {
    assert_indexed_drift_ex(tag, view, 1);
}

static void assert_scene_drift(const char *tag, FlatProgramView view) {
    if (view.cmd_count == 0) {
        char label[192];
        snprintf(label, sizeof(label),
                 "%s: failed flatten published no frames", tag);
        ASSERT_INT(label, view.call_frame_count, 0);
        snprintf(label, sizeof(label),
                 "%s: failed flatten published no args", tag);
        ASSERT_INT(label, view.call_frame_arg_count, 0);
        return;
    }
    assert_indexed_drift_ex(tag, view, 0);
}

static void test_record_size(void) {
    printf("--- call-frames: record size ---\n");
    ASSERT_INT("ReplCallFrame is 32 bytes", (int)sizeof(ReplCallFrame), 32);
    ASSERT_INT("NONE is -1", REPL_CALL_FRAME_NONE, -1);
}

static void test_top_level_unindexed(void) {
    static const char *const lines[] = {
        "glBegin(GL_POINTS);",
        "glVertex3f(1, 2, 3);",
        "glEnd();",
        NULL,
    };
    FlatProgramView view;

    printf("--- call-frames: top-level is NONE ---\n");
    load_scene(lines);
    view = repl_state_flat_program_view();
    ASSERT_INT("no frames interned", view.call_frame_count, 0);
    ASSERT_INT("no overflow", view.call_frame_overflow, 0);
    for (int i = 0; i < view.cmd_count; i++) {
        char label[64];
        snprintf(label, sizeof(label), "top-level flat %d unindexed", i);
        ASSERT_INT(label, repl_flat_cmd_call_frame(&view, i),
                   REPL_CALL_FRAME_NONE);
    }
}

static void test_single_call_and_drift(void) {
    static const char *const lines[] = {
        "draw(x, y) {",
        "glVertex3f(x, y, 0);",
        "}",
        "glBegin(GL_POINTS);",
        "draw(1.5, 2.5);",
        "glEnd();",
        NULL,
    };
    FlatProgramView view;
    int vertex_idx = -1;
    int frame;
    const ReplCallFrame *f;

    printf("--- call-frames: single call + drift ---\n");
    load_scene(lines);
    view = repl_state_flat_program_view();
    ASSERT_INT("one frame", view.call_frame_count, 1);
    ASSERT_INT("two captured args", view.call_frame_arg_count, 2);
    assert_indexed_drift("single", view);

    for (int i = 0; i < view.cmd_count; i++) {
        if (view.cmds[i].type == CMD_VERTEX3F) {
            vertex_idx = i;
            break;
        }
    }
    ASSERT_TRUE("found vertex", vertex_idx >= 0);
    frame = repl_flat_cmd_call_frame(&view, vertex_idx);
    ASSERT_TRUE("vertex is indexed", frame != REPL_CALL_FRAME_NONE);
    f = &view.call_frames[frame];
    ASSERT_INT("parent is NONE", f->parent, REPL_CALL_FRAME_NONE);
    ASSERT_INT("depth 1", f->depth, 1);
    ASSERT_INT("two args", f->arg_count, 2);
    ASSERT_FLOAT("arg0", view.call_frame_args[f->arg_offset], 1.5f);
    ASSERT_FLOAT("arg1", view.call_frame_args[f->arg_offset + 1], 2.5f);
    ASSERT_TRUE("range contains vertex",
                f->flat_begin <= vertex_idx && vertex_idx < f->flat_end);
}

static void test_nested_and_loop_siblings(void) {
    static const char *const lines[] = {
        "inner(x) {",
        "glVertex3f(x, 0, 0);",
        "}",
        "outer(n) {",
        "inner(n);",
        "}",
        "glBegin(GL_POINTS);",
        "for(i, 0, 4) {",
        "outer(i);",
        "}",
        "glEnd();",
        NULL,
    };
    FlatProgramView view;
    int vertex_frames[4];
    int nvert = 0;

    printf("--- call-frames: nested + same-site siblings ---\n");
    load_scene(lines);
    view = repl_state_flat_program_view();
    /* 4 outer + 4 inner */
    ASSERT_INT("eight frames", view.call_frame_count, 8);
    assert_indexed_drift("nested-loop", view);

    for (int i = 0; i < view.cmd_count && nvert < 4; i++) {
        if (view.cmds[i].type == CMD_VERTEX3F)
            vertex_frames[nvert++] = repl_flat_cmd_call_frame(&view, i);
    }
    ASSERT_INT("four vertices", nvert, 4);
    for (int a = 0; a < 4; a++) {
        char label[64];
        snprintf(label, sizeof(label), "vertex %d indexed", a);
        ASSERT_TRUE(label, vertex_frames[a] != REPL_CALL_FRAME_NONE);
        for (int b = a + 1; b < 4; b++) {
            snprintf(label, sizeof(label), "siblings %d and %d distinct", a, b);
            ASSERT_TRUE(label, vertex_frames[a] != vertex_frames[b]);
        }
        ASSERT_FLOAT("sibling arg is i",
                     view.call_frame_args[view.call_frames[vertex_frames[a]].arg_offset],
                     (float)a);
        ASSERT_INT("inner parent is an outer",
                   view.call_frames[vertex_frames[a]].parent != REPL_CALL_FRAME_NONE, 1);
    }
}

static void test_same_site_recursion(void) {
    static const char *const lines[] = {
        "walk(d) {",
        "glVertex3f(d, 0, 0);",
        "if(d > 0.5) {",
        "walk(d - 1);",
        "}",
        "}",
        "glBegin(GL_POINTS);",
        "walk(2);",
        "glEnd();",
        NULL,
    };
    FlatProgramView view;
    int frames[8];
    int n = 0;

    printf("--- call-frames: same-site recursion ---\n");
    load_scene(lines);
    view = repl_state_flat_program_view();
    ASSERT_TRUE("at least two frames", view.call_frame_count >= 2);
    assert_indexed_drift("recursion", view);

    for (int i = 0; i < view.cmd_count && n < 8; i++) {
        if (view.cmds[i].type == CMD_VERTEX3F)
            frames[n++] = repl_flat_cmd_call_frame(&view, i);
    }
    ASSERT_TRUE("at least two vertices", n >= 2);
    ASSERT_TRUE("recursive rungs distinct", frames[0] != frames[1]);
    ASSERT_TRUE("deeper frame has greater depth",
                view.call_frames[frames[1]].depth >
                view.call_frames[frames[0]].depth);
    ASSERT_INT("deeper parent is shallower",
               view.call_frames[frames[1]].parent, frames[0]);
}

static void test_wide_arena(void) {
    static const char *const lines[] = {
        "wide(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16) {",
        "glVertex3f(a0, a8, a16);",
        "}",
        "glBegin(GL_POINTS);",
        "wide(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);",
        "glEnd();",
        NULL,
    };
    FlatProgramView view;
    int frame = REPL_CALL_FRAME_NONE;
    const ReplCallFrame *f;

    printf("--- call-frames: 17-arg arena ---\n");
    load_scene(lines);
    view = repl_state_flat_program_view();
    ASSERT_INT("one wide frame", view.call_frame_count, 1);
    ASSERT_INT("17 arena floats", view.call_frame_arg_count, 17);
    assert_indexed_drift("wide", view);
    for (int i = 0; i < view.cmd_count; i++) {
        if (view.cmds[i].type == CMD_VERTEX3F) {
            frame = repl_flat_cmd_call_frame(&view, i);
            break;
        }
    }
    ASSERT_TRUE("vertex indexed", frame != REPL_CALL_FRAME_NONE);
    f = &view.call_frames[frame];
    ASSERT_INT("arg_count 17", f->arg_count, 17);
    for (int a = 0; a < 17; a++) {
        char label[32];
        snprintf(label, sizeof(label), "arena[%d]", a);
        ASSERT_FLOAT(label, view.call_frame_args[f->arg_offset + a], (float)a);
    }
}

static void test_identity_declines_unindexed(void) {
    printf("--- call-frames: identity declines unindexed ---\n");
    ASSERT_INT("NONE vs NONE is not an identity",
               repl_call_frame_identity(REPL_CALL_FRAME_NONE,
                                        REPL_CALL_FRAME_NONE), -1);
    ASSERT_INT("NONE vs indexed is not an identity",
               repl_call_frame_identity(REPL_CALL_FRAME_NONE, 0), -1);
    ASSERT_INT("indexed vs NONE is not an identity",
               repl_call_frame_identity(2, REPL_CALL_FRAME_NONE), -1);
    ASSERT_INT("same indexed frames match",
               repl_call_frame_identity(3, 3), 1);
    ASSERT_INT("different indexed frames disagree",
               repl_call_frame_identity(3, 4), 0);
}

static void test_overflow_is_soft(void) {
    static const char *const lines[] = {
        "leaf(x) {",
        "glVertex3f(x, 0, 0);",
        "}",
        "glBegin(GL_POINTS);",
        "leaf(1);",
        "leaf(2);",
        "leaf(3);",
        "glEnd();",
        NULL,
    };
    GLCmd cmds[64];
    FlatCmdLocalVars locals[64];
    int idx[64];
    ReplCallFrame frames[1];
    float args[4];
    ReplFlattenOptions opts;
    ReplFlattenResult result;
    FlatProgramView view;
    int seen_indexed = 0;
    int seen_none = 0;

    printf("--- call-frames: overflow latch ---\n");
    load_scene(lines);

    memset(cmds, 0, sizeof(cmds));
    memset(locals, 0, sizeof(locals));
    memset(idx, 0xFF, sizeof(idx));
    memset(frames, 0, sizeof(frames));
    memset(args, 0, sizeof(args));
    opts = (ReplFlattenOptions){
        .source_cmds = repl_state_document_cmds(),
        .source_cmd_count = repl_state_document_count(),
        .flat_cmds = cmds,
        .flat_local_vars = locals,
        .flat_capacity = 64,
        .text = source_document_view(),
        .func_aliases = repl_func_alias_view(),
        .flat_call_frame_idx = idx,
        .call_frames = frames,
        .call_frame_capacity = 1,
        .call_frame_args = args,
        .call_frame_arg_capacity = 4,
    };
    ASSERT_INT("tiny-table flatten succeeds",
               repl_flatten_program(&opts, &result), 1);
    ASSERT_INT("one interned frame", result.call_frame_count, 1);
    ASSERT_INT("overflow latched", result.call_frame_overflow, 1);

    view = (FlatProgramView){
        .cmds = cmds,
        .local_vars = locals,
        .cmd_count = result.flat_cmd_count,
        .call_frame_idx = idx,
        .call_frames = frames,
        .call_frame_count = result.call_frame_count,
        .call_frame_args = args,
        .call_frame_arg_count = result.call_frame_arg_count,
        .call_frame_overflow = result.call_frame_overflow,
    };
    for (int i = 0; i < view.cmd_count; i++) {
        if (cmds[i].type != CMD_VERTEX3F)
            continue;
        if (idx[i] == REPL_CALL_FRAME_NONE)
            seen_none++;
        else
            seen_indexed++;
    }
    ASSERT_INT("first invocation indexed", seen_indexed, 1);
    ASSERT_INT("later invocations unindexed", seen_none, 2);

    /* Drift only on the indexed vertex. Unindexed commands must not be
     * compared as if NONE were an identity. */
    assert_indexed_drift("overflow", view);
}

static void test_dump_mentions_frames(void) {
    static const char *const lines[] = {
        "mark(x) {",
        "glVertex3f(x, 0, 0);",
        "}",
        "glBegin(GL_POINTS);",
        "mark(3);",
        "glEnd();",
        NULL,
    };
    char path[] = "/tmp/glr_call_frames_dump.txt";
    FILE *f;
    char buf[8192];
    size_t n;

    printf("--- call-frames: dump-flat + call-tree ---\n");
    load_scene(lines);

    f = fopen(path, "w+");
    ASSERT_TRUE("open dump file", f != NULL);
    if (!f)
        return;
    glr_debug_dump_flat_commands_sync(f, source_document_view());
    glr_debug_dump_call_tree(f, source_document_view());
    rewind(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    remove(path);

    ASSERT_TRUE("dump-flat header has call_frames",
                strstr(buf, "call_frames=") != NULL);
    ASSERT_TRUE("dump-flat row has frame=",
                strstr(buf, "frame=") != NULL);
    ASSERT_TRUE("call-tree header",
                strstr(buf, "=== REPL Call Tree ===") != NULL);
    ASSERT_TRUE("call-tree names the alias",
                strstr(buf, "mark(") != NULL);
    ASSERT_TRUE("call-tree reconstructed count",
                strstr(buf, "reconstructed frames: 1") != NULL);
}

static void test_empty_frames_reclaimed(void) {
    static const char *const lines[] = {
        "skip(x) {",
        "if(0) {",
        "glVertex3f(x, 0, 0);",
        "}",
        "}",
        "inner(x) {",
        "if(0) {",
        "glVertex3f(x, 0, 0);",
        "}",
        "}",
        "outer(x) {",
        "inner(x);",
        "}",
        "mark(y) {",
        "glVertex3f(y, 0, 0);",
        "}",
        "glBegin(GL_POINTS);",
        "skip(1);",
        "outer(1);",
        "mark(2);",
        "glEnd();",
        NULL,
    };
    FlatProgramView view;
    int frame = REPL_CALL_FRAME_NONE;
    int i;

    printf("--- call-frames: empty frames reclaimed ---\n");
    load_scene(lines);
    view = repl_state_flat_program_view();
    ASSERT_INT("only the emitting call remains", view.call_frame_count, 1);
    ASSERT_INT("only the emitting arg remains", view.call_frame_arg_count, 1);
    ASSERT_INT("no overflow", view.call_frame_overflow, 0);
    for (i = 0; i < view.cmd_count; i++) {
        if (view.cmds[i].type == CMD_VERTEX3F) {
            frame = repl_flat_cmd_call_frame(&view, i);
            break;
        }
    }
    ASSERT_TRUE("vertex is indexed", frame != REPL_CALL_FRAME_NONE);
    ASSERT_FLOAT("kept frame is mark(2)",
                 view.call_frame_args[view.call_frames[frame].arg_offset],
                 2.0f);
}

static void test_flatten_failure_zeros_frames(void) {
    static const char *const lines[] = {
        "walk(x) {",
        "walk(x);",
        "}",
        "walk(1);",
        NULL,
    };
    FlatProgramView view;

    printf("--- call-frames: hard flatten failure zeros the table ---\n");
    load_scene(lines);
    view = repl_state_flat_program_view();
    ASSERT_INT("failed flatten has no executable cmds", view.cmd_count, 0);
    ASSERT_INT("failed flatten published no frames", view.call_frame_count, 0);
    ASSERT_INT("failed flatten published no args", view.call_frame_arg_count, 0);
    ASSERT_INT("failed flatten does not report overflow",
               view.call_frame_overflow, 0);
}

static void drift_one_example(int idx, const char *corpus) {
    char tag[192];
    FlatProgramView view;

    snprintf(tag, sizeof(tag), "%s:%s", corpus, repl_example_name(idx));
    glr_ctrl_reset_all();
    if (repl_load_example(idx) <= 0)
        return;
    repl_flatten_commands(0);
    view = repl_state_flat_program_view();
    assert_scene_drift(tag, view);
}

static void test_example_corpus_drift(void) {
    int n = repl_example_count();
    int i;

    printf("--- call-frames: built-in example drift ---\n");
    ASSERT_TRUE("built-in catalog is non-empty", n > 0);
    for (i = 0; i < n; i++)
        drift_one_example(i, "example");
}

static void test_scene_corpus_drift(void) {
    const char *const *dirs;
    int d;

    if (!repl_test_scene_corpus_enabled()) {
        printf("--- call-frames: scene corpora not walked "
               "(REPL_SCENE_CORPUS unset) ---\n");
        return;
    }

    printf("--- call-frames: scene corpus drift ---\n");
    dirs = repl_test_scene_corpus_dirs();
    for (d = 0; dirs[d]; d++) {
        char err[512];
        const char *tag = repl_test_scene_corpus_tag(dirs[d]);
        int n;
        int i;

        err[0] = '\0';
        if (!repl_examples_load_dir(dirs[d], err, sizeof(err))) {
            char label[256];
            snprintf(label, sizeof(label), "%s catalog loads", dirs[d]);
            ASSERT_TRUE(label, 0);
            continue;
        }
        n = repl_example_count();
        printf("  %s: %d scenes\n", dirs[d], n);
        for (i = 0; i < n; i++) {
            if (repl_example_source_format(i) != REPL_EXAMPLE_SOURCE_GLR)
                continue;
            drift_one_example(i, tag);
        }
        repl_examples_clear_runtime_catalog();
    }
}

int main(void) {
    test_record_size();
    test_top_level_unindexed();
    test_single_call_and_drift();
    test_nested_and_loop_siblings();
    test_same_site_recursion();
    test_wide_arena();
    test_identity_declines_unindexed();
    test_overflow_is_soft();
    test_dump_mentions_frames();
    test_empty_frames_reclaimed();
    test_flatten_failure_zeros_frames();
    test_example_corpus_drift();
    test_scene_corpus_drift();
    return test_harness_report(&g_harness, "test_repl_call_frames");
}
