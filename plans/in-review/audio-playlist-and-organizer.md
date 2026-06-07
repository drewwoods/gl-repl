# Audio Menu: Playlist Browser, Play/Remove, and Tag Organization

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
by the Scene (examples) and Tutorials menus — the `CatalogFlyoutOps` vtable in
`src/ui/app/menu_bar.c`. No new menu/flyout rendering machinery is needed; we
supply a third ops table and three small dispatch branches.

## Approach

### 1. New audio-module accessors + mutators (`src/app/glr_audio.{c,h}`)

The playlist already lives in `glr_audio.c` as
`g_playlist[GLR_AUDIO_MAX_TRACKS][GLR_AUDIO_MAX_PATH]`, `g_playlist_count`,
`g_playlist_pos`. Add a thin public surface over it (all guarded by the existing
`audio_lock()` recursive mutex, mirroring `glr_audio_next_track`):

```c
int         glr_audio_track_count(void);            /* g_playlist_count */
const char *glr_audio_track_path(int idx);          /* g_playlist[idx], NULL if OOR */
int         glr_audio_current_index(void);          /* g_playlist_pos, -1 if nothing loaded */
int         glr_audio_play_track(int idx);          /* set pos=idx; worker_post(AWR_START,idx,NO_SEEK) */
int         glr_audio_remove_track(int idx);        /* rebuild playlist minus idx */
```

- `glr_audio_play_track(idx)` mirrors the body of `glr_audio_next_track()`
  (around `glr_audio.c` next/prev + `request_start`): validate `idx`, set
  `g_playlist_pos = idx`, post `AWR_START` with `GLR_AUDIO_NO_SEEK`, respect the
  current paused flag. Bumps the track generation on actual start (existing
  worker path).
- `glr_audio_remove_track(idx)` under `audio_lock()`: shift `g_playlist[]` /
  `g_playlist_count` to delete `idx`. If the removed index **was** the current
  (`g_playlist_pos`): the next track now occupies index `idx` (or wrap to 0 if it
  was the last) — set `g_playlist_pos` accordingly and `request_start` it (the
  "advance to next" behavior). If a load is in flight for the removed track, set
  the existing `g_load_cancelled` flag. If the playlist becomes empty, post
  `AWR_UNINIT`. If the removed index was *below* the current, decrement
  `g_playlist_pos` so it keeps pointing at the same track. Returns 0/-1.

These are pure additions — no behavior change for existing callers.

### 2. New tag catalog module (`src/app/glr_audio_tags.{c,h}`)

A small **app-layer** module (no GL, no threads) that owns the parsed tag map
and presents the playlist as a tag-grouped catalog. It reads track paths from
`glr_audio_*` (above), so it stays in sync with removals automatically.

- `void glr_audio_tags_init(const char *tags_file_path);` — parse `tags.txt`
  once at startup. Each line: `stem: [tagA, tagB, ...]`. Store `stem → tag list`.
  Tolerant parser (skip blank/`#` lines, trim whitespace, ignore malformed).
- Match a playlist track to its tags by **basename stem** (basename of
  `glr_audio_track_path(idx)` with the `.mp3` extension stripped), matching the
  user's `The_Save_Point` style (no extension).
- Derive the **visible tag set**: the union of all tags seen in the file (stable
  order — first-seen or alphabetical), then append synthetic **Untagged** (only
  when ≥1 track has no tags) and **All** (always last, every track). Even with no
  `tags.txt`, "All" alone gives a flat playlist flyout.

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

### 3. Register the Audio menu (`src/app/glr_actions.h`, `src/ui/app/menu_bar.c`)

- `glr_actions.h`: add `GLR_MENU_AUDIO` to `GlrMenuId` (before `GLR_MENU_COUNT`).
- `menu_bar.c`:
  - Add alias `MENU_AUDIO = GLR_MENU_AUDIO` (next to `MENU_CONFIG`) and `"Audio"`
    to `g_menu_labels[]`. `NUM_MENUS` scales automatically; `menubar_rects`
    already derives widths from labels.
  - `menu_item_count`: `MENU_AUDIO → glr_audio_tags_visible_tag_count()` (top-level
    rows are tag rows, hover-only flyout parents — mirrors the Tutorials tag-row
    pattern).
  - `menu_item_label`: `MENU_AUDIO → glr_audio_tags_tag_label(i)`.
  - `menu_row_has_submenu`: `MENU_AUDIO` tag rows have submenus.
  - Add a third `CatalogFlyoutOps`:
    ```c
    static const CatalogFlyoutOps kAudioCatalogOps = {
        .count_for_tag = glr_audio_tags_count_for_tag,
        .index_for_tag = glr_audio_tags_index_for_tag,
        .name_of       = glr_audio_tags_track_name,
        .subheading_of = audio_no_subheading,   /* returns NULL — flat groups */
    };
    ```
    plus a `kAudioProvider` (FlyoutProvider) whose `row_count/label/abs_index/kind`
    wrap `catalog_flyout_row_count` / `catalog_flyout_row_at(&kAudioCatalogOps,…)`
    exactly as `kSceneProvider`/`kTutorialProvider` do. Add `MENU_AUDIO →
    &kAudioProvider` to `flyout_provider_for`.
  - `submenu_row_is_active`: `MENU_AUDIO → abs_idx == snap->audio.current_idx`
    (the accent-color highlight, same path examples/tutorials use). This is the
    "► currently playing" indicator.

Flyout scrolling for long playlists already works generically
(`g_submenu_scroll` / `ui_menu_bar_handle_wheel_scroll`), so no extra work there.

### 4. Snapshot field (`src/ui/app/snapshot.h`, `src/app/glr_ctrl.c`)

Add an `audio` struct to `UiRenderSnapshot` with `int current_idx;` and populate
it in `glr_ctrl_build_ui_snapshot()` from `glr_audio_current_index()`. Keeps the
render/hit path snapshot-pure (no live `glr_audio_*` calls during render),
consistent with how `tutorial.tutorial_idx` / `scenes.active_example_idx` are
captured.

### 5. Click routing (`src/app/glr_ctrl_router.c`, `src/app/glr_actions.c`)

- **Top-level tag rows are inert** on click (hover-open only): add a
  `GLR_MENU_AUDIO` branch to `glr_action_menu_item_activate` that returns 0 for
  any `item_idx` (mirrors the `MENU_TUTORIALS` tag-row guard).
- **Left-click a song** (`route_submenu_item_hit` in `glr_ctrl_router.c`):
  ```c
  if (hit->cmd_idx == GLR_MENU_AUDIO) {
      glr_audio_play_track(hit->item_idx);
      editor_request_redraw();
      return 1;              /* keep menu open, like Config */
  }
  ```
- **Right-click a song** → remove. Generalize the existing config right-press so
  it works for any flyout: rename/extend `ui_menu_bar_handle_config_right_press`
  to return the submenu hit for whatever menu is open (it already calls
  `submenu_hit_test`, which fills `cmd_idx = menu_id`). Then in the right-button
  section of `glr_ctrl_mouse` (after `handle_right_config_press`), add
  `glr_ctrl_router_handle_right_audio_press`:
  ```c
  UiHit hit = ui_panels_handle_right_press(x, y);
  if (hit.kind == UI_HIT_SUBMENU_ITEM && hit.cmd_idx == GLR_MENU_AUDIO && hit.item_idx >= 0) {
      glr_audio_remove_track(hit.item_idx);
      editor_request_redraw();
      return 1;             /* menu stays open */
  }
  ```
  (Config keeps its existing backward-cycle on right-press; the router switches
  on `cmd_idx`.)

### 6. Startup wiring (`gl_repl.c`)

After the playlist is built and `glr_audio_set_playlist(...)` is called, call
`glr_audio_tags_init(<user music dir>/tags.txt)`. `gl_repl.c` already computes
the per-user music dir via `user_music_dir()`; reuse it to build the path. (Tags
load once at startup; editing `tags.txt` takes effect on next launch — a reload
hook can be a later follow-up.)

## Critical files

| File | Change |
|---|---|
| `src/app/glr_audio.h` / `.c` | New accessors + `glr_audio_play_track` / `glr_audio_remove_track` (mirror `next_track`/`request_start`, guarded by `audio_lock`) |
| `src/app/glr_audio_tags.h` / `.c` | **New** module: parse `tags.txt`, derive tag groups, catalog query API |
| `src/app/glr_actions.h` | Add `GLR_MENU_AUDIO` to `GlrMenuId`; inert tag-row guard in `glr_action_menu_item_activate` |
| `src/ui/app/menu_bar.c` | `MENU_AUDIO` alias + label; `menu_item_count/label`, `menu_row_has_submenu`, `kAudioCatalogOps`/`kAudioProvider`, `flyout_provider_for`, `submenu_row_is_active`; generalize `*_config_right_press` to any flyout |
| `src/ui/app/snapshot.h` | Add `audio.current_idx` |
| `src/app/glr_ctrl.c` | Populate `snap->audio.current_idx` in `glr_ctrl_build_ui_snapshot` |
| `src/app/glr_ctrl_router.c` | Left-click play branch in `route_submenu_item_hit`; new right-press remove handler wired into `glr_ctrl_mouse` |
| `gl_repl.c` | `glr_audio_tags_init(<user music dir>/tags.txt)` after playlist setup |
| `Makefile` | Add `glr_audio_tags.c` to `$(SRCS)` (and any stub/test object lists) |

## Verification

- **Build & guards:** `make gl-repl`, `make test`, `make check-c99`,
  `make check-state-ownership`, `make test-stubs`, `make gl-repl USE_GL_STUBS=1`.
  Cross-check under real GCC on `gracemont` per CLAUDE.md (`make check-c99 &&
  make test-stubs`).
- **Unit tests:** extend `tests/test_audio.c` for `play_track` (index bounds,
  current-index update) and `remove_track` (mid-list, removing current →
  advances, removing below-current keeps pointer, empties to nothing). Add a new
  `tests/test_audio_tags.c` for the `tags.txt` parser (well-formed,
  malformed/blank lines, stem matching, synthetic `Untagged`/`All`, tag
  ordering). Wire both into the Makefile test targets.
- **Manual / headless:** run `./gl-repl` with ≥2 `.mp3`s in `./assets` and a
  `tags.txt`. Confirm: the **Audio** menu shows tag rows; hovering a tag opens a
  flyout of its songs; the playing track shows the accent highlight; left-click
  another song starts it (highlight follows); right-click a song removes it from
  the list (and if it was playing, the next track starts); the **All** flyout
  lists every remaining track and scrolls when long. Verify a run with **no**
  `tags.txt` still shows a usable **All** group.
