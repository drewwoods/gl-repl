/*
 * glr_url.c - Cross-platform URL opener.
 */
#include "app/glr_url.h"
#include "repl/host_effects.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
void glr_url_tick(void) {}
void glr_url_shutdown(void) {}
#elif defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
void glr_url_tick(void) {}
void glr_url_shutdown(void) {}
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__) || defined(__posix)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static pid_t *s_url_pids = NULL;
static int    s_url_pid_count = 0;
static int    s_url_pid_cap = 0;

/* Non-blocking reap of previous URL launcher child processes only */
void glr_url_tick(void) {
    if (s_url_pid_count == 0)
        return;
    int write_idx = 0;
    for (int i = 0; i < s_url_pid_count; i++) {
        pid_t pid = s_url_pids[i];
        if (pid <= 0)
            continue;
        int status = 0;
        pid_t res;
        do {
            res = waitpid(pid, &status, WNOHANG);
        } while (res < 0 && errno == EINTR);

        if (res == 0) {
            /* Child is still active; retain for future ticks */
            s_url_pids[write_idx++] = pid;
        } else if (res < 0 && errno == EINTR) {
            /* Interrupted; retain */
            s_url_pids[write_idx++] = pid;
        }
        /* If res == pid (reaped) or res < 0 with ECHILD (already reaped), drop */
    }
    s_url_pid_count = write_idx;
}

static void glr_url_track_child(pid_t pid) {
    if (pid <= 0)
        return;
    glr_url_tick();
    if (s_url_pid_count >= s_url_pid_cap) {
        int new_cap = s_url_pid_cap == 0 ? 8 : s_url_pid_cap * 2;
        pid_t *new_pids = (pid_t *)realloc(s_url_pids, (size_t)new_cap * sizeof(pid_t));
        if (!new_pids)
            return;
        s_url_pids = new_pids;
        s_url_pid_cap = new_cap;
    }
    s_url_pids[s_url_pid_count++] = pid;
}

void glr_url_shutdown(void) {
    glr_url_tick();
    if (s_url_pids) {
        free(s_url_pids);
        s_url_pids = NULL;
    }
    s_url_pid_count = 0;
    s_url_pid_cap = 0;
}
#else
void glr_url_tick(void) {}
void glr_url_shutdown(void) {}
#endif

static GlrUrlLauncherFn g_test_launcher = NULL;

void glr_url_set_launcher_for_test(GlrUrlLauncherFn fn) {
    g_test_launcher = fn;
}

void glr_url_reset_launcher_for_test(void) {
    g_test_launcher = NULL;
}

int glr_url_open(const char *url) {
    if (!url || !url[0])
        return 0;

    if (g_test_launcher)
        return g_test_launcher(url);

#if defined(__EMSCRIPTEN__)
    int opened = EM_ASM_INT({
        try {
            var w = window.open(UTF8ToString($0), '_blank');
            return w ? 1 : 0;
        } catch (e) {
            return 0;
        }
    }, url);
    return opened;
#elif defined(_WIN32)
    HINSTANCE res = ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    return (INT_PTR)res > 32;
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__) || defined(__posix)
    glr_url_tick();

    posix_spawn_file_actions_t actions;
    int actions_inited = 0;
    posix_spawnattr_t attr;
    int attr_inited = 0;
    int ok = 0;

    if (posix_spawn_file_actions_init(&actions) == 0) {
        actions_inited = 1;
        if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0) {
            if (posix_spawnattr_init(&attr) == 0) {
                attr_inited = 1;
                sigset_t sigdefault;
                sigemptyset(&sigdefault);
                sigaddset(&sigdefault, SIGCHLD);
                if (posix_spawnattr_setsigdefault(&attr, &sigdefault) == 0 &&
                    posix_spawnattr_setpgroup(&attr, 0) == 0 &&
                    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF) == 0) {
#if defined(__APPLE__)
                    const char *launcher = "open";
#else
                    const char *launcher = "xdg-open";
#endif
                    char *const argv[] = { (char *)launcher, (char *)url, NULL };
                    pid_t pid = 0;
                    int rc = posix_spawnp(&pid, launcher, &actions, &attr, argv, environ);
                    if (rc == 0) {
                        glr_url_track_child(pid);
                        ok = 1;
                    }
                }
            }
        }
    }

    if (attr_inited)
        posix_spawnattr_destroy(&attr);
    if (actions_inited)
        posix_spawn_file_actions_destroy(&actions);

    return ok;
#else
    return 0;
#endif
}

int glr_url_open_user_guide(void) {
    if (glr_url_open(GLR_USER_GUIDE_URL)) {
        repl_set_status("Requested browser open for User Guide");
        return 1;
    }
    repl_set_status_error("Could not launch browser");
    return 0;
}
