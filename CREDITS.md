# Credits

gl-repl is MIT-licensed - see [`LICENSE`](LICENSE). This file records what it
is built on and what built it.

## Third-party code

| Component | What it does here | License |
|---|---|---|
| [freeglut](https://github.com/freeglut/freeglut) | Window, input, and GL context. Vendored and linked statically, with the Cocoa, X11/GLX, OSMesa, and Emscripten backends plus the frame-capture hooks the recording scripts drive. | X-Consortium (MIT-style) |
| [gl4es](https://github.com/ptitSeb/gl4es) | Translates the immediate-mode GL the REPL emits into WebGL2, so the web build runs the same programs as the native one. | MIT |
| [GLU](https://github.com/ptitSeb/GLU) | Tessellation, quadrics, and mipmap helpers for the web build. | SGI Free Software License B |
| [miniaudio](https://github.com/mackron/miniaudio) | Single-header audio playback for the background music. | Public domain (Unlicense) or MIT-0, at your option |
| [Emscripten](https://emscripten.org) | Compiles the web build and supplies its runtime glue. | MIT / University of Illinois NCSA |

Full license texts, pinned revisions, and the local patches carried against
each are in [`docs/THIRD_PARTY_LICENSES.md`](docs/THIRD_PARTY_LICENSES.md).

## AI tools

Much of this codebase was written with
[Claude Code](https://claude.com/claude-code); individual commits carry
`Co-Authored-By` trailers naming it. [ChatGPT](https://chatgpt.com),
[Gemini](https://gemini.google.com), and [Grok](https://grok.com) were used
alongside it for design discussion, review, and drafting. The background music
was generated with [Suno](https://suno.com) and is distributed as the
`assets-v1` release rather than checked in.

Design decisions, review, and responsibility for what ships are mine.
