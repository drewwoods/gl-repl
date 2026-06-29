// @cfg poly_highlight = 0
// @cfg vertex_labels = 0
// @cfg vertex_outlines = 0
// @cfg vertex_points = 0
// camera
glTranslatef(0.0f, 0.0f, -9.0f);
glRotatef(25.0f, 1.0f, 0.0f, 0.0f);
glRotatef(0.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, 0.0f, 0.0f);
static float n, p, q, ang, rr, x, y, z; // samples, winds, angle, ring radius, coords
glClearColor(0.05, 0.05, 0.08, 1.0);
glLineWidth(2.0);
n = 400;   // samples around the closed curve
p = 2;     // turns around the torus axis
q = 3;     // turns through the torus hole
glBegin(GL_LINE_LOOP);
  for(i, 0, n) {
    ang = TAU * i/n;
    rr = 2.0 + cos(q*ang);          // distance from the axis
    x = rr * cos(p*ang);
    y = rr * sin(p*ang);
    z = sin(q*ang);
    glColor3f(0.5 + 0.5*sin(ang + t), 0.5 + 0.5*sin(ang + t + 2.0), 0.5 + 0.5*sin(ang + t + 4.0));
    glVertex3f(x, y, z);
  }
glEnd();
