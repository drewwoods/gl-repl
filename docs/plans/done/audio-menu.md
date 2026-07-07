# Audio Menu Plan

## Summary
Add a top-level `Audio` menu that reuses the existing catalog flyout engine in [menu_bar.c](/Users/drew/src/code/openGL/samples/gen-ai/gl-repl-worktree/src/ui/app/menu_bar.c). V1 groups discovered tracks by source (`Assets`, `Bundled`, `My Music`, fallback `Default`), displays filename stems, highlights the playing track, and shows live `m:ss / m:ss` only beside the active track while the menu is open.

Do not make `playlist.ini` or `g_cfg_items[]` the source of truth. Audio has its own top-level menu; Play/Pause still writes `GLR_CONFIG_AUDIO_MODE` through the config layer so persistence and tutorial `REQUIRE` observers keep one write path.

## Public Interfaces
- Add `GLR_MENU_AUDIO` to `GlrMenuId` in [glr_actions.h](/Users/drew/src/code/openGL/samples/gen-ai/gl-repl-worktree/src/app/glr_actions.h), plus Audio menu offsets for `---`, `Play/Pause`, `Next Track`, `Previous Track`, and `Loop: <mode>`.
- Extend [glr_audio.h](/Users/drew/src/code/openGL/samples/gen-ai/gl-repl-worktree/src/app/glr_audio.h) with:
  - `GlrAudioTrackSpec { path, group, display_name }`
  - `glr_audio_set_playlist_specs(...)`, while keeping `glr_audio_set_playlist(...)`
  - `glr_audio_track_count`, `glr_audio_track_display_name`, `glr_audio_track_group`
  - `glr_audio_current_index`, `glr_audio_play_track_index`
  - `glr_audio_current_cursor_seconds`, `glr_audio_track_duration_seconds`
- Keep metadata ownership in `glr_audio.c`; `menu_bar.c` reads it directly through the `glr_audio_track_*` accessors. Do not add a passthrough catalog module or a stored `subheading` field for v1.

## Implementation
- Update playlist scanning in [gl_repl.c](/Users/drew/src/code/openGL/samples/gen-ai/gl-repl-worktree/gl_repl.c) to carry source-group metadata alongside paths and call `glr_audio_set_playlist_specs`; the legacy fallback track is registered as one `Default` group track.
- Store copied track metadata inside `glr_audio.c` parallel to `g_playlist[]`; derive `display_name` from basename without `.mp3` when unspecified.
- Reset cached duration to unknown on playlist load. In `worker_load`, call `ma_sound_get_length_in_pcm_frames` on the inactive target slot before publishing `g_active`; cache seconds before the publish flip so no visible sound is scanned concurrently. UI shows `--:--` until a duration is known.
- Add an Audio flyout provider in `menu_bar.c` using `CatalogFlyoutOps`: groups are source labels, `subheading_of` always returns `NULL`, and `_is_active` compares against `glr_audio_current_index()`.
- Render `elapsed / total` in a right-aligned column only for the active track row while the Audio menu is open.
- Route `UI_HIT_SUBMENU_ITEM` with `cmd_idx == GLR_MENU_AUDIO` to `glr_audio_play_track_index`. Close the menu on accepted track selection.
- Implement top-level Audio controls:
  - `Play/Pause` toggles `GLR_CONFIG_AUDIO_MODE` only through `glr_config_set`, so tutorial `REQUIRE` observers see the write.
  - `Next Track` / `Previous Track` call existing audio APIs and close or stay open consistently with other one-shot command rows.
  - `Loop` cycles `Off -> Song -> All -> Off` via `glr_audio_set_loop_mode` and stays runtime-only.
- Change `glr_actions_apply_audio_cfg_mode` so resume only unpauses; it must not force `GLR_AUDIO_LOOP_ALL`. The audio module’s existing init/reset default remains the one-time source of default loop-all behavior.
- Refine the existing `glr_ctrl_tick` now-playing status to use the display name and ASCII text: `Now playing: name`. No permanent statusbar timer.

## Tests
- Extend `test_audio` for playlist specs, derived names, groups, current index, `play_track_index`, unknown-duration defaults, and “audio off/on does not clobber loop mode.”
- Extend `test_ui_menu_bar` for the Audio top-level label, grouped flyout hits, active-row highlighting, and submenu payloads.
- Extend `test_glr_actions` / `test_glr_ctrl` for Audio control rows, config-layer Play/Pause routing, loop cycling, and submenu track routing.
- Verify with `make test_audio test_ui_menu_bar test_glr_actions test_glr_ctrl`, then `make test-stubs` and `make check-c99`.

## Assumptions
- V1 does not implement `playlist.ini`, tag files, track removal, recursive music scanning, or persistent loop-mode storage.
- A later `playlist.ini` overlay can fill `display_name` and `group`, but unlisted files must still appear under their discovered default group.
