# assets/

Runtime data for the REPL sample. The audio backend (`repl_audio.c` via
`miniaudio.h`) looks for background music here.

## Music

`sample.c` scans this folder at startup for files matching `*.mp3`,
sorts them by filename, and plays them as a playlist. Drop one file,
drop ten — the REPL just picks them up in alphabetical order.

```
assets/
  01-intro.mp3
  02-main-theme.mp3
  03-outro.mp3
```

The `Audio` entry in the Config menu (gear icon) toggles between
`On` and `Muted` without stopping playback. The adjacent `Audio loop`
entry cycles the loop mode:

| Mode | Behavior |
|---|---|
| `Off`  | Plays the playlist through once, then stops |
| `Song` | Repeats the current track forever |
| `All`  | Plays the playlist, wraps back to the first track at the end (default) |

If `assets/` contains no `.mp3` files, the sample falls back to the
legacy single-file default `assets/song.mp3` — so older setups with
one dropped-in track keep working unchanged. The same fallback is
overridable via `-DREPL_AUDIO_DEFAULT_MUSIC='"assets/yourfile.mp3"'`
in `CFLAGS`.

miniaudio's built-in decoders also support WAV and FLAC, but the
startup scanner is deliberately restricted to `*.mp3`. If you need
other formats, either rename them or call `repl_audio_set_playlist()`
yourself from `main()`.

If the files are missing or fail to load, the REPL still runs — you'll
see a single `repl_audio:` warning on stderr per failure and no sound.
There are no hard audio dependencies.

## Web build

For `emscripten/build.sh` to bundle the assets folder into the `.data`
package, the build script passes
`--preload-file "<sample>/assets@/assets"` to `emcc`. The virtual-FS
mount point matches the native relative path so no code changes.

## Deliberately gitignored

This folder is included in the repository so the directory structure
is versioned, but actual audio files are **not** committed — see the
local `.gitignore`.
