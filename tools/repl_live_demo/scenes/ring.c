// @scene-name Ring
// Snippet start
float n = 7;
float r = 1.4;
glPointSize(6);
glColor3f(0.30, 0.85, 1.00);
glBegin(GL_LINE_LOOP);
for (i = 0; i < n; i++) {
glVertex3f(r * sin(i / n * TAU + t), r * cos(i / n * TAU + t), 0);
}
glEnd();
// Snippet end
