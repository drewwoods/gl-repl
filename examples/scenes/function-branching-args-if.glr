// @cfg view_mode = RENDER3D_VIEW_2D
// camera
glTranslatef(0.0f, 0.0f, -4.0f);
glRotatef(0.0f, 1.0f, 0.0f, 0.0f);
glRotatef(0.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, 0.0f, 0.0f);
glClearColor(0.05, 0.06, 0.08, 1.0);
glEnable(GL_DEPTH_TEST);
glEnable(GL_NORMALIZE);
shape(scale, phase) {
if(scale > 1) {
glColor3f(0.98, 0.46, 0.36);
glBegin(GL_QUADS);
glNormal3f(0, 0, 1);
glVertex3f(-0.7*scale, -0.7*scale, 0);
glVertex3f(0.7*scale, -0.7*scale, 0);
glVertex3f(0.7*scale, 0.7*scale, 0);
glVertex3f(-0.7*scale, 0.7*scale, 0);
glEnd();
}
if(scale <= 1) {
glColor3f(0.36, 0.70, 0.98);
glBegin(GL_TRIANGLES);
glNormal3f(0, 0, 1);
glVertex3f(cos(phase)*0.9*scale, sin(phase)*0.9*scale, 0);
glVertex3f(cos(phase+TAU/3)*0.9*scale, sin(phase+TAU/3)*0.9*scale, 0);
glVertex3f(cos(phase+2*TAU/3)*0.9*scale, sin(phase+2*TAU/3)*0.9*scale, 0);
glEnd();
}
}
glPushMatrix();
glTranslatef(-2.2, 0, 0);
shape(0.9, t);
glPopMatrix();
glPushMatrix();
shape(1.4, t*0.5);
glPopMatrix();
glPushMatrix();
glTranslatef(2.2, 0, 0);
shape(0.65, -t*0.8);
glPopMatrix();
