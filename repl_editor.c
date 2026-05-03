/*
 * repl_editor.c — Vestigial after Phase J1 commit 49a.
 *
 * The keyboard / special / mouse / motion / mousewheel dispatch bodies
 * migrated to editor_input.c (commits 44–46), the timer body inlined
 * into imrepl_ctrl_timer (commit 48b), the legacy repl_*_func entry
 * points + repl_editor_active_modifiers + repl_set_modifier_provider_for_test
 * forwarders have been deleted now that production callers and test
 * fixtures use editor_handle_* / editor_input_active_modifiers /
 * editor_input_set_modifier_provider_for_test directly.
 *
 * The translation unit is left as an empty placeholder so the Makefile
 * stays green; commit 49b removes the file from SRCS and the boundary
 * check guarantees no new dispatch shims reappear.
 */
