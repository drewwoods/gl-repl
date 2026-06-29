#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>   /* gl4es's header (its -I precedes); maps gl* -> gl4es_gl* */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
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
#endif

    initialize_gl4es();

    printf("[gl4es_bootstrap] Done.\n");
}
