#include <gl_includes.h>

void display() {
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glLoadIdentity();
  gluLookAt(-2.35, 1.71, 4.07,
            -0.00, -0.00, 0.00,
            0.0, 1.0, 0.0);

  glBegin(GL_TRIANGLE_STRIP);
  glNormal3f(-0.5, -0.5, 0.2);
  glVertex3f(-1, -1, -0.5);
  glNormal3f(-0.5, 0.5, 1);
  glVertex3f(-1, 1, -0.5);
  glNormal3f(0, -0.5, 1);
  glVertex3f(0, -1, -0.5);
  glEnd();

  glutSwapBuffers();
}

void reshape(int w, int h) {
  glViewport(0, 0, w, h);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  gluPerspective(45.0, (float)w/(float)h, 0.1, 100.0);
  glMatrixMode(GL_MODELVIEW);
}

void init() {
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glEnable(GL_COLOR_MATERIAL);
}

int main(int argc, char **argv) {
  glutInit(&argc, argv);
  glutInitDisplayMode(GLUT_DOUBLE|GLUT_RGB|GLUT_DEPTH);
  glutInitWindowSize(800, 600);
  glutCreateWindow("OpenGL REPL");
  init();
  glutDisplayFunc(display);
  glutReshapeFunc(reshape);
  glutMainLoop();
  return 0;
}
