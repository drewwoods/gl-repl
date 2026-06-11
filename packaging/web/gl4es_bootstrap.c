#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>   /* gl4es's header (its -I precedes); maps gl* -> gl4es_gl* */

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

    initialize_gl4es();

    printf("[gl4es_bootstrap] Done.\n");
}
