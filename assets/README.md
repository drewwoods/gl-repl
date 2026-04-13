# assets/

Runtime data for the REPL sample. The audio backend (`repl_audio.c` via
`miniaudio.h`) looks for background music here.

## Music

`sample.c` plays a single looped track on startup. The default path is
compiled in as:

```c
#define REPL_AUDIO_DEFAULT_MUSIC "assets/song.mp3"
```

Drop your file at `assets/song.mp3`, or override the default by passing
`-DREPL_AUDIO_DEFAULT_MUSIC='"assets/yourfile.mp3"'` to `CFLAGS` when
building. Supported formats (via miniaudio's built-in decoders):

- WAV
- MP3
- FLAC

If the file is missing or fails to load, the REPL still runs — you'll
see a single `repl_audio:` warning on stderr and no sound. There are no
hard audio dependencies.

The `Audio` entry in the Config menu (gear icon) toggles between
`On` and `Muted` without stopping playback.

## Web build

For `emscripten/build.sh` to bundle the assets folder into the `.data`
package, the build script passes
`--preload-file "<sample>/assets@/assets"` to `emcc`. The virtual-FS
mount point matches the native relative path so no code changes.

## Deliberately gitignored

This folder is included in the repository so the directory structure
is versioned, but actual audio files are **not** committed — see the
local `.gitignore`.
