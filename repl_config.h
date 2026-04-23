#ifndef REPL_CONFIG_H
#define REPL_CONFIG_H

#include "sample.h"

/* Config rows are still backed by the legacy pointer table for now, but the
 * descriptor type has been split out of sample.h so the eventual ReplConfigKey
 * migration has a dedicated home. */
typedef enum {
    REPL_CONFIG_NONE = 0,
    REPL_CONFIG_RENDERING,
    REPL_CONFIG_TIME_REPLAY,
    REPL_CONFIG_OVERLAYS_SCENE,
    REPL_CONFIG_GEOMETRY,
    REPL_CONFIG_INTERFACE,
    REPL_CONFIG_AUDIO,
    REPL_CONFIG_COUNT
} ReplConfigKey;

typedef struct {
    const char *label;
    int         key_code;    /* ASCII ctrl key or GLUT_KEY_* */
    int         is_special;   /* 1 for GLUT special keys */
    int        *value;
    int         n_states;     /* 2 = toggle; >2 = cycle */
    const char **state_names; /* optional state labels */
    ReplConfigKey key;        /* future key-based descriptor hook */
} CfgItem;

#endif /* REPL_CONFIG_H */
