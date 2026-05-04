#ifndef CONFIG_H
#define CONFIG_H

/* Configure shared by unrelated subsystems */

/* Max brightness (V in HSV) allowed for glClearColor channels.
 * Since max(r,g,b) == V, capping V caps all channels. */
#define CP_CLEAR_MAX_V 0.1f

#endif /* CONFIG_H */