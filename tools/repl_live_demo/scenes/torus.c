// @scene-name Torus
  glTranslatef(0.0000f, 0.0000f, -5.0000f);   // @camera dist
  glRotatef(25.0000f, 1.0f, 0.0f, 0.0f);   // @camera rx
  glRotatef(30.0000f, 0.0f, 1.0f, 0.0f);   // @camera ry
  glTranslatef(0.0000f, 0.0000f, 0.0000f);   // @camera pan
// Snippet start
float inner = 0.25;
float outer = 0.70;
glEnable(GL_LIGHT0);
glEnable(GL_LIGHTING);
glColor3f(0.90, 0.70, 0.30);
glutSolidTorus(inner, outer, 24, 40);
// Snippet end
