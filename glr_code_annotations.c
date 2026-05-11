/*
 * glr_code_annotations.c -- Full-app adapter for the neutral code-
 * annotations port.
 *
 * Bridges code_annotations.h (called by repl_replay_annotations.c)
 * to the EditorVirtualLineList owned by src/editor/state.c. The
 * standalone demo links a different implementation (or none at all,
 * since the demo has no UI to render annotations to). Phase 4 of
 * feature/source-document-port.md.
 */

#include "code_annotations.h"

#include "editor/state.h"
#include "ui/editor.h"

void code_annotations_clear(void) {
    editor_state_virtual_lines_clear();
}

int code_annotations_append(int after_line_idx,
                            CodeAnnotationStyle style,
                            const char *text,
                            const char *aux) {
    VirtualLineStyle vstyle;
    switch (style) {
    case CODE_ANNOTATION_STYLE_REPLAY_SUBST:
        vstyle = VIRTUAL_STYLE_REPLAY_SUBST;
        break;
    case CODE_ANNOTATION_STYLE_REPLAY_EVAL:
        vstyle = VIRTUAL_STYLE_REPLAY_EVAL;
        break;
    default:
        return 0;
    }
    return editor_state_virtual_lines_append(after_line_idx, vstyle,
                                             text, aux);
}
