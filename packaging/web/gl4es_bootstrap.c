#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

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
