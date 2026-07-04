/*
 * src/ui/subsystems/color_picker.c -- Floating color picker renderer + hit-test.
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
#include "ui/subsystems/color_picker.h"

#include "ui/core/gl_2d.h"
#include "ui/core/theme.h"

#include <math.h>
#include <string.h>

void ui_color_picker_render(const ColorPickerView *view,
                            int viewport_w, int viewport_h) {
    if (!view || !view->open) return;

    /* Popup-frame sizing comes from the peer via the view, so this
     * renderer reuses color_picker_state.c's CP_GAP/CP_PREV_H values
     * instead of shadowing them with duplicate literals. The hit-test
     * indexes view->rects directly; these only size the popup
     * background, preview swatch, and alpha checkerboard. */
    const int CP_GAP = view->gap;
    const int CP_PREV_H = view->prev_h;

    int px = view->anchor_px;
    int py = view->anchor_py;
    int sz = view->rects.sv_sz;
    int hue_w = view->rects.hue_w;
    int alp_w = view->rects.alp_w;
    /* Whole-popup extent comes from the peer (palette-dependent height). */
    int pw = view->popup_w;
    int ph = view->popup_h;

    gl2d_begin(viewport_w, viewport_h);

    /* Background */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ui_clr_a(UI_TOK_SUNKEN, 0.94f);
    glRectf((float)(px-CP_GAP), (float)(py-ph), (float)(px-CP_GAP)+(float)(pw+CP_GAP), (float)(py-ph)+(float)(ph+CP_GAP));
    ui_clr_a(UI_TOK_BORDER, 0.80f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(px-CP_GAP,        py-ph);
    glVertex2f(px-CP_GAP+pw+CP_GAP, py-ph);
    glVertex2f(px-CP_GAP+pw+CP_GAP, py+CP_GAP);
    glVertex2f(px-CP_GAP,        py+CP_GAP);
    glEnd();
    glDisable(GL_BLEND);

    /* All colors below are computed picker DATA (the HSV gradient,
     * crosshair, preview swatch) - not UI chrome, so intentionally not
     * theme tokens (theme.h bucket 3). */
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
    /* Shade the V > value_max zone to mark command-specific color limits. */
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
    ui_clr(UI_TOK_BORDER);
    glBegin(GL_LINE_LOOP);
    glVertex2f(px,         swy-CP_PREV_H); glVertex2f(px+total_w, swy-CP_PREV_H);
    glVertex2f(px+total_w, swy);           glVertex2f(px,         swy);
    glEnd();

    /* Hex readout (#RRGGBBAA) over the preview strip; text color chosen for
     * contrast against the preview color's luminance. */
    if (view->hex[0]) {
        float lum = 0.299f*pr + 0.587f*pg + 0.114f*pb;
        if (view->has_alpha) lum *= view->alpha;   /* dark checkerboard shows through */
        if (lum > 0.55f) glColor3f(0.0f, 0.0f, 0.0f);
        else             glColor3f(1.0f, 1.0f, 1.0f);
        float hx_x = (float)px + 4.0f;
        float hx_y = (float)(swy - CP_PREV_H) + ((float)CP_PREV_H - FONT_SMALL_H) * 0.5f + 2.0f;
        gl2d_draw_string(hx_x, hx_y, view->hex, FONT_SMALL);
    }

    /* --- Palette tab strip --------------------------------------------- */
    /* Segment boundaries (proportional to label width) and labels come
     * from the peer via the view, so the strip the user clicks matches
     * the strip drawn here. */
    {
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (int s = 0; s < CP_TAB_COUNT; s++) {
            float tx = (float)view->tab_seg_x[s];
            float seg_w = (float)(view->tab_seg_x[s + 1] - view->tab_seg_x[s]);
            float ty = (float)view->tab_y - (float)view->tab_h;  /* bottom */
            int active = (s == view->palette_tab);
            ui_clr(active ? UI_TOK_MENU_LABEL_ACTIVE_BG : UI_TOK_RAISED);
            glRectf(tx, ty, tx + seg_w, ty + (float)view->tab_h);
            const char *lbl = view->tab_labels[s];
            int lw = (int)strlen(lbl) * FONT_SMALL_W;
            float lx = tx + (seg_w - (float)lw) * 0.5f;
            float ly = ty + ((float)view->tab_h - FONT_SMALL_H) * 0.5f + 2.0f;
            ui_clr(active ? UI_TOK_TEXT_ON_HILITE : UI_TOK_TEXT_MUTED);
            gl2d_draw_string(lx, ly, lbl, FONT_SMALL);
        }
        glDisable(GL_BLEND);
        /* Strip border + segment dividers */
        float t_top = (float)view->tab_y;
        float t_bot = (float)(view->tab_y - view->tab_h);
        float t_right = (float)(view->tab_x + view->tab_w);
        ui_clr(UI_TOK_BORDER);
        glBegin(GL_LINE_LOOP);
        glVertex2f((float)view->tab_x, t_top);
        glVertex2f(t_right,            t_top);
        glVertex2f(t_right,            t_bot);
        glVertex2f((float)view->tab_x, t_bot);
        glEnd();
        glBegin(GL_LINES);
        for (int s = 1; s < CP_TAB_COUNT; s++) {
            float dx = (float)view->tab_seg_x[s];
            glVertex2f(dx, t_top); glVertex2f(dx, t_bot);
        }
        glEnd();
    }

    /* --- Palette swatch grid ------------------------------------------- */
    {
        int cell = view->pal_cell, gap = view->pal_gap, cols = view->pal_cols;
        for (int i = 0; i < view->swatch_count; i++) {
            int r = i / cols, c = i % cols;
            float cx = (float)(view->pal_x + c * (cell + gap));
            float cy_top = (float)(view->pal_y - r * (cell + gap));
            float cy_bot = cy_top - (float)cell;
            glColor3f(view->swatches[i][0], view->swatches[i][1], view->swatches[i][2]);
            glRectf(cx, cy_bot, cx + (float)cell, cy_top);
            ui_clr(UI_TOK_BORDER);
            glBegin(GL_LINE_LOOP);
            glVertex2f(cx, cy_bot); glVertex2f(cx + (float)cell, cy_bot);
            glVertex2f(cx + (float)cell, cy_top); glVertex2f(cx, cy_top);
            glEnd();
            /* Mark the harmony key swatch (index 0) with an accent inset. */
            if (i == 0 && view->palette_tab == CP_TAB_HARMONY && view->key_set) {
                ui_clr(UI_TOK_ACCENT);
                glLineWidth(2.0f);
                glBegin(GL_LINE_LOOP);
                glVertex2f(cx + 2,            cy_bot + 2);
                glVertex2f(cx + (float)cell - 2, cy_bot + 2);
                glVertex2f(cx + (float)cell - 2, cy_top - 2);
                glVertex2f(cx + 2,            cy_top - 2);
                glEnd();
                glLineWidth(1.0f);
            }
        }
    }

    /* --- Harmony "Set key" button -------------------------------------- */
    if (view->key_btn_w > 0) {
        float bx = (float)view->key_btn_x;
        float by = (float)(view->key_btn_y - view->key_btn_h);  /* bottom */
        float bw = (float)view->key_btn_w, bh = (float)view->key_btn_h;
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl2d_panel_frame(bx, by, bw, bh, UI_TOK_RAISED, 1.0f, UI_TOK_BORDER, 1.0f);
        glDisable(GL_BLEND);
        const char *lbl = "Set key from current";
        int lw = (int)strlen(lbl) * FONT_SMALL_W;
        float lx = bx + (bw - (float)lw) * 0.5f;
        if (lx < bx + 2) lx = bx + 2;
        float ly = by + (bh - FONT_SMALL_H) * 0.5f + 2.0f;
        ui_clr(UI_TOK_TEXT_PRIMARY);
        gl2d_draw_string(lx, ly, lbl, FONT_SMALL);
    }

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
    } else if (mx >= view->tab_x && mx < view->tab_x + view->tab_w &&
               gl_y <= view->tab_y && gl_y > view->tab_y - view->tab_h) {
        region = 4; /* palette tab strip */
    } else if (view->key_btn_w > 0 &&
               mx >= view->key_btn_x && mx < view->key_btn_x + view->key_btn_w &&
               gl_y <= view->key_btn_y && gl_y > view->key_btn_y - view->key_btn_h) {
        region = 6; /* harmony "set key" button */
    } else {
        /* Palette swatch grid */
        int cell = view->pal_cell, gap = view->pal_gap, cols = view->pal_cols;
        for (int i = 0; i < view->swatch_count; i++) {
            int r = i / cols, c = i % cols;
            int cx = view->pal_x + c * (cell + gap);
            int cy_top = view->pal_y - r * (cell + gap);
            if (mx >= cx && mx < cx + cell && gl_y <= cy_top && gl_y > cy_top - cell) {
                region = 5; /* palette swatch */
                break;
            }
        }
    }
    if (region == 0) return h;

    h.kind = UI_HIT_COLOR_SWATCH;
    h.cmd_idx = view->active_line;
    h.item_idx = region;
    h.local_x = (float)(mx - view->rects.sv_x);
    h.local_y = (float)(gl_y - view->rects.sv_y);
    return h;
}
