#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>   /* gl4es's header (its -I precedes); maps gl* -> gl4es_gl* */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __EMSCRIPTEN__
/* gl-repl provides this. Keep a weak no-op so the bootstrap remains reusable
 * for smaller one-file samples that do not link the REPL controller. */
__attribute__((weak)) void glr_ctrl_mousewheel(int wheel, int direction, int x, int y) {
    (void)wheel;
    (void)direction;
    (void)x;
    (void)y;
}
#endif

/* Emscripten's built-in JS GLUT (library_glut.js) supplies the windowing/
 * event layer (the patched freeglut renames its own windowing to fg_glut*
 * so the JS implementation wins), but it does not implement
 * glutExtensionSupported. Newer samples (gl-repl's runtime GL capability
 * detection) call it, so provide the standard token-scan over the live GL
 * extension string here. glGetString resolves to gl4es via the forced
 * gl4es <GL/gl.h> include every TU is compiled with. */
int glutExtensionSupported(const char *extension) {
    const char *exts, *start;
    size_t len;

    if (!extension || !*extension || strchr(extension, ' ')) return 0;
    exts = (const char *)glGetString(GL_EXTENSIONS);
    if (!exts) return 0;

    len = strlen(extension);
    for (start = exts; (start = strstr(start, extension)) != NULL; start += len) {
        if ((start == exts || start[-1] == ' ') &&
            (start[len] == ' ' || start[len] == '\0')) {
            return 1;
        }
    }
    return 0;
}

// Forward declaration of a initialization function
// This can be optionally defined in the sample code.
__attribute__((weak)) void initialize_gl4es(void) {
    // Default empty implementation
}

// This attribute tells the linker to execute this function
// before main() is called.
__attribute__((constructor)) void gl4es_bootstrap(void) {
    printf("[gl4es_bootstrap] Initializing gl4es configuration...\n");

    // --- GL4ES Configuration ---
    // These behave just like the environment variables you might set in a shell.

    // 1. Enable Non-Power-Of-Two texture support (Crucial for UI/Fonts)
    setenv("LIBGL_NPOT", "1", 1);

    // 2. Force ES 2.0 backend (since we are compiling for WebGL 2)
    setenv("LIBGL_ES", "2", 1);

    // 3. (Optional) Show gl4es debug info in the browser console
    setenv("LIBGL_DEBUG", "1", 1);

#ifdef __EMSCRIPTEN__
    /* Tag the WebGL drawing buffer Display-P3 so the web build's colors are as
     * vibrant as the native build on wide-gamut displays. JS GLUT creates the
     * context lazily inside glutCreateWindow (after this ctor runs), and tagging
     * it mid-creation (wrapping getContext) proved unreliable — GLUT's initial
     * reshape resizes the drawing buffer right after. So defer until the live
     * context is current (Module.ctx, set by GL.makeContextCurrent) and tag it
     * then, matching the timing that worked when called from the app. No-op on
     * sRGB panels / browsers lacking the property. */
    EM_ASM({
        function tagP3() {
            var gl = Module['ctx'];
            if (!gl || !('drawingBufferColorSpace' in gl)) return false;
            try { gl.drawingBufferColorSpace = 'display-p3'; } catch (e) { return false; }
            console.log('[gl4es_bootstrap] drawingBufferColorSpace = display-p3');
            return true;
        }
        if (!tagP3()) {
            var tries = 0;
            var iv = setInterval(function() {
                if (tagP3() || ++tries > 200) clearInterval(iv); /* ~10s cap */
            }, 50);
        }
    });

    /* Browser input defaults are hostile to a GLUT-style canvas: wheel scrolls
     * the page, middle-click can start browser autoscroll, right-click opens a
     * menu, and keys like Space/Backspace/arrows/F-keys/Ctrl+S go to the
     * browser unless the canvas owns focus and cancels the default action.
     * Emscripten's GLUT layer still receives the events because these handlers
     * only call preventDefault(); they do not stop propagation. The explicit
     * wheel bridge covers runtimes that do not implement freeglut's
     * glutMouseWheelFunc extension. */
    EM_ASM({
        var wheelCallback = $0;

        /* Wheel damping, mirroring the native freeglut Cocoa backend
         * (third_party/freeglut/src/cocoa/fg_window_cocoa.m, scrollWheel:).
         *
         * glr_ctrl_mousewheel is magnitude-blind: route_wheel applies one
         * fixed unit (GLR_WHEEL_ZOOM_STEP, or one code-panel line) per call,
         * so sensitivity is purely the callback rate and all damping must
         * happen here. Cocoa scales precise (trackpad / Magic Mouse) deltas
         * by 0.1, accumulates, and emits one callback per whole threshold
         * unit -- which is why a macOS trackpad gesture, ~60-120 small pixel
         * events plus a momentum tail, does not fly past the scene natively.
         *
         * The DOM has no hasPreciseScrollingDeltas equivalent, and it does
         * NOT preserve Cocoa's units: Blink reports a real wheel notch on
         * macOS as deltaMode 0 / deltaY ~100, the same pixel scale as the
         * trackpad's ~4, where Cocoa would report a non-precise +-1. So the
         * precise/discrete split has to be inferred per event -- gating on
         * navigator.platform alone would scale a genuine notch to 10 ticks. */
        var GLR_WHEEL_NOTCH = 1.0;      /* accumulated units per callback */
        var GLR_WHEEL_PRECISE_SCALE = 0.1;  /* == Cocoa's precise-delta scale */
        var GLR_WHEEL_LINES_PER_NOTCH = 3.0;
        var GLR_WHEEL_NOTCH_PAGES = 3.0;    /* deltaMode 2 is unbounded; cap it */
        var GLR_WHEEL_MAX_TICKS = 8;    /* per event; Cocoa's while() is unbounded */
        var glrWheelAccum = 0.0;

        /* Apple platforms are the ones whose native backend damps at all:
         * X11/Win32 freeglut receive discrete notches (button 4/5 presses)
         * with no accumulator, so matching native there means one tick per
         * notch -- exactly what the undamped path already did. Keep the
         * fallback conservative: unknown platform => previous behavior.
         * userAgentData.platform is the non-deprecated spelling; navigator
         * .platform still works everywhere and iPadOS reports "MacIntel",
         * which is fine (touch scroll wants the same momentum damping). */
        var glrWheelDampen = (function() {
            var p = "";
            try {
                if (navigator.userAgentData && navigator.userAgentData.platform)
                    p = navigator.userAgentData.platform;
            } catch (e) {}
            if (!p) p = navigator.platform || navigator.userAgent || "";
            p = p.toLowerCase();
            return p.indexOf("mac") >= 0 || p.indexOf("iphone") >= 0 ||
                   p.indexOf("ipad") >= 0;
        })();

        /* Signed tick count for one event, or null if it carries no whole
         * notch and must go through the accumulator. */
        function glrWheelDiscreteTicks(event) {
            var mode = event.deltaMode;
            var wd;
            /* Line / page mode is only ever produced by a discrete wheel
             * (Firefox reports +-3 lines per notch on a real wheel; its
             * trackpad path uses pixel mode). */
            if (mode === 1)
                return event.deltaY / GLR_WHEEL_LINES_PER_NOTCH;
            if (mode === 2)
                return event.deltaY * GLR_WHEEL_NOTCH_PAGES;
            /* Pixel mode: Blink/WebKit normalize a real wheel notch to a
             * wheelDeltaY that is an exact multiple of 120, while trackpad
             * events land on arbitrary (often fractional) values. A fast
             * trackpad flick can hit deltaY 40 -> wheelDeltaY 120 and be
             * misread as one notch, which under-scrolls that single event
             * rather than over-scrolling -- the safe direction. */
            wd = event.wheelDeltaY;
            if (typeof wd === 'number' && wd !== 0 && wd % 120 === 0)
                return -wd / 120;
            return null;
        }

        /* Plain (no Shift/Alt) Ctrl/Cmd+C/X/V: the browser/OS clipboard owns
         * these on web, not JS GLUT's keyboardFunc route (see below and the
         * getASCIIKey override) -- Ctrl+Shift+C (reset camera) and
         * Ctrl+Shift+V (view mode toggle) are NOT clipboard shortcuts and
         * must keep flowing to GLUT unchanged, hence the shiftKey bail-out. */
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

        /* Emscripten's own library_glut.js registers a handler on window for the
         * legacy 'mousewheel' / 'DOMMouseScroll' events (GLUT.onMouseWheel) that
         * synthesizes a GLUT mouse button 3/4 GLUT_DOWN with no matching UP. That
         * collides with the clean canvas 'wheel' bridge below: the synthetic
         * button press leaves a pointer button "stuck" and resets the camera zoom
         * velocity the bridge just added, so 3D-scene zoom never happens (the
         * code-panel scroll path, which ignores button state, still works — hence
         * wheel appears to work there but not in the scene).
         *
         * Neutralize GLUT's handler at the source by stubbing it out. GLUT reads
         * GLUT.onMouseWheel only when it registers the listener, inside glutInit
         * (called from main, after this ctor), so overwriting it now means the
         * stub is what gets registered. This touches no event propagation, so the
         * bridge's 'wheel' listener is completely unaffected. */
        if (typeof GLUT !== 'undefined') {
            GLUT.onMouseWheel = function() {};

            /* Backspace fix: emscripten's GLUT maps keyCode 8 to a non-standard
             * "special" key (120) routed to glutSpecialFunc, instead of
             * delivering ASCII 8 to glutKeyboardFunc the way real GLUT/freeglut
             * does. gl-repl's editor expects KEY_BACKSPACE (ASCII 8) on the
             * keyboard callback, and nothing handles special-120, so backspace is
             * dead. Re-route at the source: drop backspace from the special-key
             * map so onKeydown falls through to the ASCII path, and emit 8 there
             * (keeping modifiers, unlike stock getASCIIKey which returns null
             * under ctrl/alt/meta — gl-repl reads modifiers separately). Forward
             * delete (keyCode 46 -> 111 GLUT_KEY_DELETE) is left alone; gl-repl
             * already handles it. */
            var glrOrigSpecialKey = GLUT.getSpecialKey;
            GLUT.getSpecialKey = function(keycode) {
                if (keycode === 8) return null;
                return glrOrigSpecialKey(keycode);
            };
            var glrOrigAsciiKey = GLUT.getASCIIKey;
            GLUT.getASCIIKey = function(event) {
                var kc = event['keyCode'];
                var key = event['key'] || "";
                var code = event['code'] || "";
                if (kc === 8) return 8;
                /* Browser/OS clipboard is authoritative for plain Ctrl/Cmd+C/X/V
                 * on web (see packaging/web/shell.html's copy/cut/paste
                 * listeners). Returning null here means onKeydown's
                 * `key !== null && GLUT.keyboardFunc` check is false, so it
                 * calls neither keyboardFunc (gl-repl's own Ctrl+C/X/V path,
                 * which would otherwise also fire) nor preventDefault() --
                 * leaving the key's default action live, which is what makes
                 * the browser synthesize the native copy/cut/paste event. */
                if (glrIsPlainClipboardCombo(event)) return null;
                /* Emscripten's stock punctuation table omits the backquote
                 * key entirely (DOM keyCode 192), so gl-repl never receives
                 * the ASCII '`' bound to the variable-panel toggle. Accept
                 * both the modern code and legacy keyCode forms; preserve
                 * Shift+Backquote as '~' for normal editor input. */
                if (!event['ctrlKey'] && !event['altKey'] && !event['metaKey'] &&
                    (code === "Backquote" || kc === 192 ||
                     key === "`" || key === "~")) {
                    return event['shiftKey'] ? 126 : 96;
                }
                /* Emscripten's stock GLUT returns raw digit keyCodes for
                 * Shift+0..9 (`1` instead of `!`, etc.; its source has a TODO
                 * for this case). Match the US keyboard symbols that native
                 * GLUT receives from the OS for shifted number-row keys. */
                if (event['shiftKey'] && !event['ctrlKey'] &&
                    !event['altKey'] && !event['metaKey'] &&
                    kc >= 48 && kc <= 57) {
                    return ")!@#$%^&*(".charCodeAt(kc - 48);
                }
                /* Ctrl+slash: the editor's comment toggle is native GLUT's
                 * ASCII '/' plus CTRL modifier, not a KEY_CTRL_* control byte.
                 * Stock getASCIIKey drops it for the same "any modifier"
                 * reason as Ctrl+letters. */
                if (event['ctrlKey'] && !event['altKey'] &&
                    !event['metaKey'] &&
                    (key === "/" || (!event['shiftKey'] &&
                                      (code === "Slash" || kc === 191)))) {
                    return 47;
                }
                /* Ctrl+= / Ctrl+- (accum passes step) and their numpad twins:
                 * dropped by the same "any modifier" rule. Native GLUT
                 * delivers the plain ASCII punctuation with CTRL set in
                 * glutGetModifiers, which is what the router's
                 * glr_ctrl_router_handle_accum_samples_key matches. Returning
                 * a key also makes onKeydown call preventDefault(), which
                 * suppresses the browser's default Ctrl+=/- page zoom on
                 * platforms where that binding exists. */
                if (event['ctrlKey'] && !event['altKey'] && !event['metaKey']) {
                    if (code === "Equal" || kc === 187)
                        return event['shiftKey'] ? 43 /* '+' */ : 61 /* '=' */;
                    if (code === "Minus" || kc === 189)
                        return event['shiftKey'] ? 95 /* '_' */ : 45 /* '-' */;
                    if (code === "NumpadAdd" || kc === 107) return 43;
                    if (code === "NumpadSubtract" || kc === 109) return 45;
                }
                /* Ctrl+letter: stock getASCIIKey returns null on ANY modifier,
                 * so every Ctrl shortcut is dropped. freeglut delivers the
                 * control code (Ctrl+A=1 .. Ctrl+Z=26) on the keyboard callback
                 * with CTRL set in glutGetModifiers; gl-repl's keymap is built on
                 * those KEY_CTRL_* bytes. saveModifiers (called by onKeydown)
                 * already records the modifiers, so just emit the control code to
                 * match native. Shift rides along for Ctrl+Shift+<key> bindings.
                 * Alt/Meta combos are left to the stock path (the browser/OS owns
                 * most Cmd shortcuts). */
                if (event['ctrlKey'] && !event['altKey'] && !event['metaKey'] &&
                    kc >= 65 && kc <= 90) {
                    return kc - 64;
                }
                return glrOrigAsciiKey(event);
            };
        }

        function getCanvas() {
            return Module['canvas'] || document.querySelector('canvas');
        }

        function canvasCoords(canvas, event) {
            var rect = canvas.getBoundingClientRect();
            var sx = rect.width ? canvas.width / rect.width : 1;
            var sy = rect.height ? canvas.height / rect.height : 1;
            var p = new Array(2);
            p[0] = Math.max(0, Math.min(canvas.width - 1,
                Math.floor((event.clientX - rect.left) * sx)));
            p[1] = Math.max(0, Math.min(canvas.height - 1,
                Math.floor((event.clientY - rect.top) * sy)));
            return p;
        }

        function isFunctionKey(key) {
            var n;
            if (key.length < 2 || key.length > 3 || key.charAt(0) !== 'F') return false;
            n = Number(key.substring(1));
            return n >= 1 && n <= 24;
        }

        function callWheel(direction, x, y) {
            if (!wheelCallback) return;
            getWasmTableEntry(wheelCallback)(0, direction, x, y);
        }

        function installInputGuards() {
            var canvas = getCanvas();
            if (!canvas) return false;
            if (canvas.__glrInputGuardsInstalled) return true;
            canvas.__glrInputGuardsInstalled = true;
            canvas.tabIndex = canvas.tabIndex >= 0 ? canvas.tabIndex : 0;
            canvas.style.outline = canvas.style.outline || 'none';

            function focusCanvas() {
                try { canvas.focus({ preventScroll: true }); }
                catch (e) { try { canvas.focus(); } catch (ignore) {} }
            }

            function canvasActive() {
                return document.activeElement === canvas;
            }

            function editableTarget(target) {
                if (!target || target === canvas) return false;
                var tag = target.tagName;
                return target.isContentEditable ||
                    tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT';
            }

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

            canvas.addEventListener('mousedown', function(event) {
                focusCanvas();
                if (event.button === 1 || event.button === 2) {
                    event.preventDefault();
                }
            }, true);
            canvas.addEventListener('auxclick', function(event) {
                focusCanvas();
                event.preventDefault();
            }, true);
            canvas.addEventListener('contextmenu', function(event) {
                event.preventDefault();
            }, true);
            canvas.addEventListener('wheel', function(event) {
                var p, ticks, dir, emitted;
                focusCanvas();
                event.preventDefault();
                if (event.deltaY === 0) return;
                p = canvasCoords(canvas, event);

                /* direction 1 = scroll up; the router inverts it again in
                 * glr_ctrl_mousewheel (route_wheel(x, y, -direction)). */
                if (!glrWheelDampen) {
                    callWheel(event.deltaY < 0 ? 1 : -1, p[0], p[1]);
                    return;
                }

                ticks = glrWheelDiscreteTicks(event);
                if (ticks === null)
                    glrWheelAccum += event.deltaY * GLR_WHEEL_PRECISE_SCALE;
                else
                    glrWheelAccum += ticks * GLR_WHEEL_NOTCH;

                /* Sub-threshold remainder stays buffered instead of firing a
                 * full tick -- the whole point of the native accumulator. */
                emitted = 0;
                while (Math.abs(glrWheelAccum) >= GLR_WHEEL_NOTCH &&
                       emitted < GLR_WHEEL_MAX_TICKS) {
                    dir = glrWheelAccum < 0 ? 1 : -1;
                    callWheel(dir, p[0], p[1]);
                    glrWheelAccum += dir * GLR_WHEEL_NOTCH;
                    emitted++;
                }
                if (emitted >= GLR_WHEEL_MAX_TICKS) glrWheelAccum = 0.0;
            }, { capture: true, passive: false });

            document.addEventListener('keydown', function(event) {
                if (shouldCancelKey(event)) event.preventDefault();
            }, true);
            document.addEventListener('keyup', function(event) {
                if (shouldCancelKey(event)) event.preventDefault();
            }, true);

            console.log('[gl4es_bootstrap] installed canvas input guards');
            return true;
        }

        if (!installInputGuards()) {
            var guardTries = 0;
            var guardTimer = setInterval(function() {
                if (installInputGuards() || ++guardTries > 200) clearInterval(guardTimer);
            }, 50);
        }
    }, glr_ctrl_mousewheel);
#endif

    initialize_gl4es();

    printf("[gl4es_bootstrap] Done.\n");
}
