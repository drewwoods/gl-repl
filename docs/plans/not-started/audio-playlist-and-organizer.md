# Audio Menu: Playlist Browser, Play/Remove, and Tag Organization

## Status - PARTLY OVERTAKEN (2026-07-29 audit)

`done/audio-menu.md` (landed 2026-07-06) shipped the browser half of this plan
independently, so the Context section below is stale where it says there is no
playlist UI and no play-track-N API. Already in `main`: the top-level **Audio**
menu (`GLR_MENU_AUDIO`) with a `CatalogFlyoutOps` provider, tracks grouped into
flyouts, the playing track highlighted with live `m:ss / m:ss`, and
`glr_audio_track_count` / `_track_display_name` / `_track_group` /
`_current_index` / `_play_track_index`.

What is still unbuilt, and all this plan is now for:

- **Right-click removal** - no `glr_audio_remove_track()`; §1's removal
  semantics (index shifting, removing the current track, `g_load_cancelled`
  during `g_loading`) are untouched and remain the substantive design content.
- **Tag organization** - grouping is by *source* (`Assets` / `Bundled` /
  `My Music` / `Default`), not user tags. No `tags.txt` parsing anywhere in the
  tree, no `All` / `Untagged` synthetic groups.
- `glr_audio_track_path()` - not added; the menu reads display names instead.

Re-scope against the shipped menu before implementing; do not re-derive the
accessors that already exist.

## Context

The REPL plays background music from a playlist built at startup
(`build_mp3_playlist()` in `gl_repl.c`), but there is **no UI** to see what's
in the playlist, what's currently playing, or to control which track plays.
The audio module (`src/app/glr_audio.c`) is "scan-once-at-startup, write-light":
it can advance next/prev but has no *play-track-N* or *remove-track* API, and no
way to query the playlist contents.

This change adds a top-level **Audio** menu to the menu bar that:
1. Lists the playlist songs, highlighting the currently-playing track.
2. **Left-click** a song → play it. **Right-click** a song → remove it from the
   in-memory playlist for this session (the `.mp3` on disk is untouched).
3. Organizes songs by **tags** read from a plain-text file
   (`<user music dir>/tags.txt`) in the form `stem: [tag1, tag2]`, e.g.
   `The_Save_Point: [favorite, synth]`. Each tag becomes a hover-flyout group,
   plus a synthetic **All** group (every track) and **Untagged** group.

Decisions confirmed with the user: tag-grouped flyouts; session-only removal
(non-destructive); tag file at the per-user music dir; removing the current
track advances to the next.

The Audio menu rides the **existing tag-grouped flyout engine** already shared
by the Scene (examples) and Tutorials menus - the `CatalogFlyoutOps` vtable in
`src/ui/app/menu_bar.c`. No new menu/flyout rendering machinery is needed; we
supply a third ops table and three small dispatch branches.

## Approach

### 1. New audio-module accessors + mutators (`src/app/glr_audio.{c,h}`)

The playlist already lives in `glr_audio.c` as
`g_playlist[GLR_AUDIO_MAX_TRACKS][GLR_AUDIO_MAX_PATH]`, `g_playlist_count`,
`g_playlist_pos`. Add a thin public surface over it (all guarded by the existing
`audio_lock()` mutex - dynamically initialized as `PTHREAD_MUTEX_RECURSIVE` in
`glr_audio_init()` - mirroring `glr_audio_next_track`):

```c
int         glr_audio_track_count(void);            /* g_playlist_count */
const char *glr_audio_track_path(int idx);          /* g_playlist[idx], NULL if OOR */
int         glr_audio_current_index(void);          /* g_playlist_pos, -1 if nothing loaded */
int         glr_audio_play_track(int idx);          /* request_start(idx, GLR_AUDIO_NO_SEEK) */
int         glr_audio_remove_track(int idx);        /* rebuild playlist minus idx */
```

- `glr_audio_current_index()` - `g_playlist_pos` is `0` at static init (not
  `-1`), and the current-track display should follow the same loaded-state
  contract as `glr_audio_get_current_track()`: return `-1` when
  `!g_music_loaded` or `g_playlist_pos` is out of range, otherwise return
  `g_playlist_pos`. Do **not** key this only off `g_active`; remove-current can
  deliberately leave an old `ma_sound` slot active for worker cleanup while the
  UI/state-file view should already consider there to be no current track.
- `glr_audio_track_path(idx)` returns a `const char *` into
  `g_playlist[idx]`. **Threading contract:** both this accessor and all playlist
  mutations (`remove_track`, `set_playlist`) run on the main thread, so the
  pointer stays valid until the next mutating call. The tag module and menu
  renderer may cache the pointer within a single frame.
- `glr_audio_play_track(idx)` mirrors `glr_audio_next_track()` / `prev_track()`:
  validate `idx`, then call `request_start(idx, GLR_AUDIO_NO_SEEK)` (float, not
  double). **Do not set `g_playlist_pos` early** in this public API. On native
  builds the worker publishes the new current track only after `worker_load()`
  succeeds (`g_playlist_pos = idx; g_track_generation++`); that keeps the menu
  highlight, status text, and state-file save aligned with the sound that is
  actually loaded. The Emscripten deferred-gesture path may still record a
  pending target inside `request_start()`, but `glr_audio_current_index()` stays
  `-1` until a track is loaded because it checks `g_music_loaded`.
- `glr_audio_remove_track(idx)` under `audio_lock()`: validate `idx`, treat any
  playlist mutation during `g_loading` as invalidating the worker's copied
  request index/path (`g_load_cancelled = 1`), then shift `g_playlist[]` /
  `g_playlist_count` to delete `idx`. Cases:
  - Removed index **below** the current loaded track: decrement `g_playlist_pos`
    so it still points at the same path after the shift.
  - Removed index **above** the current loaded track: keep `g_playlist_pos`.
  - Removed index **is** the current loaded track: synchronously stop the active
    slot with `ma_sound_stop()` under the existing audio lock (same risk profile
    as pause/resume), set `g_music_loaded = 0`, clear any pending start for the
    removed track, then after the list shift either request the next track or
    uninit. The next track is the element that shifted into `idx`; if the removed
    track was the last element, wrap to `0`. Do not publish that replacement via
    `g_playlist_pos` until the worker successfully loads it. This avoids
    `worker_save_state()` writing the replacement path with the removed track's
    cursor while the old slot is still retiring. While that old slot remains in
    `g_active` for worker cleanup, active-slot control paths (`set_paused`,
    `tick` end-of-track advance, and any similar future path) must also gate on
    `g_music_loaded` so they do not restart or advance the removed stopped slot.
  - Playlist becomes empty: clear pending start/load state as needed and post
    `AWR_UNINIT` (the only "drop current sound" request - there is no `AWR_STOP`;
    the full enum is `AWR_NONE / AWR_START / AWR_ADVANCE / AWR_UNINIT /
    AWR_QUIT`).

  Also adjust or clear any queued/pending start index that points at or past the
  removed element (`g_req == AWR_START`, `g_pending_start`) so a stale submenu
  index cannot start a different track after the array compacts. Returns 0/-1.

These are pure additions - no behavior change for existing callers.

### 2. New tag catalog module (`src/app/glr_audio_tags.{c,h}`)

A small **app-layer** module (no GL, no threads) that owns the parsed tag map
and presents the playlist as a tag-grouped catalog. It reads track paths from
`glr_audio_*` (above), so it stays in sync with removals automatically.

- `void glr_audio_tags_init(void);` - self-resolves the tag file path (see §6)
  and parses `tags.txt` once at startup. Each line: `stem: [tagA, tagB, ...]`.
  Store `stem → tag list`. Tolerant parser (skip blank/`#` lines, trim
  whitespace, ignore malformed).
- Add a narrow parser test seam, e.g.
  `int glr_audio_tags_load_file_for_test(const char *path)` or an internal
  load-from-path helper used by both `glr_audio_tags_init()` and the test. The
  test must not write into the user's real music directory just to exercise
  parser cases.
- Match a playlist track to its tags by **basename stem** (basename of
  `glr_audio_track_path(idx)` with the `.mp3` extension stripped), matching the
  user's `The_Save_Point` style (no extension). The stem extractor handles both
  `/` and `\` path separators for Windows-port readiness.
- Derive the **visible tag set**: the union of all tags seen in the file (stable
  order - first-seen or alphabetical), **filtered to only tags that match at
  least one track in the current playlist** (so stale/irrelevant tags from
  `tags.txt` don't produce empty flyouts), then append synthetic **Untagged**
  (only when ≥1 track is tagged AND ≥1 track has no tags - if all tracks are
  untagged, skip it) and **All** (always last, every track). Even with no
  `tags.txt`, "All" alone gives a flat playlist flyout.
- **Duplicate stems.** If a stem appears on multiple lines in `tags.txt`, merge
  the tag lists into a single entry rather than creating duplicates.

Catalog query API matching the shape the menu already consumes for examples/
tutorials (`repl_example_*` / `repl_tutorial_*`):

```c
int         glr_audio_tags_visible_tag_count(void);
const char *glr_audio_tags_tag_label(int tag_ord);        /* "favorite", "Untagged", "All" */
int         glr_audio_tags_count_for_tag(int tag_ord);    /* # tracks in that group */
int         glr_audio_tags_index_for_tag(int tag_ord, int ord); /* -> playlist track index */
const char *glr_audio_tags_track_name(int track_idx);     /* basename stem for display */
```

`*_index_for_tag` returns the **absolute playlist index** so it round-trips
straight into `glr_audio_play_track` / `glr_audio_remove_track`.

**Index invalidation on removal.** The absolute indices that `*_index_for_tag`
returns shift when `glr_audio_remove_track` deletes a track (elements above the
removed index slide down by one). Rather than caching and invalidating, the tag
module re-derives indices on every query by walking `glr_audio_track_count()` /
`glr_audio_track_path(i)` and matching stems. This is O(tracks × tags-per-query)
but fine for the 64-track cap; it keeps the module stateless w.r.t. playlist
mutations and avoids a generation-counter protocol between the two modules.

### 3. Register the Audio menu (`src/app/glr_actions.h`, `src/ui/app/menu_bar.c`)

- `glr_actions.h`: add `GLR_MENU_AUDIO` after `GLR_MENU_CONFIG` (before
  `GLR_MENU_COUNT`) in the `GlrMenuId` enum. The current enum is
  `GLR_MENU_FILE / GLR_MENU_SCENE / GLR_MENU_TUTORIALS / GLR_MENU_CONFIG /
  GLR_MENU_COUNT`; audio becomes the fifth member.
- `menu_bar.c`:
  - Add alias `MENU_AUDIO = GLR_MENU_AUDIO` (next to `MENU_CONFIG`) and append
    `"Audio"` to `g_menu_labels[]` (after `"Config"` - rightmost position).
    `NUM_MENUS` is `= GLR_MENU_COUNT` (an enum alias, `menu_bar.c:30`), so adding
    `GLR_MENU_AUDIO` to `GlrMenuId` bumps the count and `NUM_MENUS` tracks it
    automatically. But `g_menu_labels[NUM_MENUS]` is a fixed-size initializer:
    the `"Audio"` string literal **must** be added in lockstep, or the array
    under-sizes and label/menu indices desync. `menubar_rects` already derives
    widths from the labels.
  - `menu_item_count`: `MENU_AUDIO → 0` when the playlist is empty, otherwise
    `glr_audio_tags_visible_tag_count()` (top-level rows are tag rows,
    hover-only flyout parents - mirrors the Scene/Tutorials tag-row pattern).
    The `snap` path can use `snap->audio.track_count`; the existing `NULL`
    layout/hit fallback can use `glr_audio_track_count()`.
  - `menu_item_label`: `MENU_AUDIO → glr_audio_tags_tag_label(i)`.
  - `menu_row_has_submenu`: `MENU_AUDIO` tag rows have submenus.
  - Add a third `CatalogFlyoutOps` (alongside the existing `kExampleCatalogOps`
    and `kTutorialCatalogOps`):
    ```c
    static const CatalogFlyoutOps kAudioCatalogOps = {
        .count_for_tag = glr_audio_tags_count_for_tag,
        .index_for_tag = glr_audio_tags_index_for_tag,
        .name_of       = glr_audio_tags_track_name,
        .subheading_of = audio_no_subheading,   /* returns NULL - flat groups */
    };
    ```
    plus a `kAudioProvider` (`FlyoutProvider`) whose
    `row_count/row_label/row_abs_index/row_kind` wrap
    `catalog_flyout_row_count` / `catalog_flyout_row_at(&kAudioCatalogOps,…)`
    exactly as `kSceneProvider`/`kTutorialProvider` do. Add `MENU_AUDIO →
    &kAudioProvider` to `flyout_provider_for`.
  - `submenu_row_is_active`: add a `MENU_AUDIO` branch. The function takes
    `(int menu_id, int parent_row, int ordinal, const UiRenderSnapshot *snap)`;
    the new branch calls `submenu_row_abs_index(menu_id, parent_row, ordinal)` to
    get the absolute playlist index, then returns
    `abs_idx >= 0 && abs_idx == snap->audio.current_idx` (the accent-color
    highlight - same pattern as the Scene/Tutorials branches). This is the
    "► currently playing" indicator.

Flyout scrolling for long playlists already works generically
(`g_submenu_scroll` / `ui_menu_bar_handle_wheel_scroll`), so no extra work there.

**Dynamic menu content / cache invalidation.** Audio is the first flyout catalog
whose row count can shrink while the menu remains open. `menu_dropdown_rect` and
`submenu_rect` cache geometry by menu/window/parent row only, so a removal can
otherwise leave stale widths, heights, or parent-row mappings. Add a small
menu-bar API such as `ui_menu_bar_note_content_changed()` that clears
`g_dropdown_cache` / `g_submenu_cache`, clamps or resets `g_submenu_scroll`, and
resets the open submenu if the current `(menu_id, parent_row)` no longer has a
submenu. Call it after successful `glr_audio_remove_track()` before
`editor_request_redraw()`. Left-click play does not change geometry and does not
need this invalidation.

### 4. Snapshot field (`src/ui/app/snapshot.h`, `src/app/glr_ctrl.c`)

Add an `audio` struct to `UiRenderSnapshot` with `int current_idx;` and
`int track_count;`, and populate both in `glr_ctrl_build_ui_snapshot()` from
`glr_audio_current_index()` / `glr_audio_track_count()`. This makes the active
highlight and empty-playlist display frame-stable, consistent with how
`tutorial.tutorial_idx` / `scenes.active_example_idx` are captured.

This is **not** a full audio-catalog snapshot: the provider described above
still calls `glr_audio_tags_*`, and that module intentionally re-derives indices
from the live playlist so removals are reflected without a cross-module
generation protocol. Keep that contract explicit in comments. If a later purity
pass wants a fully frozen menu, it should snapshot the visible audio tag labels
and per-tag playlist-index rows into `UiRenderSnapshot`; this plan does not need
that extra structure.

### 5. Click routing (`src/app/glr_ctrl_router.c`, `src/app/glr_actions.c`)

- **Top-level tag rows are inert** on click (hover-open only): add a
  `GLR_MENU_AUDIO` branch to `glr_action_menu_item_activate` that returns 0 for
  any `item_idx` (mirrors the `MENU_SCENE` / `MENU_TUTORIALS` tag-row guard -
  both already return 0).
- **Left-click a song** (`route_submenu_item_hit` in `glr_ctrl_router.c`):
  ```c
  if (hit->cmd_idx == GLR_MENU_AUDIO) {
      glr_audio_play_track(hit->item_idx);
      editor_request_redraw();
      return 1;              /* keep menu open, like Config */
  }
  ```
- **Right-click a song** → remove. Right presses are dispatched by
  `route_right_press` in `src/app/glr_ctrl_router.c`: one canonical
  `ui_panels_hit_test`, then a switch on the hit kind. Its
  `UI_HIT_SUBMENU_ITEM` case already receives the owning menu id in
  `hit.cmd_idx` (today only `GLR_MENU_CONFIG` acts on it - backward
  cycle), so audio needs no new plumbing: add a
  `hit.cmd_idx == GLR_MENU_AUDIO` branch that removes the track at
  `hit.item_idx` and keeps the dropdown open.

  ```c
  int glr_ctrl_router_handle_right_audio_press(int button, int state, int x, int y) {
      if (state != GLUT_DOWN || button != GLUT_RIGHT_BUTTON) return 0;
      UiHit hit = ui_panels_handle_right_press(x, y);
      if (hit.kind == UI_HIT_SUBMENU_ITEM && hit.cmd_idx == GLR_MENU_AUDIO && hit.item_idx >= 0) {
          if (glr_audio_remove_track(hit.item_idx) == 0)
              ui_menu_bar_note_content_changed();
          editor_request_redraw();
          return 1;             /* menu stays open */
      }
      return 0;
  }
  ```
  Config keeps its existing `handle_right_config_press` semantics
  (backward-cycle on `GLR_MENU_CONFIG`); the only shared change is that the
  lower-level right-hit function becomes generic instead of Config-filtered.

### 6. Startup wiring (`gl_repl.c`)

After the playlist is built and `glr_audio_set_playlist(...)` is called (the
public signature is `int glr_audio_set_playlist(const char *const *paths, int
count)`), call `glr_audio_tags_init()`. The tag module self-resolves the file
path: `glr_paths_user_music_dir(char *buf, size_t buflen)` (public API in
`src/app/glr_paths.h` - the old `user_music_dir()` static was refactored out)
**fills a caller buffer and returns success** (it is not a string-returning
accessor), so `glr_audio_tags_init` writes the dir into a local buffer, appends
`/tags.txt`, and parses that - no path is threaded through `main()`. (Tags load
once at startup; editing `tags.txt` takes effect on next launch - a reload hook
can be a later follow-up.)

## Critical files

| File | Change |
|---|---|
| `src/app/glr_audio.h` / `.c` | New accessors + `glr_audio_play_track` / `glr_audio_remove_track` (mirror `next_track`/`request_start`, guarded by `audio_lock`) |
| `src/app/glr_audio_tags.h` / `.c` | **New** module: parse `tags.txt`, derive tag groups, catalog query API |
| `src/app/glr_actions.h` | Add `GLR_MENU_AUDIO` to `GlrMenuId`; inert tag-row guard in `glr_action_menu_item_activate` |
| `src/ui/app/menu_bar.c` / `.h` | `MENU_AUDIO` alias + label; `menu_item_count/label`, `menu_row_has_submenu`, `kAudioCatalogOps`/`kAudioProvider`, `flyout_provider_for`, `submenu_row_is_active`; generic submenu right-hit helper; `ui_menu_bar_note_content_changed()` cache invalidation |
| `src/ui/app/panels.c` / `.h` | Route `ui_panels_handle_right_press()` through the generic submenu right-hit helper instead of the Config-only helper |
| `src/ui/app/snapshot.h` | Add `audio.current_idx`, `audio.track_count` |
| `src/app/glr_ctrl.c` | Populate `snap->audio` in `glr_ctrl_build_ui_snapshot` |
| `src/app/glr_ctrl_router.c` | Left-click play branch in `route_submenu_item_hit`; new `handle_right_audio_press` wired into `glr_ctrl_mouse`; call menu content invalidation after successful removal |
| `gl_repl.c` | `glr_audio_tags_init(...)` after playlist setup |
| `Makefile` | Add `glr_audio_tags.c` to `$(SRCS)` **and** `$(CORE_TEST_SRCS)` so `menu_bar.c` links in app/core-test binaries; add `glr_audio_tags.h` to `$(HDRS)`; add a custom `test_audio_tags` target outside `CORE_TEST_BINS` with `test_audio_tags.o` + `glr_audio_tags.o` + mocks, not `glr_audio.o` |

## Verification

- **Build & guards:** `make gl-repl`, `make test`, `make check-c99`,
  `make check-state-ownership`, `make test-stubs`, `make gl-repl USE_GL_STUBS=1`.
  Cross-check under real GCC on `gracemont` per CLAUDE.md (`make check-c99 &&
  make test-stubs`).
- **Unit tests:** extend `tests/test_audio.c` for `play_track` (index bounds,
  no early current-index publish before a successful load) and `remove_track`
  (mid-list, removing current → advances, removing below-current keeps pointer,
  empties to nothing, remove-current does not let pause/resume restart the
  retired slot). Add a new
  `tests/test_audio_tags.c` for the `tags.txt` parser (well-formed,
  malformed/blank lines, stem matching, duplicate-stem merging, synthetic
  `Untagged`/`All`, empty-group filtering, tag ordering). **Test mocking:**
  `test_audio_tags` should **not** link `glr_audio.c` (which pulls in miniaudio
  and pthreads); instead, define lightweight mock implementations of
  `glr_audio_track_count()` and `glr_audio_track_path()` directly in the test
  file, backed by a static array of paths, and drive parsing through the
  load-from-path test seam above. In the Makefile, add `test_audio_tags` to
  `TEST_BINS`, filter it out of `CORE_TEST_BINS`, and define
  `test_audio_tags_OBJS = $(OBJDIR)/$(TEST_DIR)/test_audio_tags.o
  $(OBJDIR)/src/app/glr_audio_tags.o` with no `glr_audio.o`. This keeps compile
  and run times fast and avoids OS audio dependencies in a pure-data-structure
  test. Wire both test binaries into the Makefile test targets.
- **Manual / headless:** run `./gl-repl` with ≥2 `.mp3`s in `./assets` and a
  `tags.txt`. Confirm: the **Audio** menu shows tag rows; hovering a tag opens a
  flyout of its songs; the playing track shows the accent highlight; left-click
  another song starts it (highlight follows); right-click a song removes it from
  the list (and if it was playing, the next track starts); the **All** flyout
  lists every remaining track and scrolls when long. Verify a run with **no**
  `tags.txt` still shows a usable **All** group.
