// @cfg view_mode = RENDER3D_VIEW_2D
// @cfg vertex_outlines = 0
// @cfg vertex_points = 0
// camera
glTranslatef(0.0f, 0.0f, -6.0f);
glRotatef(0.0f, 1.0f, 0.0f, 0.0f);
glRotatef(0.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, 0.0f, 0.0f);

glClearColor(0.05, 0.06, 0.08, 1.0);
glEnable(GL_DEPTH_TEST);
glEnable(GL_NORMALIZE);
// Recursive triangle tree: child calls shrink and rotate from the parent
branch(depth, size, spin) {
glColor3f(0.62 - 0.065*depth, 0.52 + 0.045*depth, 0.95 + 0.0075*depth);
glBegin(GL_LINE_LOOP);
glNormal3f(0, 0, 1);
glVertex3f(0, size, 0);
glVertex3f(-0.866*size, -0.5*size, 0);
glVertex3f(0.866*size, -0.5*size, 0);
glEnd();
if(depth <= 0) {
glColor3f(0.98, 0.76, 0.36);
glBegin(GL_TRIANGLES);
glNormal3f(0, 0, 1);
glVertex3f(0, size*0.55, 0);
glVertex3f(-0.48*size, -0.25*size, 0);
glVertex3f(0.48*size, -0.25*size, 0);
glEnd();
}
if(depth > 0) {
glPushMatrix();
glTranslatef(0, size*1.02, 0);
glRotatef(18 + spin*14, 0, 0, 1);
branch(depth - 1, size*0.62, spin + 0.15);
glPopMatrix();
glPushMatrix();
glTranslatef(-size*0.9, -size*0.38, 0);
glRotatef(-30 - spin*12, 0, 0, 1);
branch(depth - 1, size*0.58, spin + 0.12);
glPopMatrix();
glPushMatrix();
glTranslatef(size*0.9, -size*0.38, 0);
glRotatef(30 + spin*12, 0, 0, 1);
branch(depth - 1, size*0.58, spin + 0.12);
glPopMatrix();
}
}
glRotatef(sin(t*0.35)*12, 0, 0, 1);
branch(4, 1.05, sin(t*0.4));
