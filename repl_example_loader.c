/*
 * repl_example_loader.c -- Built-in example loading and metadata handling.
 */
#include "sample.h"
#include "repl_command_store.h"
#include "repl_core_internal.h"
#include "repl_examples.h"

static const char *example_cam_skip_ws(const char *text) {
    while (*text && isspace((unsigned char)*text))
        text++;
    return text;
}

static const char *example_cam_skip_sep(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == ',' ||
           *text == 'f' || *text == 'F')
        text++;
    return text;
}

static int example_cam_read_floats(const char *text, float *out_vals,
                                   int out_count, const char **end_out) {
    for (int i = 0; i < out_count; i++) {
        char *end = NULL;

        text = example_cam_skip_sep(text);
        out_vals[i] = strtof(text, &end);
        if (end == text)
            return 0;
        text = end;
    }

    if (end_out)
        *end_out = text;
    return 1;
}

static int example_cam_finish_call(const char *text) {
    text = example_cam_skip_sep(text);
    while (*text == ')' || *text == ';' || isspace((unsigned char)*text))
        text++;
    return *text == '\0';
}

static int example_cam_parse_translate(const char *text,
                                       float *x, float *y, float *z) {
    const char *end = NULL;
    float vals[3];

    text = example_cam_skip_ws(text);
    if (strncmp(text, "glTranslatef", 12) != 0)
        return 0;

    text = strchr(text, '(');
    if (!text)
        return 0;
    text++;

    if (!example_cam_read_floats(text, vals, 3, &end) ||
        !example_cam_finish_call(end))
        return 0;

    *x = vals[0];
    *y = vals[1];
    *z = vals[2];
    return 1;
}

static int example_cam_parse_rotate(const char *text,
                                    float axis_x, float axis_y, float axis_z,
                                    float *angle_out) {
    const char *end = NULL;
    float vals[4];

    text = example_cam_skip_ws(text);
    if (strncmp(text, "glRotatef", 9) != 0)
        return 0;

    text = strchr(text, '(');
    if (!text)
        return 0;
    text++;

    if (!example_cam_read_floats(text, vals, 4, &end) ||
        !example_cam_finish_call(end))
        return 0;

    if (fabsf(vals[1] - axis_x) > 1e-4f ||
        fabsf(vals[2] - axis_y) > 1e-4f ||
        fabsf(vals[3] - axis_z) > 1e-4f)
        return 0;

    *angle_out = vals[0];
    return 1;
}

static int try_apply_example_camera_header(const char *const *lines) {
    float dist_x, dist_y, dist_z;
    float rx, ry;
    float tx, ty, tz;

    if (!lines || !lines[0] || strcmp(lines[0], "// camera") != 0)
        return 0;
    if (!lines[1] || !lines[2] || !lines[3] || !lines[4])
        return 0;

    if (!example_cam_parse_translate(lines[1], &dist_x, &dist_y, &dist_z) ||
        fabsf(dist_x) > 1e-4f || fabsf(dist_y) > 1e-4f ||
        !example_cam_parse_rotate(lines[2], 1.0f, 0.0f, 0.0f, &rx) ||
        !example_cam_parse_rotate(lines[3], 0.0f, 1.0f, 0.0f, &ry) ||
        !example_cam_parse_translate(lines[4], &tx, &ty, &tz))
        return 0;

    repl_state_camera_set_orbit(rx, ry);
    repl_state_camera_set_distance(-dist_z);
    repl_state_camera_set_pan(-tx, -ty, -tz);
    return 1;
}

static void reset_example_presentation_defaults(void) {
    repl_state_presentation_reset_example_defaults();
}

static int example_cfg_extract_slug(const char *text,
                                    char *slug, int slug_sz) {
    const char *p = text;
    int slug_len = 0;

    if (!text || !slug || slug_sz < 2)
        return 0;

    p = example_cam_skip_ws(p);
    if (p[0] != '/' || p[1] != '/')
        return 0;
    p += 2;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '@')
        return 0;
    p++;

    if (strncmp(p, "cfg", 3) != 0 || !isspace((unsigned char)p[3]))
        return 0;
    p += 4;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '_' && !isalnum((unsigned char)*p))
        return 0;

    while ((*p == '_' || isalnum((unsigned char)*p)) &&
           slug_len < slug_sz - 1)
        slug[slug_len++] = *p++;
    slug[slug_len] = '\0';
    if (slug_len == 0)
        return 0;

    while (*p && isspace((unsigned char)*p))
        p++;
    return *p == '=';
}

static int example_cfg_slug_allowed(const char *slug) {
    static const char *const allowed_slugs[] = {
        "wireframe",
        "grid",
        "grid_major",
        "grid_extent",
        "axes",
        "vertex_labels",
        "normal_vectors",
        "vertex_outlines",
        "vertex_points",
        "vertex_guides",
        "light_indicators",
        "backdrop",
        "camera_rotate",
        NULL
    };

    for (int i = 0; allowed_slugs[i]; i++) {
        if (strcmp(allowed_slugs[i], slug) == 0)
            return 1;
    }
    return 0;
}

static int consume_example_cfg_header(const char *const *lines) {
    int count = 0;

    while (lines && lines[count]) {
        char slug[32];

        if (!example_cfg_extract_slug(lines[count], slug, sizeof(slug)))
            break;
        if (example_cfg_slug_allowed(slug))
            parse_workspace_header_line(lines[count]);
        count++;
    }

    return count;
}

static void load_example_lines(const char *const *lines) {
    const char *const *body = lines;
    ReplCommandStore store = repl_command_store_live();

    repl_command_store_load(&store, NULL, 0, 0);
    repl_state_flat_program_set_count(0);
    repl_state_insert_mode_set(0);
    {
        ReplEditorInputState *inp = repl_state_editor_input_mut();
        inp->input[0] = '\0';
        *inp->input_len = 0;
        repl_state_cursor_pos_set(0);
        inp->pending_newline[0] = '\0';
        *inp->pending_newline_len = 0;
    }
    repl_editor_reset_transients();
    init_predef_vars();
    reset_example_presentation_defaults();

    if (body)
        body += consume_example_cfg_header(body);

    if (body && body[0] && strcmp(body[0], "// camera") == 0) {
        try_apply_example_camera_header(body);
        for (int skip = 0; skip < 5 && body[0]; skip++)
            body++;
    }

    for (; body && *body; body++)
        feed_line(*body);

    repl_state_insert_mode_set(0);
    repl_state_edit_line_set(repl_state_document_count());
    {
        ReplEditorInputState *inp = repl_state_editor_input_mut();
        inp->input[0] = '\0';
        *inp->input_len = 0;
        repl_state_cursor_pos_set(0);
    }
    mark_normals_dirty();
}

static void load_example(int idx) {
    int count = repl_examples_count();
    const char *const *lines;
    const char *name;

    if (idx < 0 || idx >= count) return;
    lines = repl_examples_lines(idx);
    name = repl_examples_name(idx);
    if (!lines || !name) return;

    /* Preserve the user's work (once, into slot 0) before overwriting with
     * an example. Subsequent example loads leave the home slot untouched. */
    repl_scenes_capture_home_if_needed();

    load_example_lines(lines);
    *repl_state_scenes_mut()->active_example_idx = idx;
    repl_scenes_mark_example_active();
    char msg[128];
    snprintf(msg, sizeof(msg), "Example %d/%d: %s (F12 for next)",
             idx + 1, count, name);
    set_status(msg);
}

int repl_example_count(void) {
    return repl_examples_count();
}

const char *repl_example_name(int idx) {
    return repl_examples_name(idx);
}

void repl_load_example(int idx) {
    load_example(idx);
}

void repl_load_example_lines_for_test(const char *const *lines) {
    load_example_lines(lines);
}
