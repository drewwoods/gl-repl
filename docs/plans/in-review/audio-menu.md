# Audio Menu Plan

## Summary
Add a top-level `Audio` menu that reuses the existing catalog flyout engine in [menu_bar.c](/Users/drew/src/code/openGL/samples/gen-ai/gl-repl-worktree/src/ui/app/menu_bar.c). V1 groups discovered tracks by source (`Assets`, `Bundled`, `My Music`, fallback `Default`), displays filename stems, highlights the playing track, and shows live `m:ss / m:ss` only beside the active track while the menu is open.

Do not make `playlist.ini` or `g_cfg_items[]` the source of truth. The existing Config `AUDIO` row remains the global off/on toggle.

## Public Interfaces
- Add `GLR_MENU_AUDIO` to `GlrMenuId` in [glr_actions.h](/Users/drew/src/code/openGL/samples/gen-ai/gl-repl-worktree/src/app/glr_actions.h), plus Audio menu offsets for `---`, `Play/Pause`, `Next Track`, `Previous Track`, and `Loop: <mode>`.
- Extend [glr_audio.h](/Users/drew/src/code/openGL/samples/gen-ai/gl-repl-worktree/src/app/glr_audio.h) with:
  - `GlrAudioTrackSpec { path, group, display_name, subheading }`
  - `glr_audio_set_playlist_specs(...)`, while keeping `glr_audio_set_playlist(...)`
  - `glr_audio_track_count`, `glr_audio_track_display_name`, `glr_audio_track_group`, `glr_audio_track_subheading`
  - `glr_audio_current_index`, `glr_audio_play_track_index`
  - `glr_audio_current_cursor_seconds`, `glr_audio_track_duration_seconds`
- Add a small app-side audio catalog facade mirroring the example catalog queries: visible group count/label, count-for-group, index-for-group, track name, subheading. `subheading` returns `NULL` in v1.

## Implementation
- Update playlist scanning in [gl_repl.c](/Users/drew/src/code/openGL/samples/gen-ai/gl-repl-worktree/gl_repl.c) to carry source-group metadata alongside paths and call `glr_audio_set_playlist_specs`; the legacy fallback track is registered as one `Default` group track.
- Store copied track metadata inside `glr_audio.c` parallel to `g_playlist[]`; derive `display_name` from basename without `.mp3` when unspecified.
- Reset cached duration to unknown on playlist load. After `worker_load` publishes a new active track, have the worker call `ma_sound_get_length_in_pcm_frames`, cache seconds only if the same path/index is still valid, and let the UI show `--:--` until known.
- Add an Audio flyout provider in `menu_bar.c` using `CatalogFlyoutOps`, plus an Audio-specific right column in active track rows for `elapsed / total`. Use `glr_audio_current_index()` for `_is_active`.
- Route `UI_HIT_SUBMENU_ITEM` with `cmd_idx == GLR_MENU_AUDIO` to `glr_audio_play_track_index`. Close the menu on accepted track selection.
- Top-level Audio controls stay open after activation:
  - `Play/Pause` pauses/resumes while preserving loop mode and keeps the existing audio config value synchronized.
  - `Next Track` / `Previous Track` call existing audio APIs.
  - `Loop` cycles `Off -> Song -> All -> Off` as runtime state only.
- Refine the existing `glr_ctrl_tick` now-playing status to use the display name and the requested `♪ Now playing: name` text. No permanent statusbar timer.

## Tests
- Extend `test_audio` for playlist specs, derived names, groups, current index, `play_track_index`, and unknown-duration defaults.
- Extend `test_ui_menu_bar` for the Audio top-level label, grouped flyout hits, scroll behavior, and submenu payloads.
- Extend `test_glr_actions` / `test_glr_ctrl` for Audio control rows and submenu routing.
- Verify with `make test_audio test_ui_menu_bar test_glr_actions test_glr_ctrl`, then `make test-stubs` and `make check-c99`.

## Assumptions
- V1 does not implement `playlist.ini`, tag files, track removal, recursive music scanning, or persistent loop-mode storage.
- A later `playlist.ini` overlay can fill `display_name`, `group`, and `subheading`, but unlisted files must still appear under their discovered default group.

