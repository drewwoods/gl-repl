// @cfg view_mode = RENDER3D_VIEW_2D
// camera
glTranslatef(0.0f, 0.0f, -2.5f);
glRotatef(0.0f, 1.0f, 0.0f, 0.0f);
glRotatef(0.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, 0.0f, 0.0f);
static float x, y;
glClearColor(0.05, 0.06, 0.08, 1.0);
// 2D assignment sketch: tests runtime variable assignment without goto
glDisable(GL_LIGHTING);
x = -1.7;
glBegin(GL_LINE_STRIP);
for(i, 0, 48) {
  x = x + 0.07;
  y = sin(x*2.5 + t)*0.52 + cos(i*0.25 + t*0.6)*0.16;
  glColor3f(0.98 - 0.62*(i/47.0), 0.46 + 0.24*(i/47.0), 0.36 + 0.62*(i/47.0));
  glVertex3f(x, y, 0);
}
glEnd();
