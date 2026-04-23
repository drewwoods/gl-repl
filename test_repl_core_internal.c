#include "repl_core_internal.h"
#include "repl_command_store.h"
#include "repl_state.h"
#include <stdio.h>
#include <string.h>

static int g_run = 0;
static int g_pass = 0;

#define ASSERT_TRUE(label, cond) do { \
    g_run++; \
    if (cond) g_pass++; \
    else printf("FAIL [%s] (line %d)\n", label, __LINE__); \
} while (0)

#define ASSERT_INT(label, got, exp) do { \
    g_run++; \
    if ((got) == (exp)) g_pass++; \
    else printf("FAIL [%s] got %d, expected %d (line %d)\n", label, (int)(got), (int)(exp), __LINE__); \
} while (0)

#define ASSERT_STR(label, got, exp) do { \
    g_run++; \
    if (strcmp(got, exp) == 0) g_pass++; \
    else printf("FAIL [%s] got \"%s\", expected \"%s\" (line %d)\n", label, got, exp, __LINE__); \
} while (0)

static void declare_test_vars(void) {
    char err[128];
    declare_predef_var("x", err, sizeof(err));
    declare_predef_var("y", err, sizeof(err));
    declare_predef_var("z", err, sizeof(err));
    declare_predef_var("i", err, sizeof(err));
    declare_predef_var("j", err, sizeof(err));
    declare_predef_var("k", err, sizeof(err));
    declare_predef_var("n", err, sizeof(err));
}

int main() {
    init_predef_vars();
    printf("--- repl_core internal tests ---\n");

    /* 1. trim_in_place */
    {
        char s1[] = "  hello  ";
        trim_in_place(s1);
        ASSERT_STR("trim normal", s1, "hello");

        char s2[] = "   ";
        trim_in_place(s2);
        ASSERT_STR("trim empty", s2, "");

        char s3[] = "a";
        trim_in_place(s3);
        ASSERT_STR("trim single", s3, "a");
    }

    /* 2. extract_for_args_text */
    {
        char var[16], args[64];
        int r = extract_for_args_text("for(i, 0, 10)", var, sizeof(var), args, sizeof(args));
        ASSERT_INT("extract_for_args ok", r, 1);
        ASSERT_STR("extract_for_args var", var, "i");
        ASSERT_STR("extract_for_args args", args, "0, 10");

        r = extract_for_args_text("for ( j , 1 , 5 , 0.5 )", var, sizeof(var), args, sizeof(args));
        ASSERT_INT("extract_for_args spaced ok", r, 1);
        ASSERT_STR("extract_for_args spaced var", var, "j");
        ASSERT_STR("extract_for_args spaced args", args, "1 , 5 , 0.5");
        
        ASSERT_INT("extract_for_args fail", extract_for_args_text("not a for", var, 16, args, 64), 0);
    }

    /* 3. parse_expr_list_exact */
    {
        float vals[4];
        int count = 0;
        int r = parse_expr_list_exact("1, 2, 3", vals, 4, NULL, 0, &count);
        ASSERT_INT("parse_expr_list ok", r, 1);
        ASSERT_INT("parse_expr_list count", count, 3);
        ASSERT_TRUE("val[0]", vals[0] == 1.0f);
        ASSERT_TRUE("val[1]", vals[1] == 2.0f);
        ASSERT_TRUE("val[2]", vals[2] == 3.0f);

        r = parse_expr_list_exact("1, 2", vals, 1, NULL, 0, &count);
        ASSERT_INT("parse_expr_list overflow", r, 0);
    }

    /* 4. Func signatures */
    {
        int fn = -1;
        char params[4][16];
        int count = 0;
        int r = parse_repl_func_signature("func0(r, g, b) {", &fn, params, 4, &count);
        ASSERT_INT("parse_signature ok", r, 1);
        ASSERT_INT("parse_signature fn", fn, 0);
        ASSERT_INT("parse_signature count", count, 3);
        ASSERT_STR("param0", params[0], "r");
        ASSERT_STR("param1", params[1], "g");
        ASSERT_STR("param2", params[2], "b");

        r = parse_repl_func_signature("func1 {", &fn, params, 4, &count);
        ASSERT_INT("parse_signature no params", r, 1);
        ASSERT_INT("parse_signature no params fn", fn, 1);
        ASSERT_INT("parse_signature no params count", count, 0);
    }

    /* 5. Func call args */
    {
        int fn = -1;
        char args[64];
        int r = extract_func_call_args_text("func2(1, x+y)", &fn, args, sizeof(args));
        ASSERT_INT("extract_call ok", r, 1);
        ASSERT_INT("extract_call fn", fn, 2);
        ASSERT_STR("extract_call args", args, "1, x+y");
    }

    /* 6. Block helpers */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 1) {");     /* 0 */
        repl_feed_line_public("  glVertex3f(0,0,0);"); /* 1 */
        repl_feed_line_public("}");                    /* 2 */
        
        ASSERT_INT("find_block_end(0)", find_block_end(0), 2);
        ASSERT_INT("block_depth_at(1)", block_depth_at(1), 1);
        ASSERT_INT("block_depth_at(2)", block_depth_at(2), 1); /* Still 1 at the closing brace */
        ASSERT_INT("nearest_open_block_at(1)", nearest_open_block_at(1), CMD_FOR_BEGIN);
    }

    /* 7. collect_visible_vars */
    {
        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 1) {");
        repl_feed_line_public("  for(j, 0, 1) {");
        
        ExprVar vars[8];
        int n = collect_visible_vars(2, vars, 8);
        ASSERT_INT("collect_visible_vars count", n, 2);
        /* Note: variables are collected inner-to-outer */
        ASSERT_STR("var0 name", vars[0].name, "j");
        ASSERT_STR("var1 name", vars[1].name, "i");
    }

    /* 8. explicit flatten destination */
    {
        GLCmd temp_flat[8];
        FlatCmdLocalVars temp_locals[8];
        ReplFlattenOptions opts;
        ReplFlattenResult result;
        int live_count;
        GLCmd live_first;

        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("glEnable(GL_LIGHTING);");
        repl_feed_line_public("func0(r) {");
        repl_feed_line_public("  glVertex3f(r, 0, 0);");
        repl_feed_line_public("}");
        repl_feed_line_public("func0(2);");
        repl_flatten_commands();
        live_count = g_num_flat_cmds;
        live_first = g_flat_cmds[0];

        memset(temp_flat, 0, sizeof(temp_flat));
        memset(temp_locals, 0, sizeof(temp_locals));
        opts = (ReplFlattenOptions){
            .source_cmds = g_cmds,
            .source_cmd_count = g_num_cmds,
            .flat_cmds = temp_flat,
            .flat_local_vars = temp_locals,
            .flat_capacity = 8
        };
        ASSERT_INT("flatten_program ok",
                   repl_flatten_program(&opts, &result), 1);
        ASSERT_INT("flatten_program result ok", result.ok, 1);
        ASSERT_INT("flatten_program count", result.flat_cmd_count, 2);
        ASSERT_INT("flatten_program lighting result",
                   result.user_lighting_enabled, 1);
        ASSERT_INT("flatten_program first type", temp_flat[0].type, CMD_ENABLE);
        ASSERT_INT("flatten_program second type", temp_flat[1].type, CMD_VERTEX3F);
        ASSERT_TRUE("flatten_program arg eval",
                    fabsf(temp_flat[1].args[0] - 2.0f) < 1e-6f);
        ASSERT_INT("flatten_program provenance source type",
                   g_cmds[temp_flat[1].src_cmd_idx].type, CMD_VERTEX3F);
        ASSERT_INT("flatten_program live count unchanged",
                   g_num_flat_cmds, live_count);
        ASSERT_INT("flatten_program live first unchanged",
                   g_flat_cmds[0].type, live_first.type);

        opts.flat_capacity = 1;
        ASSERT_INT("flatten_program capacity fail",
                   repl_flatten_program(&opts, &result), 0);
        ASSERT_INT("flatten_program capacity count", result.flat_cmd_count, 0);
        ASSERT_TRUE("flatten_program capacity status",
                    strstr(result.status, "limit") != NULL);
        ASSERT_INT("flatten_program fail leaves live count",
                   g_num_flat_cmds, live_count);
    }

    /* 9. input_has_expr_vars */
    {
        ExprVar vars[2] = { { "radius", 1.0f }, { "height", 2.0f } };
        ASSERT_INT("has_expr_vars true", input_has_expr_vars("radius + 1", vars, 2), 1);
        ASSERT_INT("has_expr_vars false", input_has_expr_vars("x + 1", vars, 2), 0);
        ASSERT_INT("has_any_visible true (predef)", input_has_any_visible_vars("x + 1", vars, 2), 1);
    }

    /* 10. command-store bulk load */
    {
        GLCmd loaded[2];
        ReplCommandStore store;

        repl_reset_state(); declare_test_vars();
        repl_feed_line_public("for(i, 0, 1) {");
        repl_feed_line_public("}");
        ASSERT_INT("command_store_load pre-cache depth", block_depth_at(1), 1);

        memset(loaded, 0, sizeof(loaded));
        loaded[0].type = CMD_VERTEX3F;
        loaded[0].valid = 1;
        snprintf(loaded[0].source, sizeof(loaded[0].source),
                 "glVertex3f(1, 0, 0);");
        loaded[1].type = CMD_COLOR3F;
        loaded[1].valid = 1;
        snprintf(loaded[1].source, sizeof(loaded[1].source),
                 "glColor3f(1, 0, 0);");

        store = repl_command_store_live();
        ASSERT_TRUE("command_store uses document cmds",
                    store.cmds == repl_state_document_cmds_mut());
        ASSERT_TRUE("command_store uses document count",
                    store.count == repl_state_document_mut()->cmd_count);
        repl_state_normals_dirty_clear();
        ASSERT_INT("document normals dirty clear",
                   repl_state_normals_dirty(), 0);
        ASSERT_INT("command_store_load ok",
                   repl_command_store_load(&store, loaded, 2, 99), 1);
        ASSERT_INT("command_store_load count", g_num_cmds, 2);
        ASSERT_INT("command_store_load state count",
                   repl_state_document_count(), 2);
        ASSERT_INT("command_store_load edit clamp", g_edit_line, 2);
        ASSERT_INT("command_store_load state edit clamp",
                   repl_state_edit_line(), 2);
        ASSERT_STR("command_store_load source", g_cmds[1].source,
                   "glColor3f(1, 0, 0);");
        ASSERT_STR("command_store_load state source",
                   repl_state_document_cmd_at(1)->source,
                   "glColor3f(1, 0, 0);");
        ASSERT_INT("command_store_load marks normals dirty",
                   repl_state_normals_dirty(), 1);
        ASSERT_INT("command_store_load invalidates depth",
                   block_depth_at(1), 0);

        ASSERT_INT("command_store_load rejects missing cmds",
                   repl_command_store_load(&store, NULL, 1, 0), 0);
        ASSERT_INT("command_store_load reject keeps count", g_num_cmds, 2);
        ASSERT_INT("command_store_load rejects overflow",
                   repl_command_store_load(&store, loaded,
                                           MAX_COMMANDS + 1, 0), 0);
        ASSERT_INT("command_store_load overflow keeps count", g_num_cmds, 2);
        ASSERT_INT("command_store_load empty ok",
                   repl_command_store_load(&store, NULL, 0, -5), 1);
        ASSERT_INT("command_store_load empty count", g_num_cmds, 0);
        ASSERT_INT("command_store_load empty edit clamp", g_edit_line, 0);
    }

    /* 11. editor input/selection/clipboard state facade */
    {
        ReplEditorInputState *input;
        ReplEditorState editor;
        GLCmd *clipboard;

        repl_reset_state(); declare_test_vars();

        input = repl_state_editor_input_mut();
        ASSERT_TRUE("editor input facade uses input buffer",
                    input->input == g_input);
        ASSERT_TRUE("editor input facade uses input len",
                    input->input_len == &g_input_len);
        ASSERT_TRUE("editor input facade uses cursor",
                    input->cursor_pos == &g_cursor_pos);

        repl_state_input_set_text("abc");
        ASSERT_STR("state input set text", g_input, "abc");
        ASSERT_INT("state input set len", g_input_len, 3);
        ASSERT_INT("state input cursor at end", g_cursor_pos, 3);
        repl_state_cursor_pos_set(99);
        ASSERT_INT("state cursor clamps high", g_cursor_pos, 3);
        repl_state_cursor_pos_set(-5);
        ASSERT_INT("state cursor clamps low", g_cursor_pos, 0);

        repl_state_pending_newline_set_text("next line");
        ASSERT_STR("state newline set text", g_newline_buf, "next line");
        ASSERT_INT("state newline len", g_newline_len, 9);
        repl_state_insert_mode_set(42);
        ASSERT_INT("state insert mode set", g_inserting, 1);

        repl_state_selection_set(4, 2);
        ASSERT_INT("state selection anchor", g_sel_anchor, 4);
        ASSERT_INT("state selection end", g_sel_end, 2);
        repl_state_selection_clear();
        ASSERT_INT("state selection clear anchor", g_sel_anchor, -1);
        ASSERT_INT("state selection clear end", g_sel_end, -1);

        clipboard = repl_state_clipboard_cmds_mut();
        clipboard[0].type = CMD_COLOR3F;
        clipboard[0].valid = 1;
        repl_state_clipboard_count_set(1);
        ASSERT_TRUE("state clipboard buffer", clipboard == g_clipboard);
        ASSERT_INT("state clipboard count", g_clipboard_count, 1);
        ASSERT_INT("state clipboard count accessor",
                   repl_state_clipboard_count(), 1);
        repl_state_clipboard_clear();
        ASSERT_INT("state clipboard clear", g_clipboard_count, 0);

        editor = repl_editor_state_live();
        ASSERT_TRUE("editor live bundle uses state input",
                    editor.input == repl_state_input_buffer_mut());
        ASSERT_TRUE("editor live bundle uses state selection",
                    editor.sel_anchor == repl_state_selection_mut()->anchor_idx);
        ASSERT_TRUE("editor live bundle uses state clipboard",
                    editor.clipboard == repl_state_clipboard_cmds_mut());

        repl_state_editor_input_reset();
        ASSERT_STR("state input reset text", g_input, "");
        ASSERT_INT("state input reset inserting", g_inserting, 0);
        ASSERT_STR("state newline reset text", g_newline_buf, "");
    }

    printf("\n%d / %d tests passed\n", g_pass, g_run);
    return (g_pass == g_run) ? 0 : 1;
}
