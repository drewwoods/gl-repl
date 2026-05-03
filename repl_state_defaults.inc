    .document = {
        .cmd_count = 0,
        .capacity = MAX_COMMANDS,
        .edit_line_idx = 0,
        .normals_dirty = 1,
    },
    .flat_program = {
        .cmd_count = 0,
        .capacity = MAX_COMMANDS,
        .dirty = 1,
        .user_lighting_enabled = 0,
        .current_block_begin_idx = -1,
        .current_block_end_idx = -1,
        .current_block_source_line_idx = -1,
    },
    .variables = {
        .predef_var_count = 0,
        .time_var_idx = -1,
        .time_playing = 1,
        .anim_time = 0.0f,
    },
    /* editor_input + selection + clipboard defaults moved to
     * editor_state.c (Phase 1 commits 5-6). */
    /* code_panel.scroll + scroll_follow_cursor moved to editor_state.c
     * (Phase 1 commit 11); the code_panel render-chrome bits
     * (panel_frac, resizing_panel, cursor blink, cursor px/py) moved
     * to ui_state.c (Phase A commit 12).
     * help / variable_panel / profile_panel / status / pointer / viewport
     * defaults moved to ui_state.c (Phase 1 commit 8).
     * variable_drag defaults moved to editor_state.c (Phase 1 commit 9).
     * search + autocomplete defaults moved to editor_state.c
     * (Phase 1 commit 7). */
    /* camera defaults moved to ui_state.c (Phase A commit 13). */
    .presentation = {
        .wireframe = CFG_DEFAULT_WIREFRAME,
        .grid_theme = CFG_DEFAULT_GRID_THEME,
        .grid_major_idx = CFG_DEFAULT_GRID_MAJOR_IDX,
        .grid_extent_idx = CFG_DEFAULT_GRID_EXTENT_IDX,
        .axes_theme = CFG_DEFAULT_AXES_THEME,
        .show_vertex_labels = CFG_DEFAULT_VERTEX_LABELS,
        .show_normal_vectors = CFG_DEFAULT_NORMAL_VECTORS,
        .show_vertex_indices = CFG_DEFAULT_VERTEX_INDICES,
        .show_vertex_outlines = CFG_DEFAULT_VERTEX_OUTLINES,
        .show_vertex_points = CFG_DEFAULT_VERTEX_POINTS,
        .show_vertex_guides = CFG_DEFAULT_VERTEX_GUIDES,
        .xform_guide_mode = CFG_DEFAULT_XFORM_GUIDE_MODE,
        .autonormal = 0,
        .show_light_indicators = CFG_DEFAULT_LIGHT_INDICATORS,
        .backdrop_mode = CFG_DEFAULT_BACKDROP_MODE,
        .highlight_current_poly = 1,
        .ortho_mode = 0,
        .wrap_at_comma = CFG_DEFAULT_WRAP_AT_COMMA,
        .code_panel_layout = CFG_DEFAULT_CODE_PANEL_LAYOUT,
        .focus_vertex = { 0.0f, 0.0f, 0.0f },
        .focus_vertex_valid = 0,
    },
    .render = {
        .use_accum = 1,
        .accum_aa_enabled = 1,
        .accum_samples = 2,
        .accum_jitter_x = 0.0f,
        .accum_jitter_y = 0.0f,
        .multisample_enabled = CFG_DEFAULT_MULTISAMPLE,
        .line_smooth_enabled = CFG_DEFAULT_LINE_SMOOTH,
        .point_attenuation_enabled = CFG_DEFAULT_ATTENUATE_POINTS,
        .lights = {
            { GL_LIGHT0, 1,
              { 2.0f, 4.0f, 5.0f, 0.0f },
              { 0.80f, 0.80f, 0.75f, 1.0f },
              { 0.10f, 0.10f, 0.12f, 1.0f },
              { 1.0f, 1.0f, 0.95f, 1.0f } },
            { GL_LIGHT1, 1,
              { -3.0f, 2.0f, -2.0f, 1.0f },
              { 0.45f, 0.30f, 0.15f, 1.0f },
              { 0.05f, 0.03f, 0.02f, 1.0f },
              { 0.30f, 0.20f, 0.10f, 1.0f } },
            { GL_LIGHT2, 1,
              { 0.0f, -1.0f, 3.0f, 1.0f },
              { 0.15f, 0.25f, 0.50f, 1.0f },
              { 0.02f, 0.03f, 0.06f, 1.0f },
              { 0.10f, 0.15f, 0.35f, 1.0f } },
            { GL_LIGHT3, 0,
              { 1.0f, 1.0f, -4.0f, 0.0f },
              { 0.35f, 0.35f, 0.40f, 1.0f },
              { 0.05f, 0.05f, 0.06f, 1.0f },
              { 0.20f, 0.20f, 0.25f, 1.0f } },
        },
        .clear_color = { 0.10f, 0.10f, 0.13f, 1.0f },
    },
    /* .replay defaults moved to replay_state.c (Phase F commit 33). */
    .scenes = {
        .active_example_idx = -1,
        .workspace_dir = "",
    },
    .import_export = {
        .workspace_header_lines = {
            [0] = "",
        },
        .workspace_header_line_count = 0,
        .render_state_lines = {
            "  glEnable(GL_MULTISAMPLE);",
            "  glDisable(GL_LINE_SMOOTH);",
            "  glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);"
        },
        .cam_lines = {
            "  glTranslatef(0.0000f, 0.0000f, -5.0000f);",
            "  glRotatef(20.0000f, 1.0f, 0.0f, 0.0f);",
            "  glRotatef(30.0000f, 0.0f, 1.0f, 0.0f);",
            "  glTranslatef(0.0000f, 0.0000f, 0.0000f);"
        },
        .export_scene_name_hint = NULL,
        .pending_scene_name = "",
        .pending_workspace_dir = "",
    },
