/* @workspace: REPL state (auto-saved) */
/* A hand-kept exported-C fixture: the shape no .glr can express.
 * `spin` is an exported-C projection detail, and every comment here is a
 * C89 block comment - the form the reader would silently drop if it only
 * understood `//`, which would be this plan's own bug with new syntax. */
#include <GL/gl.h>
#include <GL/glut.h>

static float g_angle = 0.0f;
static int   g_rotating = 0;

void display(void)
{
  glLoadIdentity();
  /* camera */
  glTranslatef(0.0000f, 0.0000f, -12.0000f);   /* @camera dist */
  glRotatef(18.0000f, 1.0f, 0.0f, 0.0f);   /* @camera rx */
  glRotatef(42.0000f, 0.0f, 1.0f, 0.0f);   /* @camera ry */
  glRotatef(g_angle, 0.0f, 1.0f, 0.0f);   /* @camera spin */
  glTranslatef(-0.5000f, 1.5000f, -2.5000f);   /* @camera pan */
  /* Snippet start */
  glBegin(GL_POINTS);
  glVertex3f(0, 0, 0);
  glEnd();
  /* Snippet end */
}
