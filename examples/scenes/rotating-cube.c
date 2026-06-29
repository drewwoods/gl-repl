// @cfg axes = AXES_THEME_OFF
// @cfg light_indicators = 1
// @cfg grid = GRID_THEME_CLASSIC
// @cfg light_theme = LIGHT_THEME_NEON
// @cfg grid_brightness = GRID_BRIGHTNESS_DIM
// camera
glTranslatef(0.0000f, 0.0000f, -9.1513f);
glRotatef(12.0069f, 1.0f, 0.0f, 0.0f);
glRotatef(92.0173f, 0.0f, 1.0f, 0.0f);
glTranslatef(-0.0000f, -0.0000f, -0.0000f);

glClearColor(0.1, 0.1, 0.1, 1);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT3);
glEnable(GL_LIGHT2);
glEnable(GL_LIGHT1);
glEnable(GL_LIGHT0);

glRotatef(40*t, 0, 1, 0);
glutSolidCube(2);
