#ifndef GLR_CTRL_REPLAY_ANNOTATIONS_H
#define GLR_CTRL_REPLAY_ANNOTATIONS_H

#include "repl/replay_annotations.h"

/* Publish a prepared replay-annotation list to the editor's virtual-line
 * storage. The full app uses this bridge so the REPL annotation builder
 * stays free of editor/UI dependencies. */
void glr_publish_replay_annotations(const ReplReplayAnnotationOutput *out);

#endif /* GLR_CTRL_REPLAY_ANNOTATIONS_H */
