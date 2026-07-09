# OS clipboard integration for the web build

## Context

gl-repl's editor already has a mature internal clipboard (`src/editor/clipboard.c`,
`EditorClipboardState` in `src/editor/state.h`) bound to Ctrl+C/X/V. On the
Emscripten web build that clipboard is currently app-internal only: keyboard
copy/paste never reaches the browser's real OS clipboard, so users cannot copy a
GL snippet out to another app or paste one in from outside.

For the web keyboard path, the browser/OS clipboard should be the source of
truth:

- Plain Ctrl/Cmd+C copies from gl-repl into the browser clipboard.
- Plain Ctrl/Cmd+X cuts in gl-repl and writes the cut text into the browser
  clipboard.
- Plain Ctrl/Cmd+V reads from the browser clipboard and pastes into gl-repl.

The editor clipboard still stays in the design, but on Emscripten keyboard
clipboard shortcuts it acts as an adapter for the existing editor operations,
not as the authoritative long-lived clipboard. That lets us reuse the mature
copy/cut/paste behavior, guards, undo behavior, and paste pipeline without
allowing stale app-internal data to override the user's OS clipboard.

Use `ClipboardEvent`'s `copy`/`cut`/`paste` DOM events, not the async
`navigator.clipboard.readText()` path, for keyboard paste. The event path is
the one browsers already fire for the user's real Ctrl/Cmd+C/X/V gesture and
gives synchronous access to `event.clipboardData`; async clipboard reads have
permission/prompt and secure-context friction, especially for paste.

Two current web-input details matter:

- `packaging/web/gl4es_bootstrap.c` has a document-level key guard that calls
  `preventDefault()` for Ctrl/Meta/Alt combinations while the canvas is focused
  so browser shortcuts do not steal GLUT-style input.
- Emscripten's JS GLUT layer registers its own `keydown` listener on `window`
  in capture phase and calls `preventDefault()` when `GLUT.getASCIIKey()` returns
  a key. A later `document` capture listener cannot reliably stop that listener.

So the fix must be at the source: make Emscripten GLUT decline plain clipboard
shortcuts, and make gl-repl's own key guard decline to `preventDefault()` them.
That leaves the browser free to synthesize the real `copy`/`cut`/`paste` event,
which becomes authoritative for the web keyboard path.

Ctrl+Shift+C and Ctrl+Shift+V are not clipboard shortcuts in gl-repl. They must
continue through GLUT unchanged (`Ctrl+Shift+C` reset camera, `Ctrl+Shift+V`
view-mode toggle).

## Design

### 1. `packaging/web/gl4es_bootstrap.c` - let plain clipboard shortcuts become DOM clipboard events

In the existing `EM_ASM` block, add one shared helper in the outer scope so both
the `GLUT.getASCIIKey` override and `installInputGuards()` can use it:

```js
function glrIsPlainClipboardCombo(event) {
    if (event.shiftKey || event.altKey) return false;
    if (!(event.ctrlKey || event.metaKey)) return false;

    var key = (event.key || '').toLowerCase();
    if (key === 'c' || key === 'x' || key === 'v') return true;

    var code = event.code || '';
    if (code === 'KeyC' || code === 'KeyX' || code === 'KeyV') return true;

    var kc = event.keyCode;
    return kc === 67 || kc === 88 || kc === 86;
}
```

Then, inside the existing `GLUT.getASCIIKey` override, before the Ctrl+letter
mapping that currently emits `KEY_CTRL_*`, decline plain clipboard shortcuts:

```js
GLUT.getASCIIKey = function(event) {
    var kc = event['keyCode'];
    var key = event['key'] || "";
    var code = event['code'] || "";

    /* Browser clipboard is authoritative for plain Ctrl/Cmd+C/X/V on web.
     * Returning null means JS GLUT will not call keyboardFunc and will not
     * preventDefault(), leaving the browser to fire copy/cut/paste. */
    if (glrIsPlainClipboardCombo(event)) return null;

    ...
};
```

Also update `shouldCancelKey()` inside `installInputGuards()`:

```js
function shouldCancelKey(event) {
    if (!canvasActive() || editableTarget(event.target)) return false;
    if (glrIsPlainClipboardCombo(event)) return false;
    var key = event.key || "";
    return event.ctrlKey || event.metaKey || event.altKey ||
        key === ' ' || key === 'Spacebar' || key === 'Tab' ||
        key === 'Backspace' || key === 'Escape' ||
        key === 'Home' || key === 'End' ||
        key === 'PageUp' || key === 'PageDown' ||
        key === 'Insert' || key === 'Delete' ||
        key.indexOf('Arrow') === 0 || isFunctionKey(key);
}
```

Do **not** rely on a document-level `stopImmediatePropagation()` listener to
block GLUT. Local Emscripten registers GLUT's keydown handler on `window` capture
phase, so document capture runs too late. The robust path is to make
`GLUT.getASCIIKey()` return `null` for the three plain browser-clipboard
shortcuts.

### 2. `src/editor/clipboard.c` - add success-returning copy/cut helpers

The current public editor copy/cut functions return `void`, which is fine for
native keyboard handling but not enough for a browser clipboard bridge. The web
copy/cut handler must know whether a fresh payload was actually produced before
it writes `event.clipboardData`.

Add success-returning variants and keep the existing API as wrappers:

```c
int editor_clipboard_copy_current_with_result(void);
int editor_clipboard_cut_current_with_result(void);

void editor_clipboard_copy_current(void) {
    (void)editor_clipboard_copy_current_with_result();
}

void editor_clipboard_cut_current(void) {
    (void)editor_clipboard_cut_current_with_result();
}
```

Return `1` only when the operation produced a fresh clipboard payload:

- input-buffer selection copied/cut
- source line/range/block copied
- source line/range/block cut and deleted

Return `0` for no-op or blocked cases:

- insert mode with no input selection
- no current source line to copy/cut
- var-declaration guard rejects copy/cut
- tutorial guard rejects cut
- any other branch that leaves the existing internal clipboard unchanged

This avoids exporting stale `EditorClipboardState` after a blocked copy/cut.

### 3. `src/app/glr_web_io.c` - bridge exports using browser clipboard as source of truth

Add `#include "editor/clipboard.h"` plus `<stdlib.h>` and `<string.h>`. Avoid
`strdup`; allocate and copy explicitly so the C99 build has no declaration
surprises.

Use a free-on-next-call static snapshot for data returned to JS:

```c
static char *g_web_clipboard_text = NULL;
static const char *g_web_clipboard_kind = "lines";

static void web_clipboard_clear_snapshot(void) {
    free(g_web_clipboard_text);
    g_web_clipboard_text = NULL;
    g_web_clipboard_kind = "lines";
}
```

Serialize the current editor clipboard only after a success-returning copy/cut
helper says a fresh payload exists:

```c
static int web_clipboard_snapshot_from_editor(void) {
    const EditorClipboardState *cb = editor_state_clipboard();
    web_clipboard_clear_snapshot();

    if (cb->kind == EDITOR_CLIPBOARD_INPUT_TEXT) {
        g_web_clipboard_kind = "input";
        /* malloc(input_text_len + 1), memcpy, NUL terminate */
    } else if (cb->kind == EDITOR_CLIPBOARD_LINES) {
        g_web_clipboard_kind = "lines";
        /* newline-join cb->lines[0..line_count) */
    } else {
        return 0;
    }

    return g_web_clipboard_text != NULL;
}

EMSCRIPTEN_KEEPALIVE
int glr_web_clipboard_copy(void) {
    if (!editor_clipboard_copy_current_with_result()) {
        web_clipboard_clear_snapshot();
        return 0;
    }
    return web_clipboard_snapshot_from_editor();
}

EMSCRIPTEN_KEEPALIVE
int glr_web_clipboard_cut(void) {
    if (!editor_clipboard_cut_current_with_result()) {
        web_clipboard_clear_snapshot();
        return 0;
    }
    return web_clipboard_snapshot_from_editor();
}

EMSCRIPTEN_KEEPALIVE
const char *glr_web_clipboard_text(void) {
    return g_web_clipboard_text ? g_web_clipboard_text : "";
}

EMSCRIPTEN_KEEPALIVE
const char *glr_web_clipboard_kind(void) {
    return g_web_clipboard_kind;
}
```

For paste, browser clipboard data is authoritative. Populate
`EditorClipboardState` from the DOM event payload immediately before calling the
existing editor paste operation:

```c
EMSCRIPTEN_KEEPALIVE
int glr_web_clipboard_paste_text(const char *text, const char *kind) {
    if (!text || !text[0]) return 0;

    if (kind && strcmp(kind, "input") == 0) {
        editor_clipboard_set_input_text(text, (int)strlen(text));
    } else if (kind && strcmp(kind, "lines") == 0) {
        web_clipboard_stage_lines(text);
    } else if (strchr(text, '\n')) {
        web_clipboard_stage_lines(text);
    } else if (editor_input_selection_active() || editor_insert_mode()) {
        editor_clipboard_set_input_text(text, (int)strlen(text));
    } else {
        web_clipboard_stage_lines(text);
    }

    editor_clipboard_paste_current();
    return 1;
}
```

The custom `kind` preserves exact round-trips for gl-repl-originated copies:
input substring copies paste back as input text, line/range/block copies paste
back as source lines. If the clipboard came from another app and only has
`text/plain`, use the fallback heuristic above:

- multiline text becomes source lines
- single-line text replaces an active input selection or inserts into the input
  row while already in insert mode
- otherwise single-line text becomes one source line, which is the useful
  default for pasting snippets into the code panel

`web_clipboard_stage_lines()` should split CRLF/LF text into
`EditorClipboardState.lines`, clamp each line to `MAX_LINE_LEN - 1`, clamp the
count to `MAX_COMMANDS`, and call `editor_state_clipboard_count_set(n)`. Drop a
single trailing empty line caused by a terminal newline; otherwise preserve the
non-empty source lines that will be fed through the existing paste pipeline.

### 4. `packaging/web/shell.html` - wire DOM clipboard events

Add document-level `copy`, `cut`, and `paste` listeners near the existing
`Module.ccall('glr_web_*', ...)` button wiring. Gate them on
`sceneControlsReady` and canvas focus so browser clipboard handling outside the
canvas remains normal.

Use `text/plain` for interoperability and a small custom MIME type to preserve
the gl-repl clipboard kind when the clipboard round-trips back into this app:

```js
var glrClipboardKindMime = 'application/x-gl-repl-clipboard-kind';

document.addEventListener('copy', (event) => {
  if (!sceneControlsReady || document.activeElement !== canvasElement) return;
  var ok = Module.ccall('glr_web_clipboard_copy', 'number', [], []);
  if (!ok) return;
  var text = Module.ccall('glr_web_clipboard_text', 'string', [], []);
  var kind = Module.ccall('glr_web_clipboard_kind', 'string', [], []);
  event.clipboardData.setData('text/plain', text);
  event.clipboardData.setData(glrClipboardKindMime, kind || 'lines');
  event.preventDefault();
});

document.addEventListener('cut', (event) => {
  if (!sceneControlsReady || document.activeElement !== canvasElement) return;
  var ok = Module.ccall('glr_web_clipboard_cut', 'number', [], []);
  if (!ok) return;
  var text = Module.ccall('glr_web_clipboard_text', 'string', [], []);
  var kind = Module.ccall('glr_web_clipboard_kind', 'string', [], []);
  event.clipboardData.setData('text/plain', text);
  event.clipboardData.setData(glrClipboardKindMime, kind || 'lines');
  event.preventDefault();
});

document.addEventListener('paste', (event) => {
  if (!sceneControlsReady || document.activeElement !== canvasElement) return;
  var data = event.clipboardData || window.clipboardData;
  if (!data) return;
  var text = data.getData('text/plain');
  if (!text) return;
  var kind = data.getData(glrClipboardKindMime);
  var ok = Module.ccall('glr_web_clipboard_paste_text', 'number',
                        ['string', 'string'], [text, kind || '']);
  if (ok) event.preventDefault();
});
```

The DOM event, not the old GLUT Ctrl+C/X/V route, owns web keyboard clipboard
shortcuts. The editor clipboard is populated inside the bridge only long enough
to reuse the existing paste/cut/copy machinery.

### 5. Makefile - export the new bridge symbols

Append the new symbols to the WEB=1 `-sEXPORTED_FUNCTIONS` list:

```
_glr_web_clipboard_copy
_glr_web_clipboard_cut
_glr_web_clipboard_text
_glr_web_clipboard_kind
_glr_web_clipboard_paste_text
```

Keep `EMSCRIPTEN_KEEPALIVE` on the C exports as belt-and-suspenders, matching
the existing `glr_web_new_scene`, `glr_web_load_scene_text`, and
`glr_web_export_scene` convention.

### 6. Docs

Add a bullet to `packaging/web/README.md`'s browser input shim section
describing:

- plain Ctrl/Cmd+C/X/V bypass JS GLUT and are handled by DOM clipboard events
- Ctrl+Shift+C/V still flow through GLUT as app shortcuts
- `text/plain` interoperates with other apps, while the custom
  `application/x-gl-repl-clipboard-kind` value preserves line-vs-input behavior
  for gl-repl round-trips when the browser keeps it

## Out of scope

Pointer/touch-triggered OS clipboard writes from in-canvas copy/cut chips are a
separate path. Clicking the app's in-canvas copy/cut controls does not fire a
browser `copy`/`cut` event, so supporting OS clipboard writes there would require
an async `navigator.clipboard.writeText()` call during the click activation.
Leave those controls as internal clipboard operations for this change.

## Verification

1. Native/editor tests:
   - Add or update editor tests for `editor_clipboard_copy_current_with_result()`
     and `editor_clipboard_cut_current_with_result()` so success and blocked
     no-op cases are pinned.
   - Include line copy, input-selection copy, insert-mode no-op, and guarded
     var-declaration copy/cut cases where practical.
2. Web build:
   - `make web`
   - `make web-serve`
3. Browser checks via Chrome/CDP:
   - Focus the canvas, seed `navigator.clipboard.writeText()` with a multiline
     GL snippet, dispatch real Ctrl+V, and verify new command lines appear.
   - Copy a selected source line/range with real Ctrl+C, then read
     `navigator.clipboard.readText()` and confirm the expected `text/plain`.
   - Copy an input-buffer selection and paste it back; verify the custom kind
     keeps it as input text, not a source-line paste.
   - Paste external single-line text while not in insert mode; verify it stages
     as one source line.
   - Paste external single-line text while an input selection is active; verify
     it replaces the input selection.
   - Dispatch Ctrl+Shift+C and Ctrl+Shift+V and confirm the existing app
     shortcuts still fire rather than browser clipboard events.
4. `make check-c99`.
