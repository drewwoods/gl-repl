// @cfg vertex_outlines = 0
// @cfg vertex_points = 0
// @cfg light_indicators = 0
// @cfg poly_highlight = 0
// @cfg vertex_labels = 0
// @cfg grid = GRID_THEME_SOIL
// @cfg grid_extent = GRID_EXTENT_CLOSE
// @cfg axes = AXES_THEME_OFF
// @cfg msaa = 1
// @cfg variable_panel = 0
// camera
glTranslatef(0.0000f, 0.0000f, -6.0020f);
glRotatef(5.0000f, 1.0f, 0.0f, 0.0f);
glRotatef(32.6714f, 0.0f, 1.0f, 0.0f);
glTranslatef(-0.0074f, -0.9000f, -0.4837f);

static float bladeCount = 77.05; // @tune
static float field = 3.65; // half-width @tune
static float x, z, h, w, angle, phase, flex;
static float wind, wave, bend, u, cx, cz, halfW, shade;
blade(seed, x0, z0, height, width, ang, ph, stiffness) {
  wind = 0.55 + 0.25*sin(t*0.23);
  wave = sin(t*2.2 + ph + x0*0.35 + z0*0.20) + 0.35*sin(t*4.9 + ph*1.7);
  bend = (0.65 + 0.35*sin(t*0.75))*wave*stiffness*height*0.26;
  glBegin(GL_TRIANGLE_STRIP);
    for(s, 0, 5) {
      u = s/4;
      cx = x0 + 0.08*rand2(seed, 6)*u + cos(wind)*bend*u*u;
      cz = z0 + 0.08*rand2(seed, 7)*u + sin(wind)*bend*u*u;
      halfW = width*(1 - 0.82*u);
      shade = 0.65 + 0.45*u;
      glColor4f((0.08 + 0.07*rand(seed, 8))*shade, (0.38 + 0.32*rand(seed, 9))*shade, (0.06 + 0.07*rand(seed, 10))*shade, 0.95);
      glVertex3f(cx - cos(ang)*halfW, height*u, cz - sin(ang)*halfW);
      glVertex3f(cx + cos(ang)*halfW, height*u, cz + sin(ang)*halfW);
    }
  glEnd();
}
glClearColor(0.025, 0.055, 0.035, 1);
glEnable(GL_DEPTH_TEST);
glDisable(GL_LIGHTING);
glDisable(GL_CULL_FACE);
glEnable(GL_BLEND);
glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
for(p, 0, bladeCount) {
  x = (rand(p, 0)*2 - 1)*field;
  z = (rand(p, 1)*2 - 1)*field;
  h = 0.28 + rand(p, 2)*0.72;
  w = 0.018 + rand(p, 3)*0.026;
  angle = rand(p, 4)*TAU;
  phase = rand(p, 5)*TAU;
  flex = 0.55 + rand(p, 6)*1.1;
  blade(p, x, z, h, w, angle, phase, flex);
}
glDisable(GL_BLEND);
