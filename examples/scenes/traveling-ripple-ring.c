// @cfg view_mode = RENDER3D_VIEW_2D
// @cfg fit_frame = 1.15
// @cfg vertex_outlines = 0
// @cfg vertex_points = 0
// camera
glTranslatef(0.0f, 0.0f, -3.5f);
glRotatef(0.0f, 1.0f, 0.0f, 0.0f);
glRotatef(0.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, 0.0f, 0.0f);
// Traveling ripple ring: nested for + fmod + conditional deformation
static float phase, width, delta, amp, x, y;
glClearColor(0.05, 0.06, 0.08, 1.0);
amp = 0.1 * sin(t * TAU);
phase = fmod(t / 3, TAU);
width = PI / 8;
glBegin(GL_LINE_LOOP);
  for(i, 0, 400) {
    glColor3f(0.36, 0.70, 0.98);
    x = cos(TAU * i / 400);
    y = sin(TAU * i / 400);
    delta = fmod(TAU * i / 400 - phase + TAU, TAU);
    if(delta < width) {
      glColor3f(0.98, 0.76, 0.36);
      x = x + amp * x * sin(delta * TAU / width);
      y = y + amp * y * sin(delta * TAU / width);
    }
    glVertex3f(x, y, 0);
  }
glEnd();
