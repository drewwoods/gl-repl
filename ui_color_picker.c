/*
 * ui_color_picker.c -- Floating color picker for literal color commands.
 */
#include "sample.h"
#include "repl_command_store.h"
#include "repl_parser.h"
#include "repl_state_views.h"
#include "repl_undo.h"
#include "ui_color_picker.h"
#include "repl_layout.h"

/* ========================================================================= */
/* Color picker                                                               */
/* ========================================================================= */

#define CP_SV_SZ    150   /* SV square side */
#define CP_HUE_W     18   /* hue bar width  */
#define CP_ALPHA_W   18   /* alpha bar width (COLOR4F only) */
#define CP_GAP        6   /* gap between elements */
#define CP_PREV_H    16   /* preview strip height */
/* CP_CLEAR_MAX_V is defined in sample.h */

/* g_cp_line >= 0: picker is open for that cmd index */
static int   g_cp_line     = -1;
static float g_cp_hue      = 0.0f, g_cp_sat = 1.0f, g_cp_val = 1.0f;
static float g_cp_alpha    = 1.0f;
static int   g_cp_px       = 0, g_cp_py = 0;  /* top-left (OpenGL y-up) */
static int   g_cp_drag     = 0;    /* 0=none 1=SV 2=hue 3=alpha */
static int   g_cp_has_alpha= 0;    /* 1 when editing an RGBA color command */

/* Hit-rect bundle in y-up OpenGL coords. Derived from anchor + flags via
 * cp_compute_rects() at both render and input time so render no longer
 * caches geometry that input depends on. */
typedef struct {
    int sv_x, sv_y, sv_sz;
    int hue_x, hue_y, hue_h;
    int alp_x, alp_y, alp_h;
} CpRects;

static void cp_compute_rects(CpRects *r) {
    int px = g_cp_px;
    int py = g_cp_py;
    int sz = CP_SV_SZ;
    int hx = px + sz + CP_GAP;

    r->sv_x  = px;          r->sv_y  = py - sz; r->sv_sz = sz;
    r->hue_x = hx;          r->hue_y = py - sz; r->hue_h = sz;
    r->alp_x = hx + CP_HUE_W + CP_GAP;
    r->alp_y = py - sz;
    r->alp_h = sz;
}

static void cp_hsv_to_rgb(float h, float s, float v,
                           float *r, float *g, float *b) {
    int i = (int)(h*6.0f);
    float f=h*6.0f-i, p=v*(1-s), q=v*(1-f*s), t=v*(1-(1-f)*s);
    switch (i%6) {
    case 0:*r=v;*g=t;*b=p;break; case 1:*r=q;*g=v;*b=p;break;
    case 2:*r=p;*g=v;*b=t;break; case 3:*r=p;*g=q;*b=v;break;
    case 4:*r=t;*g=p;*b=v;break; default:*r=v;*g=p;*b=q;break;
    }
}
static void cp_rgb_to_hsv(float r, float g, float b,
                           float *h, float *s, float *v) {
    float mx=r>g?(r>b?r:b):(g>b?g:b), mn=r<g?(r<b?r:b):(g<b?g:b), d=mx-mn;
    *v=mx; *s=(mx!=0.0f)?d/mx:0.0f;
    if (d==0.0f){*h=0.0f;return;}
    if      (mx==r)*h=fmodf((g-b)/d,6.0f)/6.0f;
    else if (mx==g)*h=((b-r)/d+2.0f)/6.0f;
    else           *h=((r-g)/d+4.0f)/6.0f;
    if(*h<0.0f)*h+=1.0f;
}
static void cp_ring(float cx, float cy, float r, int n) {
    glBegin(GL_LINE_LOOP);
    for (int i=0;i<n;i++){float a=(float)i/(float)n*6.28318f;
        glVertex2f(cx+cosf(a)*r, cy+sinf(a)*r);}
    glEnd();
}

static const GLCmd *cp_cmd_at(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= repl_state_document_count())
        return NULL;
    return repl_state_document_cmd_at(cmd_idx);
}

static void color_picker_write_cmd(void) {
    const GLCmd *cmd = cp_cmd_at(g_cp_line);
    float r, g, b;
    char new_line[MAX_LINE_LEN];
    int written;

    if (!cmd)
        return;

    cp_hsv_to_rgb(g_cp_hue, g_cp_sat, g_cp_val, &r, &g, &b);
    if (cmd->type == CMD_CLEAR_COLOR) {
        if (r > CP_CLEAR_MAX_V) r = CP_CLEAR_MAX_V;
        if (g > CP_CLEAR_MAX_V) g = CP_CLEAR_MAX_V;
        if (b > CP_CLEAR_MAX_V) b = CP_CLEAR_MAX_V;
        written = snprintf(new_line, sizeof(new_line),
                           "glClearColor(%g, %g, %g, %g);",
                           r, g, b, g_cp_alpha);
    } else if (cmd->type == CMD_COLOR3F) {
        written = snprintf(new_line, sizeof(new_line),
                           "glColor3f(%g, %g, %g);", r, g, b);
    } else if (cmd->type == CMD_COLOR4F) {
        written = snprintf(new_line, sizeof(new_line),
                           "glColor4f(%g, %g, %g, %g);",
                           r, g, b, g_cp_alpha);
    } else if (cmd->type == CMD_TESS_COLOR) {
        if (g_cp_has_alpha)
            written = snprintf(new_line, sizeof(new_line),
                               "gluColor(%g, %g, %g, %g);",
                               r, g, b, g_cp_alpha);
        else
            written = snprintf(new_line, sizeof(new_line),
                               "gluColor(%g, %g, %g);", r, g, b);
    } else {
        return;
    }

    if (written < 0 || written >= (int)sizeof(new_line)) {
        set_status("Command too long");
        return;
    }

    ReplParseContext parse_ctx = { g_cp_line, NULL, 0, 0 };
    ReplParsedLine pl;
    if (!repl_parser_parse_command_ctx(new_line, &pl, &parse_ctx))
        return;

    ReplCommandStore store = repl_command_store_live();
    repl_command_store_replace_one(&store, g_cp_line, &pl.cmd, pl.text);
}

/* Open (or switch) the picker for cmd_idx.  my is GLUT screen y coord. */
void ui_color_picker_open(int cmd_idx, int my) {
    int cp_x, cp_w;
    const GLCmd *cmd;

    if (!ui_color_picker_can_edit_cmd(cmd_idx))
        return;

    if (g_cp_line != cmd_idx)
        repl_undo_push_snapshot();

    repl_layout_code_panel_rect(&cp_x, NULL, &cp_w, NULL);
    g_cp_line = cmd_idx;
    cmd = cp_cmd_at(cmd_idx);
    if (!cmd)
        return;
    g_cp_has_alpha = (cmd->type == CMD_COLOR4F ||
                      cmd->type == CMD_TESS_COLOR ||
                      cmd->type == CMD_CLEAR_COLOR);
    g_cp_alpha     = g_cp_has_alpha ? cmd->args[3] : 1.0f;
    cp_rgb_to_hsv(cmd->args[0], cmd->args[1], cmd->args[2],
                  &g_cp_hue, &g_cp_sat, &g_cp_val);
    if (cmd->type == CMD_CLEAR_COLOR && g_cp_val > CP_CLEAR_MAX_V)
        g_cp_val = CP_CLEAR_MAX_V;
    /* Position to the right of the panel, near the click y */
    int pw = CP_SV_SZ + CP_GAP + CP_HUE_W
           + (g_cp_has_alpha ? CP_GAP + CP_ALPHA_W : 0) + CP_GAP;
    int ph = CP_SV_SZ + CP_GAP + CP_PREV_H + CP_GAP;
    int ppx = cp_x + cp_w + 8;
    int win_w = repl_state_viewport().window_w;
    int win_h = repl_state_viewport().window_h;
    if (ppx + pw > win_w - 4) ppx = cp_x - pw - 4;
    if (ppx < 4) ppx = 4;
    if (ppx + pw > win_w - 4) ppx = win_w - pw - 4;
    if (ppx < 4) ppx = 4;
    int ppy = (win_h - my) + ph / 2;
    if (ppy > win_h - 4) ppy = win_h - 4;
    if (ppy - ph < 4)       ppy = ph + 4;
    g_cp_px = ppx;  g_cp_py = ppy;
}

void ui_color_picker_render(const UiRenderSnapshot *snap) {
    const GLCmd *cmd;

    (void)snap;
    if (g_cp_line < 0) return;
    cmd = cp_cmd_at(g_cp_line);
    if (!cmd)
        return;
    int px = g_cp_px, py = g_cp_py, sz = CP_SV_SZ;
    int pw = sz + CP_GAP + CP_HUE_W
           + (g_cp_has_alpha ? CP_GAP + CP_ALPHA_W : 0) + CP_GAP;
    int ph = sz + CP_GAP + CP_PREV_H + CP_GAP;

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
    float hr,hg,hb; cp_hsv_to_rgb(g_cp_hue,1,1,&hr,&hg,&hb);
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
    /* glClearColor: shade the V > CP_CLEAR_MAX_V zone to show it's off-limits */
    if (cmd->type == CMD_CLEAR_COLOR) {
        float lim_y = py - (1.0f - CP_CLEAR_MAX_V) * (float)sz;
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
    /* SV cursor */
    float cx=px+g_cp_sat*sz, cy=py-(1.0f-g_cp_val)*sz;
    glColor3f(1,1,1); glLineWidth(1.5f); cp_ring(cx,cy,5.0f,16);
    glColor3f(0,0,0); glLineWidth(1.0f); cp_ring(cx,cy,6.5f,16);

    /* Hue bar: hue=0 at top, hue=1 at bottom */
    int hx=px+sz+CP_GAP;
    for (int i=0;i<40;i++) {
        float h1=(float)i/40.0f, h2=(float)(i+1)/40.0f;
        float r1,g1,b1,r2,g2,b2;
        cp_hsv_to_rgb(h1,1,1,&r1,&g1,&b1); cp_hsv_to_rgb(h2,1,1,&r2,&g2,&b2);
        glBegin(GL_QUADS);
        glColor3f(r1,g1,b1); glVertex2f(hx,          py-h1*sz);
        glColor3f(r1,g1,b1); glVertex2f(hx+CP_HUE_W, py-h1*sz);
        glColor3f(r2,g2,b2); glVertex2f(hx+CP_HUE_W, py-h2*sz);
        glColor3f(r2,g2,b2); glVertex2f(hx,          py-h2*sz);
        glEnd();
    }
    glColor3f(1,1,1); glLineWidth(2.0f);
    float hy=py-g_cp_hue*sz;
    glBegin(GL_LINES); glVertex2f(hx-2,hy); glVertex2f(hx+CP_HUE_W+2,hy); glEnd();
    glLineWidth(1.0f);

    /* Alpha bar (COLOR4F only): alpha=1 at top */
    if (g_cp_has_alpha) {
        int ax=hx+CP_HUE_W+CP_GAP;
        float cr,cg,cb; cp_hsv_to_rgb(g_cp_hue,g_cp_sat,g_cp_val,&cr,&cg,&cb);
        int ck=5;
        for (int iy=0;iy<sz;iy+=ck) for (int ix=0;ix<CP_ALPHA_W;ix+=ck) {
            float gv=((ix/ck+iy/ck)%2)?0.35f:0.55f; glColor3f(gv,gv,gv);
            int tw=(ix+ck<CP_ALPHA_W)?ck:CP_ALPHA_W-ix, th=(iy+ck<sz)?ck:sz-iy;
            glRectf((float)(ax+ix), (float)(py-iy-th), (float)(ax+ix)+(float)tw, (float)(py-iy-th)+(float)th);
        }
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        for (int i=0;i<40;i++) {
            float a1=1.0f-(float)i/40.0f, a2=1.0f-(float)(i+1)/40.0f;
            float y1=py-(float)i/40.0f*sz,  y2=py-(float)(i+1)/40.0f*sz;
            glBegin(GL_QUADS);
            glColor4f(cr,cg,cb,a1); glVertex2f(ax,            y1);
            glColor4f(cr,cg,cb,a1); glVertex2f(ax+CP_ALPHA_W, y1);
            glColor4f(cr,cg,cb,a2); glVertex2f(ax+CP_ALPHA_W, y2);
            glColor4f(cr,cg,cb,a2); glVertex2f(ax,            y2);
            glEnd();
        }
        glDisable(GL_BLEND);
        float ay=py-(1.0f-g_cp_alpha)*sz;
        glColor3f(1,1,1); glLineWidth(2.0f);
        glBegin(GL_LINES); glVertex2f(ax-2,ay); glVertex2f(ax+CP_ALPHA_W+2,ay); glEnd();
        glLineWidth(1.0f);
    }

    /* Preview swatch */
    float pr,pg,pb; cp_hsv_to_rgb(g_cp_hue,g_cp_sat,g_cp_val,&pr,&pg,&pb);
    int total_w=sz+CP_GAP+CP_HUE_W+(g_cp_has_alpha?CP_GAP+CP_ALPHA_W:0);
    int swy=py-sz-CP_GAP;
    if (g_cp_has_alpha) {
        int ck=4;
        for (int iy=0;iy<CP_PREV_H;iy+=ck) for (int ix=0;ix<total_w;ix+=ck) {
            float gv=((ix/ck+iy/ck)%2)?0.35f:0.55f; glColor3f(gv,gv,gv);
            int tw=(ix+ck<total_w)?ck:total_w-ix, th=(iy+ck<CP_PREV_H)?ck:CP_PREV_H-iy;
            glRectf((float)(px+ix), (float)(swy-CP_PREV_H+iy), (float)(px+ix)+(float)tw, (float)(swy-CP_PREV_H+iy)+(float)th);
        }
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(pr,pg,pb,g_cp_alpha);
    } else {
        glColor3f(pr,pg,pb);
    }
    glRectf((float)px, (float)(swy-CP_PREV_H), (float)px+(float)total_w, (float)(swy-CP_PREV_H)+(float)CP_PREV_H);
    if (g_cp_has_alpha) glDisable(GL_BLEND);
    glColor3f(0.4f,0.4f,0.5f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(px,         swy-CP_PREV_H); glVertex2f(px+total_w, swy-CP_PREV_H);
    glVertex2f(px+total_w, swy);           glVertex2f(px,         swy);
    glEnd();
}

int ui_color_picker_press(int mx, int my) {
    const GLCmd *cmd;
    CpRects r;

    if (g_cp_line < 0) return 0;
    cmd = cp_cmd_at(g_cp_line);
    if (!cmd)
        return 0;
    int gl_y = repl_state_viewport().window_h - my;
    cp_compute_rects(&r);

    /* SV square */
    if (mx >= r.sv_x && mx < r.sv_x+r.sv_sz &&
        gl_y >= r.sv_y && gl_y < r.sv_y+r.sv_sz) {
        g_cp_drag = 1;
        g_cp_sat = (float)(mx-r.sv_x)/(float)r.sv_sz;
        g_cp_val = (float)(gl_y-r.sv_y)/(float)r.sv_sz;
        if (g_cp_sat<0)g_cp_sat=0; if (g_cp_sat>1)g_cp_sat=1;
        if (g_cp_val<0)g_cp_val=0; if (g_cp_val>1)g_cp_val=1;
        if (cmd->type == CMD_CLEAR_COLOR && g_cp_val>CP_CLEAR_MAX_V)
            g_cp_val=CP_CLEAR_MAX_V;
        color_picker_write_cmd(); return 1;
    }
    /* Hue bar */
    if (mx >= r.hue_x && mx < r.hue_x+CP_HUE_W &&
        gl_y >= r.hue_y && gl_y < r.hue_y+r.hue_h) {
        g_cp_drag = 2;
        g_cp_hue = 1.0f-(float)(gl_y-r.hue_y)/(float)r.hue_h;
        if (g_cp_hue<0)g_cp_hue=0; if (g_cp_hue>=1)g_cp_hue=0.999f;
        color_picker_write_cmd(); return 1;
    }
    /* Alpha bar */
    if (g_cp_has_alpha &&
        mx >= r.alp_x && mx < r.alp_x+CP_ALPHA_W &&
        gl_y >= r.alp_y && gl_y < r.alp_y+r.alp_h) {
        g_cp_drag = 3;
        g_cp_alpha = (float)(gl_y-r.alp_y)/(float)r.alp_h;
        if (g_cp_alpha<0)g_cp_alpha=0; if (g_cp_alpha>1)g_cp_alpha=1;
        color_picker_write_cmd(); return 1;
    }

    /* Click outside picker: close and let the event fall through */
    g_cp_line = -1;  g_cp_drag = 0;
    return 0;
}

int ui_color_picker_motion(int mx, int my) {
    const GLCmd *cmd;
    CpRects r;

    if (g_cp_drag == 0) return 0;
    cmd = cp_cmd_at(g_cp_line);
    if (!cmd)
        return 0;
    int gl_y = repl_state_viewport().window_h - my;
    cp_compute_rects(&r);
    if (g_cp_drag == 1) {
        g_cp_sat = (float)(mx-r.sv_x)/(float)r.sv_sz;
        g_cp_val = (float)(gl_y-r.sv_y)/(float)r.sv_sz;
        if (g_cp_sat<0)g_cp_sat=0; if (g_cp_sat>1)g_cp_sat=1;
        if (g_cp_val<0)g_cp_val=0; if (g_cp_val>1)g_cp_val=1;
        if (cmd->type == CMD_CLEAR_COLOR && g_cp_val>CP_CLEAR_MAX_V)
            g_cp_val=CP_CLEAR_MAX_V;
    } else if (g_cp_drag == 2) {
        g_cp_hue = 1.0f-(float)(gl_y-r.hue_y)/(float)r.hue_h;
        if (g_cp_hue<0)g_cp_hue=0; if (g_cp_hue>=1)g_cp_hue=0.999f;
    } else if (g_cp_drag == 3) {
        g_cp_alpha = (float)(gl_y-r.alp_y)/(float)r.alp_h;
        if (g_cp_alpha<0)g_cp_alpha=0; if (g_cp_alpha>1)g_cp_alpha=1;
    }
    color_picker_write_cmd();
    return 1;
}

void ui_color_picker_release(void) { g_cp_drag = 0; }

/* Close the picker.  Returns 1 if it was open (caller should redisplay). */
int ui_color_picker_close(void) {
    if (g_cp_line < 0) return 0;
    g_cp_line = -1;  g_cp_drag = 0;
    return 1;
}


int ui_color_picker_active_line(void) {
    return g_cp_line;
}

int ui_color_picker_can_edit_cmd(int cmd_idx) {
    const GLCmd *cmd = cp_cmd_at(cmd_idx);

    if (!cmd || !cmd->valid || cmd->has_vars)
        return 0;
    return cmd->type == CMD_COLOR3F ||
           cmd->type == CMD_COLOR4F ||
           cmd->type == CMD_TESS_COLOR ||
           cmd->type == CMD_CLEAR_COLOR;
}

void ui_color_picker_render_swatch(int cmd_idx, int sx, int sy) {
    const GLCmd *cmd = cp_cmd_at(cmd_idx);

    if (!ui_color_picker_can_edit_cmd(cmd_idx))
        return;

    int sw = UI_COLOR_SWATCH_W;
    float alpha = (cmd->type == CMD_COLOR4F ||
                   cmd->type == CMD_TESS_COLOR ||
                   cmd->type == CMD_CLEAR_COLOR)
                ? cmd->args[3] : 1.0f;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(cmd->args[0], cmd->args[1], cmd->args[2], alpha);
    glRectf((float)sx, (float)sy, (float)sx + (float)sw, (float)sy + (float)sw);

    glColor4f(0.55f, 0.55f, 0.65f, 0.9f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(sx,    sy);
    glVertex2f(sx+sw, sy);
    glVertex2f(sx+sw, sy+sw);
    glVertex2f(sx,    sy+sw);
    glEnd();

    if (g_cp_line == cmd_idx) {
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
