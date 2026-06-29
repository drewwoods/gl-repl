// @cfg view_mode = RENDER3D_VIEW_2D
// camera
glTranslatef(0.0f, 0.0f, -3.0f);
glRotatef(0.0f, 1.0f, 0.0f, 0.0f);
glRotatef(0.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, 0.0f, 0.0f);
// @cfg vertex_points = 0
glClearColor(0.05, 0.06, 0.08, 1.0);
glEnable(GL_DEPTH_TEST);
triangle() {
glBegin(GL_TRIANGLES);
glNormal3f(0, 0, 1);
glVertex3f(0, 0.8, 0);
glVertex3f(-0.7, -0.4, 0);
glVertex3f(0.7, -0.4, 0);
glEnd();
}
glColor3f(0.98, 0.46, 0.36);
triangle();
glTranslatef(2, 0, 0);
glColor3f(0.30, 0.84, 0.80);
triangle();
glTranslatef(-4, 0, 0);
glColor3f(0.62, 0.52, 0.95);
triangle();
