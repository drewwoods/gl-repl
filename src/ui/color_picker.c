/*
 * src/ui/color_picker.c -- Floating color picker renderer + hit-test.
 *
 * Pure UI layer over the `color_picker_state.h` peer. Reads a
 * frame-shaped `ColorPickerView` for popup geometry/state and a
 * couple of viewport scalars; never mutates peer state, never reads
 * live REPL/editor state, and never calls the parser/commit pipeline.
 *
 * The companion guard `check-color-picker-ui-isolation.sh` enforces
 * that surface — anything mutating must live on the peer
 * (color_picker_state.c) or another module the controller routes to.
 */
#include "color_picker.h"

#include "gl_2d.h"

#include <math.h>

void ui_color_picker_render(const ColorPickerView *view,
                            int viewport_w, int viewport_h) {
    if (!view || !view->open) return;

    /* Slider geometry constants reused by the renderer. The hit-test
     * indexes view->rects directly; these only feed the popup
     * background, preview swatch, and alpha checkerboard sizing. */
    enum { CP_GAP = 6, CP_PREV_H = 16 };

    int px = view->anchor_px;
    int py = view->anchor_py;
    int sz = view->rects.sv_sz;
    int hue_w = view->rects.hue_w;
    int alp_w = view->rects.alp_w;
    int pw = sz + CP_GAP + hue_w
           + (view->has_alpha ? CP_GAP + alp_w : 0) + CP_GAP;
    int ph = sz + CP_GAP + CP_PREV_H + CP_GAP;

    gl2d_begin(viewport_w, viewport_h);

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.08f, 0.08f, 0.12f, 0.94f);
    glRectf((float)(px-CP_GAP), (float)(py-ph), (float)(px-CP_GAP)+(float)(pw+CP_GAP), (float)(py-ph)+(float)(ph+CP_GAP));
    glColor4f(0.30f, 0.30f, 0.50f, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(px-CP_GAP,        py-ph);
    glVertex2f(px-CP_GAP+pw+CP_GAP, py-ph);
    glVertex2f(px-CP_GAP+pw+CP_GAP, py+CP_GAP);
    glVertex2f(px-CP_GAP,        py+CP_GAP);
    glEnd();
    glDisable(GL_BLEND);

    /* SV square: white→hue left-right, hue→black top-bottom */
    float hr,hg,hb; color_picker_hsv_to_rgb(view->hue,1,1,&hr,&hg,&hb);
    glBegin(GL_QUADS);
    glColor3f(1,1,1);    glVertex2f(px,    py);
    glColor3f(hr,hg,hb); glVertex2f(px+sz, py);
    glColor3f(hr,hg,hb); glVertex2f(px+sz, py-sz);
    glColor3f(1,1,1);    glVertex2f(px,    py-sz);
    glEnd();
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    glColor4f(0,0,0,0); glVertex2f(px,    py);
    glColor4f(0,0,0,0); glVertex2f(px+sz, py);
    glColor4f(0,0,0,1); glVertex2f(px+sz, py-sz);
    glColor4f(0,0,0,1); glVertex2f(px,    py-sz);
    glEnd();
    glDisable(GL_BLEND);
    /* Shade the V > value_max zone to mark it off-limits (used by
     * CMD_CLEAR_COLOR which clamps to CP_CLEAR_MAX_V). */
    if (view->value_max < 1.0f) {
        float lim_y = py - (1.0f - view->value_max) * (float)sz;
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0.0f, 0.0f, 0.0f, 0.72f);
        glRectf((float)px, lim_y, (float)px + (float)sz, lim_y + (py - lim_y));
        glDisable(GL_BLEND);
        glColor3f(0.85f, 0.85f, 0.50f); glLineWidth(1.5f);
        glBegin(GL_LINES);
        glVertex2f((float)px, lim_y); glVertex2f((float)(px+sz), lim_y);
        glEnd();
        glLineWidth(1.0f);
    }

    /* SV cursor (small pair of rings centred on (sat, val)). */
    {
        float cx = px + view->sat * sz;
        float cy = py - (1.0f - view->val) * sz;
        for (int pass = 0; pass < 2; pass++) {
            float r = (pass == 0) ? 5.0f : 6.5f;
            if (pass == 0) { glColor3f(1,1,1); glLineWidth(1.5f); }
            else           { glColor3f(0,0,0); glLineWidth(1.0f); }
            glBegin(GL_LINE_LOOP);
            for (int i = 0; i < 16; i++) {
                float a = (float)i / 16.0f * 6.28318f;
                glVertex2f(cx + cosf(a) * r, cy + sinf(a) * r);
            }
            glEnd();
        }
    }

    /* Hue bar: hue=0 at top, hue=1 at bottom */
    int hx = px + sz + CP_GAP;
    for (int i = 0; i < 40; i++) {
        float h1=(float)i/40.0f, h2=(float)(i+1)/40.0f;
        float r1,g1,b1,r2,g2,b2;
        color_picker_hsv_to_rgb(h1,1,1,&r1,&g1,&b1);
        color_picker_hsv_to_rgb(h2,1,1,&r2,&g2,&b2);
        glBegin(GL_QUADS);
        glColor3f(r1,g1,b1); glVertex2f(hx,         py-h1*sz);
        glColor3f(r1,g1,b1); glVertex2f(hx+hue_w,   py-h1*sz);
        glColor3f(r2,g2,b2); glVertex2f(hx+hue_w,   py-h2*sz);
        glColor3f(r2,g2,b2); glVertex2f(hx,         py-h2*sz);
        glEnd();
    }
    glColor3f(1,1,1); glLineWidth(2.0f);
    {
        float hy = py - view->hue * sz;
        glBegin(GL_LINES); glVertex2f(hx-2,hy); glVertex2f(hx+hue_w+2,hy); glEnd();
    }
    glLineWidth(1.0f);

    /* Alpha bar (RGBA-shaped only): alpha=1 at top */
    if (view->has_alpha) {
        int ax = hx + hue_w + CP_GAP;
        float cr,cg,cb; color_picker_hsv_to_rgb(view->hue,view->sat,view->val,&cr,&cg,&cb);
        int ck = 5;
        for (int iy = 0; iy < sz; iy += ck) for (int ix = 0; ix < alp_w; ix += ck) {
            float gv=((ix/ck+iy/ck)%2)?0.35f:0.55f; glColor3f(gv,gv,gv);
            int tw = (ix+ck < alp_w)?ck:alp_w-ix, th = (iy+ck < sz)?ck:sz-iy;
            glRectf((float)(ax+ix), (float)(py-iy-th), (float)(ax+ix)+(float)tw, (float)(py-iy-th)+(float)th);
        }
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        for (int i = 0; i < 40; i++) {
            float a1 = 1.0f - (float)i/40.0f, a2 = 1.0f - (float)(i+1)/40.0f;
            float y1 = py - (float)i/40.0f * sz,  y2 = py - (float)(i+1)/40.0f * sz;
            glBegin(GL_QUADS);
            glColor4f(cr,cg,cb,a1); glVertex2f(ax,         y1);
            glColor4f(cr,cg,cb,a1); glVertex2f(ax+alp_w,   y1);
            glColor4f(cr,cg,cb,a2); glVertex2f(ax+alp_w,   y2);
            glColor4f(cr,cg,cb,a2); glVertex2f(ax,         y2);
            glEnd();
        }
        glDisable(GL_BLEND);
        float ay = py - (1.0f - view->alpha) * sz;
        glColor3f(1,1,1); glLineWidth(2.0f);
        glBegin(GL_LINES); glVertex2f(ax-2,ay); glVertex2f(ax+alp_w+2,ay); glEnd();
        glLineWidth(1.0f);
    }

    /* Preview swatch */
    float pr,pg,pb; color_picker_hsv_to_rgb(view->hue,view->sat,view->val,&pr,&pg,&pb);
    int total_w = sz + CP_GAP + hue_w + (view->has_alpha ? CP_GAP + alp_w : 0);
    int swy = py - sz - CP_GAP;
    if (view->has_alpha) {
        int ck = 4;
        for (int iy = 0; iy < CP_PREV_H; iy += ck) for (int ix = 0; ix < total_w; ix += ck) {
            float gv=((ix/ck+iy/ck)%2)?0.35f:0.55f; glColor3f(gv,gv,gv);
            int tw = (ix+ck < total_w)?ck:total_w-ix, th = (iy+ck < CP_PREV_H)?ck:CP_PREV_H-iy;
            glRectf((float)(px+ix), (float)(swy-CP_PREV_H+iy), (float)(px+ix)+(float)tw, (float)(swy-CP_PREV_H+iy)+(float)th);
        }
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(pr,pg,pb,view->alpha);
    } else {
        glColor3f(pr,pg,pb);
    }
    glRectf((float)px, (float)(swy-CP_PREV_H), (float)px+(float)total_w, (float)(swy-CP_PREV_H)+(float)CP_PREV_H);
    if (view->has_alpha) glDisable(GL_BLEND);
    glColor3f(0.4f,0.4f,0.5f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(px,         swy-CP_PREV_H); glVertex2f(px+total_w, swy-CP_PREV_H);
    glVertex2f(px+total_w, swy);           glVertex2f(px,         swy);
    glEnd();

    gl2d_end();
}

UiHit ui_color_picker_hit_test(const ColorPickerView *view,
                               int mx, int my, int viewport_h) {
    UiHit h = ui_hit_none();
    if (!view || !view->open) return h;
    if (viewport_h <= 0) return h;
    int gl_y = viewport_h - my;

    int region = 0;
    if (mx >= view->rects.sv_x && mx < view->rects.sv_x + view->rects.sv_sz &&
        gl_y >= view->rects.sv_y && gl_y < view->rects.sv_y + view->rects.sv_sz) {
        region = 1; /* SV square */
    } else if (mx >= view->rects.hue_x && mx < view->rects.hue_x + view->rects.hue_w &&
               gl_y >= view->rects.hue_y && gl_y < view->rects.hue_y + view->rects.hue_h) {
        region = 2; /* hue bar */
    } else if (view->has_alpha &&
               mx >= view->rects.alp_x && mx < view->rects.alp_x + view->rects.alp_w &&
               gl_y >= view->rects.alp_y && gl_y < view->rects.alp_y + view->rects.alp_h) {
        region = 3; /* alpha bar */
    }
    if (region == 0) return h;

    h.kind = UI_HIT_COLOR_SWATCH;
    h.cmd_idx = view->active_line;
    h.item_idx = region;
    h.local_x = (float)(mx - view->rects.sv_x);
    h.local_y = (float)(gl_y - view->rects.sv_y);
    return h;
}

void ui_color_picker_render_swatch(const EditorTransformer *t,
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
