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
                if (event['keyCode'] === 8) return 8;
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
                focusCanvas();
                event.preventDefault();
                if (event.deltaY === 0) return;
                var p = canvasCoords(canvas, event);
                callWheel(event.deltaY < 0 ? 1 : -1, p[0], p[1]);
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
