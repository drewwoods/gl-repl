// @scene-name Torus
// camera
  glTranslatef(0.0000f, 0.0000f, -5.0000f);
  glRotatef(25.0000f, 1.0f, 0.0f, 0.0f);
  glRotatef(30.0000f, 0.0f, 1.0f, 0.0f);
  glTranslatef(0.0000f, 0.0000f, 0.0000f);
// Snippet start
float inner = 0.25;
float outer = 0.70;
glEnable(GL_LIGHT0);
glEnable(GL_LIGHTING);
glColor3f(0.90, 0.70, 0.30);
glutSolidTorus(inner, outer, 24, 40);
// Snippet end
