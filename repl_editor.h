#ifndef REPL_EDITOR_H
#define REPL_EDITOR_H

/* Editor state, input dispatch, and GLUT funnel helpers.
 *
 * The only *_func() wrappers forwarded from sample.c.
 * GLUT input/feedback calls (glutGetModifiers, glutPostRedisplay, glutSetCursor)
 * are centralized here and funneled through named static helpers to establish
 * a clear review boundary for GLUT API usage.
 */

int repl_editor_active_modifiers(void);

#endif
