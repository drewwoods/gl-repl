# Audio Integration Plan - `src/immediate-mode-repl/claude4.6-opus-thinking`



## Context



The OpenGL Immediate-Mode REPL currently has no audio support. The user has one

audio file they want to play and wants to stay on a path that works on Linux

now, macOS alongside the existing build, and Emscripten/gl4es in the browser

later. Long-term wish list: MP3, possibly MIDI. The user is new to audio, so

the plan also doubles as an explanation of the option space and why the pick

is the pick.



---



## Background: is audio all-or-nothing?



No - audio breaks into three independent layers. You can swap any one of them

without touching the others.



| Layer | What it does | Typical picks |

|---|---|---|

| **Backend** | Pushes PCM samples to the OS / speakers | ALSA, PulseAudio, CoreAudio, WASAPI, Web Audio |

| **Decoder** | Converts a file (MP3/WAV/…) into PCM samples | `dr_mp3`, `dr_wav`, `stb_vorbis`, `minimp3` |

| **Mixer/engine** | Optional: mixes multiple sounds, handles looping, fades | `ma_engine`, SDL_mixer, OpenAL |



"PCM" is just an array of 16-bit (or float) samples at a fixed sample rate -

e.g. `int16_t samples[44100 * 2]` is one second of 44.1 kHz stereo. Everything

else is either (a) decoding compressed bytes into that array or (b) getting

that array to the speakers.



### `/dev/dsp` - why it's a dead end



`/dev/dsp` is the old **OSS** (Open Sound System) interface. You'd do

`open("/dev/dsp")`, `ioctl()` to set format, `write()` PCM. Dead-simple, and

it still technically works on some distros via OSS emulation.



Problems for this project:

- Modern Linux uses **ALSA** (2002+), layered with **PulseAudio** / **PipeWire**.

  Most distros do not ship `/dev/dsp` without installing `osspd`/`padsp`.

- macOS has no equivalent - you'd need a second CoreAudio backend.

- Emscripten cannot use it - the browser has no filesystem devices. You'd need

  a third Web Audio backend.

- Writes are blocking; without a separate thread you'd stall the GLUT main loop.



So "roll your own" effectively means writing three backends (ALSA +

CoreAudio + Web Audio). That's exactly the work that miniaudio/raudio already

did, tested against, and ship as a single header. Not worth reinventing.



---



## Option comparison



| Option | Backends | Decoders | Emscripten | Files added | Effort to first sound |

|---|---|---|---|---|---|

| **miniaudio** (recommended) | ALSA/PA/CoreAudio/WASAPI/Web Audio | WAV+MP3+FLAC built-in | Native (Web Audio) | 1 header (`miniaudio.h`) | ~1-2 hours |

| raylib `raudio` | Same (uses miniaudio) | WAV/MP3/OGG/FLAC/XM/MOD | Works via raylib's existing web path | ~6 files (`raudio.c` + dr_*.h + stb_vorbis + jar_*) | ~2-3 hours |

| SDL2 + SDL_mixer | Cross-platform | WAV/MP3/OGG/MIDI | Yes (`-s USE_SDL=2`) | External libs (`-lSDL2 -lSDL2_mixer`) | Medium; conflicts with freeglut main loop model |

| OpenAL Soft | Cross-platform | None (needs separate decoder) | Yes (`-s USE_OPENAL=1`) | External lib | High; API is designed for 3D spatial |

| PortAudio + dr_mp3 | Linux/macOS/Win | Separate | Spotty | External lib + 1 header | Medium |

| Roll own (`/dev/dsp`, ALSA, …) | Pick one | Bring your own | No | - | Days of yak-shaving |



### Why **miniaudio** wins for this project



1. **Matches the project's single-header style.** `include/stb_image.h` is

   already vendored the same way. One header in `include/miniaudio.h`, one

   `#define MINIAUDIO_IMPLEMENTATION` in one `.c` file, done.

2. **Batteries included.** `ma_decoder` handles WAV / MP3 / FLAC out of the

   box - no separate `dr_mp3.h` needed.

3. **Emscripten-native.** It has a direct Web Audio backend; you don't need

   `-s USE_SDL=2`, which would fight with freeglut. Web Audio also satisfies

   the eventual gl4es/Emscripten goal without any SDL shim.

4. **No build system surgery.** On Linux it needs `-lpthread -lm -ldl`, all

   three of which the Makefile already links. On macOS it needs

   `-framework CoreAudio -framework CoreFoundation -framework AudioToolbox` -

   three extra flags, added alongside the existing `-framework` block.

5. **It's what raudio uses underneath anyway.** raudio is a thin wrapper

   around miniaudio. Going direct skips one layer of files.

6. **High-level API if you want it.** `ma_engine_play_sound(&engine, "x.mp3",

   NULL)` is literally one line to play a file.



### What about MIDI?



MIDI is genuinely different - it's not audio, it's a sequence of note events

that have to be synthesized. Two single-header libraries make this tractable:



- **`tml.h`** (TinyMidiLoader) - parses a `.mid` file into events

- **`tsf.h`** (TinySoundFont) - synthesizes those events against a `.sf2`

  soundfont into PCM



These feed PCM into miniaudio just like any other source. A small SoundFont

(2-10 MB) handles general-MIDI playback. Same author as `dr_mp3`/miniaudio,

same code style.



**Recommendation: defer MIDI.** It's an additive change - `repl_audio.c`

can grow a `play_midi()` entry point later without disturbing the PCM path.

Scope it as a stretch goal so the first milestone stays small.



---



## Recommended approach



**Chosen scope:** autoplay the one audio file on startup, add a `Mute` toggle to

the existing Config menu, defer MIDI entirely. No changes to the REPL language

or to save/load (the audio file is configured in `main()`, not referenced by

scenes).



### Step 1 - Vendor miniaudio



- Drop `miniaudio.h` into `include/miniaudio.h` (single file, public domain /

  MIT-0). Follows the same convention as `include/stb_image.h`.

- No other files. No submodules.



### Step 2 - Add a thin audio module



New files in `src/immediate-mode-repl/claude4.6-opus-thinking/`:



- **`repl_audio.h`** - small public API:

  ```c

  int  repl_audio_init(void);                    // once, at startup

  void repl_audio_shutdown(void);                // called via atexit

  int  repl_audio_play_music(const char *path);  // streaming, looped

  void repl_audio_stop_music(void);

  void repl_audio_set_muted(int muted);          // non-destructive silence

  int  repl_audio_is_muted(void);

  void repl_audio_on_user_gesture(void);         // browser autoplay unlock

  ```

- **`repl_audio.c`** - ~120 lines, file-scoped statics:

  - `static ma_engine g_engine;`

  - `static ma_sound  g_music;`

  - `static int       g_music_loaded;`

  - `static int       g_muted;`

  - `static int       g_gesture_ready;`  (always 1 on native, starts 0 under `__EMSCRIPTEN__`)

  - `#define MINIAUDIO_IMPLEMENTATION` must appear **exactly once**, at the

    top of this file.

  - `repl_audio_init()` → `ma_engine_init(NULL, &g_engine)`; on native,

    also call `ma_engine_start()`. On Emscripten, skip start - defer until

    `repl_audio_on_user_gesture()` fires the first time.

  - `repl_audio_play_music()` → `ma_sound_init_from_file(..., MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_ASYNC, ..., &g_music)`,

    `ma_sound_set_looping(&g_music, MA_TRUE)`, `ma_sound_start(&g_music)`.

    If a previous music handle exists, `ma_sound_uninit()` it first.

  - `repl_audio_set_muted()` → `ma_engine_set_volume(&g_engine, muted ? 0 : 1)`

    (engine-wide volume is the cleanest way to be non-destructive).

  - `repl_audio_shutdown()` → `ma_sound_uninit` (if loaded) then

    `ma_engine_uninit`. Idempotent.

  - Missing file / init failure → log to stderr, return non-zero, never crash

    the REPL. The visual path must keep working with no sound.



### Step 3 - Wire into the REPL lifecycle



- **`sample.c`** (reference: sample.c:88):

  - After `repl_init_gl();` call `repl_audio_init();` then

    `repl_audio_play_music("assets/song.mp3");` (or whichever file the user

    drops in - see Step 5).

  - After `main()` finishes argument parsing, register

    `atexit(repl_audio_shutdown);` so freeglut tear-down is clean.

- **`repl_editor.c`** - browser-gesture unlock:

  - In `repl_keyboard_func` and `repl_mouse_func`, add a single guarded call:

    ```c

    static int g_audio_gesture_sent = 0;

    if (!g_audio_gesture_sent) { repl_audio_on_user_gesture(); g_audio_gesture_sent = 1; }

    ```

  - Native: the function is a no-op after the first call, so the cost is one

    branch. Emscripten: the first call transitions `ma_engine` to started

    state, which satisfies Chrome/Safari autoplay policy.

- **Config menu mute toggle** - follows the CLAUDE.md recipe in the

  "Config Menu" section of the sample's `CLAUDE.md`:

  - Append an entry to `g_cfg_items[]` in `repl_editor.c` pointing at a

    `static int g_audio_muted = 0;` with 2 states `{"On","Muted"}`.

  - The existing render/hit-test code picks it up automatically

    (`sizeof`-derived count).

  - Wrap the toggle so flipping it calls `repl_audio_set_muted(g_audio_muted)`.

    Easiest spot: in the `handle_cfg_menu_press()` branch that increments a

    CfgItem's value, add a `if (item->value == &g_audio_muted) repl_audio_set_muted(*item->value);`

    post-hook (or make `CfgItem` carry an optional callback if the file

    already uses that pattern - worth checking during implementation).



### Step 4 - Makefile edits



- Add `repl_audio.c` to the source list at `Makefile:64`.

- Link flags split by platform (the Makefile already conditionally selects

  `GLUT_GL_LDFLAGS` vs `GL_LDFLAGS` - piggy-back on that split):

  - **Linux** (freeglut path): append `-ldl` - miniaudio `dlopen`s

    `libpulse.so` / `libasound.so` at runtime, no build-time lib needed.

  - **macOS** (both `GL_LDFLAGS` and `GLUT_GL_LDFLAGS`): append

    `-framework CoreAudio -framework CoreFoundation -framework AudioToolbox`.

- No new `-I` paths needed - `include/miniaudio.h` is reachable via the

  existing `REPO_INCLUDE` flag.



### Step 5 - Asset placement



- Create `src/immediate-mode-repl/claude4.6-opus-thinking/assets/`.

- Drop the user's single audio file there (`assets/song.mp3` in the plan;

  the actual filename is a one-line change in `sample.c`).

- `./sample` is already launched from the sample directory, so a relative

  path `"assets/song.mp3"` resolves correctly for the native build.



### Step 6 - Emscripten / gl4es web build



- Extend `emscripten/build.sh:212` (the `emcc` invocation in `build_one()`):

  add `--preload-file "${sample_dir}/assets@/assets"` so the whole `assets/`

  folder is packaged into the `.data` file and mounted at the same relative

  path the native build uses.

- No miniaudio-specific emcc flags - it autoselects the Web Audio backend

  when `__EMSCRIPTEN__` is defined. Do **not** add `-s USE_SDL=2`; freeglut

  is already the windowing layer via the existing patch.

- Threading: keep `-lpthread` out of the Emscripten link line (Web Audio

  runs on the page's audio thread; miniaudio doesn't need pthreads there).

- The Step 3 gesture hook handles Chrome/Safari autoplay policy - first key

  or mouse event starts the engine.



---



## Verification



1. **Linux native build:** `make clean && make sample`. Run `./sample`; the

   music starts within a second of the window appearing and loops.

2. **Mute toggle:** open the Config menu, flip `Audio → Muted`, confirm

   silence; flip back, sound returns without a restart (volume is the

   silencing mechanism, not stop/start).

3. **Graceful degradation:** rename `assets/song.mp3`, run `./sample` - the

   REPL launches normally and prints a single-line warning to stderr; no

   crash.

4. **Existing tests unaffected:** `make test` - all eight suites still pass.

   `repl_audio.*` is not unit-tested (it drives a real device); it's isolated

   so none of the existing tests link it.

5. **macOS smoke test (if reachable):** `make glut` still builds and links

   after the framework flags are added.

6. **Emscripten web build:** `./emscripten/build.sh src/immediate-mode-repl/claude4.6-opus-thinking/sample.c`

   produces `emscripten/out/immediate-mode-repl/index.html`. Open it, click

   once anywhere, and confirm music plays. Browser devtools console should

   be free of autoplay-policy warnings after the first click.



---



## Critical files



- **Modify:**

  - `Makefile` - add source, add link flags

  - `sample.c` - init/play/atexit calls after `repl_init_gl()`

  - `repl_editor.c` - gesture hook in keyboard/mouse callbacks,

    `g_cfg_items[]` mute entry

  - `emscripten/build.sh` - `--preload-file` flag in `build_one()`

- **Create:**

  - `src/immediate-mode-repl/claude4.6-opus-thinking/repl_audio.h`

  - `src/immediate-mode-repl/claude4.6-opus-thinking/repl_audio.c`

  - `include/miniaudio.h` (vendored)

  - `src/immediate-mode-repl/claude4.6-opus-thinking/assets/` + audio file

- **Read (for patterns, no edits):**

  - `include/gl_includes.h` - vendoring style

  - `sample.c` (sample.c:88) - init-order reference

  - CLAUDE.md "Config Menu" section - CfgItem recipe

  - `emscripten/build.sh` (build.sh:212) - emcc command location



## Out of scope (intentionally deferred)



- **MIDI playback.** Can be added later by vendoring `tml.h` + `tsf.h` and a

  SoundFont, and adding a `repl_audio_play_midi()` entry point that feeds

  synthesized PCM into the same `ma_engine`. No changes to the Step 1-6 work

  would be needed.

- **REPL `playSound()` language command.** The audio file is wired in

  `main()`, not reachable from the REPL or exported scenes. Add later via the

  `CmdType` recipe in the sample's CLAUDE.md if needed.

- **Multiple concurrent sounds / mixing.** `ma_engine` already supports it;

  the current API intentionally exposes only single-track music for

  simplicity.

