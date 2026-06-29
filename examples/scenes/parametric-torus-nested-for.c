// @cfg vertex_labels = 0
// camera
glTranslatef(0.0f, 0.0f, -8.0f);
glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
glRotatef(35.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, 0.0f, 0.0f);

static float R = 2;  // major radius (ring center to tube center)
static float r = 1;  // minor radius (tube thickness)
static float n = 24; // segment count around both u and v
static float u, v;   // current major/minor angles
glClearColor(0.1, 0.1, 0.1, 1.0);
glEnable(GL_DEPTH_TEST);
//glEnable(GL_LIGHTING);
glEnable(GL_NORMALIZE);
glEnable(GL_LIGHT3);
glEnable(GL_LIGHT2);
glShadeModel(GL_SMOOTH);
n = floor(n); // keep an an integer
n = min(25, max(n, 3)); // clamp to reasonable range
for(i, 0, n) {
glBegin(GL_QUAD_STRIP);
for(j, 0, n+1) {
// step v across the tube; emit two vertices, one at u=i and one at u=i+1
v = j*TAU/n;
u = i*TAU/n;
glColor3f(sin(u)*0.4+0.6, cos(v)*0.4+0.6, 0.5);
glVertex3f((R + r*cos(v))*cos(u), (R + r*cos(v))*sin(u), r*sin(v));
u = (i+1)*TAU/n;
glVertex3f((R + r*cos(v))*cos(u), (R + r*cos(v))*sin(u), r*sin(v));
}
glEnd();
}
