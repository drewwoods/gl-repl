/*
 * ui_app_color_swatch.c - Inline code-panel color swatch renderer.
 *
 * Relocated out of src/ui/subsystems/color_picker.c: it is the only piece
 * that needed the app-owned UiTransformer, so moving it here lets the
 * floating-picker renderer/hit-test stay ui/app-free.
 */
#include "ui/app/color_swatch.h"
#include "gl_includes.h"

void ui_color_picker_render_swatch(const UiTransformer *t,
                                   int sx, int sy,
                                   int active_line) {
    if (!t || t->kind != TRANSFORMER_COLOR_PICKER)
        return;

    int sw = UI_COLOR_SWATCH_W;
    float alpha = t->state.color.has_alpha ? t->state.color.a : 1.0f;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(t->state.color.r, t->state.color.g, t->state.color.b, alpha);
    glRectf((float)sx, (float)sy, (float)sx + (float)sw, (float)sy + (float)sw);

    glColor4f(0.55f, 0.55f, 0.65f, 0.9f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(sx,    sy);
    glVertex2f(sx+sw, sy);
    glVertex2f(sx+sw, sy+sw);
    glVertex2f(sx,    sy+sw);
    glEnd();

    if (active_line == t->line_idx) {
        glColor4f(1.0f, 1.0f, 1.0f, 0.9f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(sx-1,    sy-1);
        glVertex2f(sx+sw+1, sy-1);
        glVertex2f(sx+sw+1, sy+sw+1);
        glVertex2f(sx-1,    sy+sw+1);
        glEnd();
    }
    glDisable(GL_BLEND);
}
