/*
 * Table-driven code-panel status strip. See repl_code_panel_statusbar.h.
 *
 * Adding an item: one k_statusbar_items[] row + prepare (and draw if it
 * is not reuse-draw_state_text / a keycap icon). New UiHitKind + router
 * arm only if it is clickable. Do not add a parallel switch.
 */
#include "ui/app/repl_code_panel_statusbar.h"

#include "c_compat.h"
#include "config.h"
#include "keys.h"
#include "keymap.h"
#include "repl/program_query.h"
#include "subsystems/edit_overlays/edit_overlays.h"
#include "support/cpuprof.h"
#include "ui/app/hit.h"
#include "ui/app/layout.h"
#include "ui/core/gl_2d.h"
#include "ui/core/metrics.h"
#include "ui/core/theme.h"

#include <stdio.h>
#include <string.h>

/* 22 live items today. Room to grow; STATIC_ASSERT guards the table. */
#define STATUSBAR_ITEM_MAX 24
#define STATUSBAR_SEP_W    16
/* Shared by Overlay scope / Vertex labels / Polygon highlight. Only the
 * item with is_group_host paints the hover band across the whole group. */
#define STATUSBAR_GROUP_ID_OVERLAY 1

#define TEXT_PANEL_VRULE_GLYPH 0x19

typedef enum {
    STATUSBAR_ALIGN_LEFT = 0,
    STATUSBAR_ALIGN_CENTER,
    STATUSBAR_ALIGN_RIGHT
} StatusbarAlign;

typedef enum {
    STATUSBAR_GAP_NONE = 0, /* first item of a cluster; never applied */
    STATUSBAR_GAP_PACK,     /* FONT_SMALL_W, no divider: "AA" + "4x" */
    STATUSBAR_GAP_SEP,      /* STATUSBAR_SEP_W + vertical divider */
    STATUSBAR_GAP_PAIR,     /* 4px: undo/redo, copy/cut/paste */
    STATUSBAR_GAP_GROUP,    /* 6px: redo|copy, paste|trash */
    STATUSBAR_GAP_CLUSTER   /* 12px: trash | focus | help */
} StatusbarGap;

/* Per-frame geometry handed to prepare/draw. ky/kh is the keycap box,
 * inset 3px from the strip so the panel-divider grab band stays free. */
typedef struct {
    const UiRenderSnapshot *snap;
    int sx, sy, sw, sh;
    int text_y, ky, kh;
} StatusbarSlot;

/* Prepare-time output. eligible is "wants to exist this frame"
 * (unbalanced/cost/AA hide themselves). placed.visible is decided later
 * by cull / collision hide - never reuse one flag for both. */
typedef struct {
    int  eligible;
    int  active;        /* accent vs muted; cmds uses this as "not overflow" */
    int  warn;          /* draw in UI_TOK_STATUS_WARN over state color */
    int  width;         /* text width; ignored when has_keycap */
    char text[64];
    char tooltip[48];   /* empty = use item->tooltip */
} StatusbarPrepared;

typedef void (*StatusbarPrepareFn)(const StatusbarSlot *, StatusbarPrepared *);
typedef void (*StatusbarDrawFn)(const StatusbarSlot *,
                                const StatusbarPrepared *,
                                int x, int y, int w, int h);

typedef struct {
    StatusbarAlign     align;
    StatusbarGap       gap_before;     /* vs previous *visible* same-align item */
    UiHitKind          hit;            /* UI_HIT_NONE = display-only */
    const char        *tooltip;        /* NULL = no popup */
    int                key_code;       /* KM_KEY(GLR_*); 0 = label-only tooltip */
    int                modifiers;      /* KM_MODS(GLR_*); Redo is Shift */
    int                is_special;     /* 1 for GLUT_KEY_* (F1/F7/F8) */
    int                group_id;       /* 0 = none */
    int                is_group_host;  /* hovering this lights the whole group */
    int                cull_rank;      /* center only: higher dropped first */
    int                has_keycap;     /* engine draws chrome, then draw() */
    StatusbarPrepareFn prepare;
    StatusbarDrawFn    draw;
} StatusbarItem;

typedef struct {
    int item_idx;
    int x, y, w, h;
    int visible;            /* post-layout: survived cull / collision hide */
    StatusbarPrepared prep;
} StatusbarPlaced;

typedef struct {
    int text_y, ky, kh;
    int count;
    StatusbarPlaced items[STATUSBAR_ITEM_MAX];
} StatusbarLayout;

static int statusbar_gap_px(StatusbarGap gap) {
    switch (gap) {
    case STATUSBAR_GAP_NONE:    return 0;
    case STATUSBAR_GAP_PACK:    return FONT_SMALL_W;
    case STATUSBAR_GAP_SEP:     return STATUSBAR_SEP_W;
    case STATUSBAR_GAP_PAIR:    return 4;
    case STATUSBAR_GAP_GROUP:   return 6;
    case STATUSBAR_GAP_CLUSTER: return 12;
    }
    return 0;
}

static int statusbar_point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void statusbar_draw_sep_at(int x, int sy, int sh) {
    ui_clr(UI_TOK_DIVIDER);
    glBegin(GL_LINES);
    glVertex2f((float)x, (float)(sy + 4));
    glVertex2f((float)x, (float)(sy + sh - 4));
    glEnd();
}

/* Color answers one question for every center readout: "is this setting
 * changing what I see right now?" The value is already in the text
 * ("OS scene" vs "OS poly"), so colour does not also encode the mode. */
static void statusbar_state_color(int active) {
    if (active)
        ui_clr(UI_TOK_ACCENT);
    else
        ui_clr_a(UI_TOK_TEXT_MUTED, 0.50f);
}

static void statusbar_group_band(int x0, int x1, int ky, int kh) {
    if (x1 <= x0)
        return;
    ui_clr_a(UI_TOK_ACCENT_GLOW_BG, 0.55f);
    glRectf((float)(x0 - 4), (float)ky, (float)(x1 + 4), (float)(ky + kh));
}

/* Scope is a modifier with no output of its own - it selects which
 * vertices the overlay family acts on. With every consumer off it
 * governs nothing, so it renders inert whatever its value. Transform
 * guides live in render3d/; 0 is OFF (X-macro leads with OFF) and this
 * file must not include that band. */
static int overlay_scope_is_live(const UiRenderSnapshot *snap) {
    return snap->config_values[GLR_CONFIG_VERTEX_LABELS] != OVERLAY_VERTEX_LABEL_OFF ||
           snap->config_values[GLR_CONFIG_POLY_HIGHLIGHT] != POLY_HIGHLIGHT_OFF ||
           snap->config_values[GLR_CONFIG_NORMAL_VECTORS] != 0 ||
           snap->config_values[GLR_CONFIG_VERTEX_OUTLINES] != 0 ||
           snap->config_values[GLR_CONFIG_XFORM_GUIDE_MODE] != 0;
}

static void prepare_text_len(StatusbarPrepared *p) {
    p->width = (int)strlen(p->text) * FONT_SMALL_W;
}

static void prepare_cmds(const StatusbarSlot *slot, StatusbarPrepared *p) {
    const UiRenderSnapshot *snap = slot->snap;

    snprintf(p->text, sizeof p->text, "%d/%d cmds",
             snap->flat_program_overflow_count > 0
                 ? snap->flat_program_overflow_count
                 : snap->flat_program_count,
             MAX_FLAT_COMMANDS);
    p->eligible = 1;
    /* active=0 means overflow: draw uses STATUS_ERR_TEXT. */
    p->active = snap->flat_program_overflow_count <= 0;
    prepare_text_len(p);
}

static void prepare_line(const StatusbarSlot *slot, StatusbarPrepared *p) {
    const UiRenderSnapshot *snap = slot->snap;
    int edit_col = snap->editor_input.cursor_pos + 1;

    if (snap->editor_input.insert_mode)
        snprintf(p->text, sizeof p->text, "Ln %d:%d [INSERT]",
                 snap->edit_line + 1, edit_col);
    else if (snap->in_begin_block)
        snprintf(p->text, sizeof p->text, "Ln %d:%d  %s",
                 snap->edit_line + 1, edit_col,
                 repl_mode_name(snap->current_begin_mode));
    else
        snprintf(p->text, sizeof p->text, "Ln %d:%d",
                 snap->edit_line + 1, edit_col);
    p->eligible = 1;
    p->active = 0;
    prepare_text_len(p);
}

/* Which funcN body the cursor is in. The controller resolves the name
 * (alias, else "funcN") and leaves it empty at top level, which is what
 * hides the segment - the display() body is the whole document and
 * naming it would say nothing. */
static void prepare_func(const StatusbarSlot *slot, StatusbarPrepared *p) {
    const UiRenderSnapshot *snap = slot->snap;

    p->eligible = snap->cursor_func_name[0] != '\0';
    p->active = 0;
    p->width = 0;
    p->text[0] = '\0';
    if (!p->eligible)
        return;
    snprintf(p->text, sizeof p->text, "%s()", snap->cursor_func_name);
    prepare_text_len(p);
}

static void prepare_unbal(const StatusbarSlot *slot, StatusbarPrepared *p) {
    const UiRenderSnapshot *snap = slot->snap;

    p->eligible = snap->unbalanced_count > 0;
    p->active = 0;
    p->width = 0;
    p->text[0] = '\0';
    if (!p->eligible)
        return;
    if (snap->unbalanced_warning[0] != '\0')
        snprintf(p->text, sizeof p->text, "%s", snap->unbalanced_warning);
    else
        snprintf(p->text, sizeof p->text, "%d unbalanced",
                 snap->unbalanced_count);
    prepare_text_len(p);
}

static void prepare_cost(const StatusbarSlot *slot, StatusbarPrepared *p) {
    const UiRenderSnapshot *snap = slot->snap;

    p->eligible = snap->cursor_cost_label[0] != '\0';
    p->active = 1;
    p->width = 0;
    p->text[0] = '\0';
    if (!p->eligible)
        return;
    snprintf(p->text, sizeof p->text, "%s cmds %d",
             snap->cursor_cost_label, snap->cursor_cost_count);
    prepare_text_len(p);
}

static void prepare_aa(const StatusbarSlot *slot, StatusbarPrepared *p) {
    const UiRenderSnapshot *snap = slot->snap;

    /* Hidden unless the accum path is even in play. */
    p->eligible = snap->render.use_accum ? 1 : 0;
    p->active = snap->render.accum_effect != RENDER3D_ACCUM_EFFECT_OFF &&
                snap->render.accum_passes > 1;
    p->warn = snap->perf_hint_active &&
              snap->perf_hint_culprit == GLR_PERF_CULPRIT_ACCUM;
    p->width = 0;
    p->text[0] = '\0';
    if (!p->eligible)
        return;
    switch (snap->render.accum_effect) {
    case RENDER3D_ACCUM_EFFECT_AA:
        snprintf(p->text, sizeof p->text, "AA");
        break;
    case RENDER3D_ACCUM_EFFECT_BLUR:
        snprintf(p->text, sizeof p->text, "Blur");
        break;
    case RENDER3D_ACCUM_EFFECT_BLUR_CAMERA:
        snprintf(p->text, sizeof p->text, "Cam");
        break;
    case RENDER3D_ACCUM_EFFECT_OFF:
    default:
        snprintf(p->text, sizeof p->text, "noAA");
        break;
    }
    prepare_text_len(p);
}

static void prepare_aa_passes(const StatusbarSlot *slot, StatusbarPrepared *p) {
    const UiRenderSnapshot *snap = slot->snap;

    p->eligible = snap->render.use_accum ? 1 : 0;
    p->active = snap->render.accum_passes > 1;
    p->warn = snap->perf_hint_active &&
              snap->perf_hint_culprit == GLR_PERF_CULPRIT_ACCUM;
    p->width = 0;
    p->text[0] = '\0';
    if (!p->eligible)
        return;
    snprintf(p->text, sizeof p->text, "%dx",
             snap->render.accum_passes > 0 ? snap->render.accum_passes : 1);
    prepare_text_len(p);
}

static void prepare_scope(const StatusbarSlot *slot, StatusbarPrepared *p) {
    switch ((OverlayScope)slot->snap->config_values[GLR_CONFIG_OVERLAY_SCOPE]) {
    case OVERLAY_SCOPE_ALL_INSTANCES:
        snprintf(p->text, sizeof p->text, "OS all");
        break;
    case OVERLAY_SCOPE_WHOLE_SCENE:
        snprintf(p->text, sizeof p->text, "OS scene");
        break;
    case OVERLAY_SCOPE_SINGLE_POLYGON:
        snprintf(p->text, sizeof p->text, "OS poly");
        break;
    case OVERLAY_SCOPE_LAST_INSTANCE:
    default:
        snprintf(p->text, sizeof p->text, "OS last");
        break;
    }
    p->eligible = 1;
    p->active = overlay_scope_is_live(slot->snap);
    prepare_text_len(p);
}

static void prepare_vlabel(const StatusbarSlot *slot, StatusbarPrepared *p) {
    switch ((OverlayVertexLabelMode)slot->snap->config_values[GLR_CONFIG_VERTEX_LABELS]) {
    case OVERLAY_VERTEX_LABEL_INDEX:
        snprintf(p->text, sizeof p->text, "VL idx");
        break;
    case OVERLAY_VERTEX_LABEL_INDEX_POS:
        snprintf(p->text, sizeof p->text, "VL pos");
        break;
    case OVERLAY_VERTEX_LABEL_INDEX_WORLD:
        snprintf(p->text, sizeof p->text, "VL world");
        break;
    case OVERLAY_VERTEX_LABEL_INDEX_WORLD_FINE:
        snprintf(p->text, sizeof p->text, "VL fine");
        break;
    case OVERLAY_VERTEX_LABEL_OFF:
    default:
        snprintf(p->text, sizeof p->text, "VL off");
        break;
    }
    p->eligible = 1;
    p->active = slot->snap->config_values[GLR_CONFIG_VERTEX_LABELS] !=
                OVERLAY_VERTEX_LABEL_OFF;
    prepare_text_len(p);
}

static void prepare_poly(const StatusbarSlot *slot, StatusbarPrepared *p) {
    switch ((PolyHighlightMode)slot->snap->config_values[GLR_CONFIG_POLY_HIGHLIGHT]) {
    case POLY_HIGHLIGHT_OFF:
        snprintf(p->text, sizeof p->text, "PH off");
        break;
    case POLY_HIGHLIGHT_CLIPPED_CULLED:
        snprintf(p->text, sizeof p->text, "PH cull");
        break;
    case POLY_HIGHLIGHT_ON:
    default:
        snprintf(p->text, sizeof p->text, "PH on");
        break;
    }
    p->eligible = 1;
    p->active = slot->snap->config_values[GLR_CONFIG_POLY_HIGHLIGHT] !=
                POLY_HIGHLIGHT_OFF;
    prepare_text_len(p);
}

static void prepare_always(const StatusbarSlot *slot, StatusbarPrepared *p) {
    (void)slot;
    p->eligible = 1;
    p->active = 1;
    p->width = 0;
    p->text[0] = '\0';
}

static void prepare_undo(const StatusbarSlot *slot, StatusbarPrepared *p) {
    prepare_always(slot, p);
    p->active = slot->snap->can_undo;
}

static void prepare_redo(const StatusbarSlot *slot, StatusbarPrepared *p) {
    prepare_always(slot, p);
    p->active = slot->snap->can_redo;
}

static void prepare_focus(const StatusbarSlot *slot, StatusbarPrepared *p) {
    prepare_always(slot, p);
    p->active = slot->snap->code_panel.code_focus;
}

static void prepare_help(const StatusbarSlot *slot, StatusbarPrepared *p) {
    prepare_always(slot, p);
    p->active = slot->snap->help.visible;
}

static void prepare_select_all(const StatusbarSlot *slot, StatusbarPrepared *p) {
    prepare_always(slot, p);
    p->active = slot->snap->document_count > 0;
}

static void draw_text_at(const StatusbarSlot *slot, const char *text,
                         int x, UiThemeToken tok) {
    ui_clr(tok);
    gl2d_draw_string((float)x, (float)slot->text_y, text, FONT_SMALL);
}

static void draw_cmds(const StatusbarSlot *slot, const StatusbarPrepared *p,
                      int x, int y, int w, int h) {
    (void)y; (void)w; (void)h;
    draw_text_at(slot, p->text, x,
                 p->active ? UI_TOK_TEXT_PRIMARY : UI_TOK_STATUS_ERR_TEXT);
}

/* Plain muted readout: the cursor's line/column and the function it
 * sits in. Both state where the caret is, so they share a weight. */
static void draw_muted_text(const StatusbarSlot *slot, const StatusbarPrepared *p,
                            int x, int y, int w, int h) {
    (void)y; (void)w; (void)h;
    draw_text_at(slot, p->text, x, UI_TOK_TEXT_MUTED);
}

static void draw_unbal(const StatusbarSlot *slot, const StatusbarPrepared *p,
                       int x, int y, int w, int h) {
    (void)y; (void)w; (void)h;
    draw_text_at(slot, p->text, x, UI_TOK_STATUS_WARN);
}

static void prepare_perf(const StatusbarSlot *slot, StatusbarPrepared *p) {
    const UiRenderSnapshot *snap = slot->snap;
    const char *effect;
    char body[40];

    p->eligible = snap->perf_hint_active;
    p->active = 1;
    p->warn = 1;
    p->width = 0;
    p->text[0] = '\0';
    p->tooltip[0] = '\0';
    if (!p->eligible)
        return;

    body[0] = '\0';
    switch (snap->perf_hint_culprit) {
    case GLR_PERF_CULPRIT_ACCUM:
        switch (snap->render.accum_effect) {
        case RENDER3D_ACCUM_EFFECT_AA:          effect = "AA";   break;
        case RENDER3D_ACCUM_EFFECT_BLUR:        effect = "Blur"; break;
        case RENDER3D_ACCUM_EFFECT_BLUR_CAMERA: effect = "Cam";  break;
        case RENDER3D_ACCUM_EFFECT_OFF:
        default:                                effect = "noAA"; break;
        }
        snprintf(body, sizeof body, "Accum %s %dx", effect,
                 snap->render.accum_passes > 0 ? snap->render.accum_passes : 1);
        snprintf(p->tooltip, sizeof p->tooltip, "Accum passes -> 1");
        break;
    case GLR_PERF_CULPRIT_POST_FX_FRAME:
        snprintf(body, sizeof body, "Post FX Frame");
        snprintf(p->tooltip, sizeof p->tooltip, "Post FX -> Off");
        break;
    case GLR_PERF_CULPRIT_POST_FX_VIEW:
        snprintf(body, sizeof body, "Post FX View");
        snprintf(p->tooltip, sizeof p->tooltip, "Post FX -> Off");
        break;
    case GLR_PERF_CULPRIT_LINE_SMOOTH:
        snprintf(body, sizeof body, "Line smooth");
        snprintf(p->tooltip, sizeof p->tooltip, "Line smooth -> Off");
        break;
    case GLR_PERF_CULPRIT_NONE:
    case GLR_PERF_CULPRIT_COUNT:
        break;
    }

    if (snap->perf_hint_culprit_count > 1)
        snprintf(p->text, sizeof p->text, "! %d fps  %s +%d more",
                 snap->perf_hint_fps, body,
                 snap->perf_hint_culprit_count - 1);
    else
        snprintf(p->text, sizeof p->text, "! %d fps  %s",
                 snap->perf_hint_fps, body);
    prepare_text_len(p);
}

static void prepare_perf_fix(const StatusbarSlot *slot, StatusbarPrepared *p) {
    p->eligible = slot->snap->perf_hint_active;
    p->active = 1;
    p->width = 0;
    p->text[0] = '\0';
    if (!p->eligible)
        return;
    snprintf(p->text, sizeof p->text, "[off]");
    prepare_text_len(p);
}

static void prepare_perf_dismiss(const StatusbarSlot *slot, StatusbarPrepared *p) {
    p->eligible = slot->snap->perf_hint_active;
    p->active = 1;
    p->width = 0;
    p->text[0] = '\0';
    if (!p->eligible)
        return;
    snprintf(p->text, sizeof p->text, "[x]");
    prepare_text_len(p);
}

static void draw_perf(const StatusbarSlot *slot, const StatusbarPrepared *p,
                      int x, int y, int w, int h) {
    (void)y; (void)w; (void)h;
    draw_text_at(slot, p->text, x, UI_TOK_STATUS_WARN);
}

static void draw_perf_chip(const StatusbarSlot *slot, const StatusbarPrepared *p,
                           int x, int y, int w, int h) {
    (void)y; (void)w; (void)h;
    gl2d_chip_action((float)x, (float)slot->text_y, p->text);
}

static void draw_cost(const StatusbarSlot *slot, const StatusbarPrepared *p,
                      int x, int y, int w, int h) {
    (void)y; (void)w; (void)h;
    draw_text_at(slot, p->text, x, UI_TOK_ACCENT);
}

static void draw_state_text(const StatusbarSlot *slot, const StatusbarPrepared *p,
                            int x, int y, int w, int h) {
    (void)y; (void)w; (void)h;
    if (p->warn)
        ui_clr(UI_TOK_STATUS_WARN);
    else
        statusbar_state_color(p->active);
    gl2d_draw_string((float)x, (float)slot->text_y, p->text, FONT_SMALL);
}

/* A sunken keycap chip (box + divider border) using theme tokens. The
 * caller draws the centred key glyphs afterwards with its own color so
 * the focus chip can tint them by ON/OFF state. */
static void repl_code_panel_draw_keycap(int kx, int ky, int kw, int kh) {
    ui_clr(UI_TOK_SUNKEN);
    glRectf((float)kx, (float)ky, (float)(kx + kw), (float)(ky + kh));
    ui_clr(UI_TOK_DIVIDER);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)kx, (float)ky);
    glVertex2f((float)(kx + kw), (float)ky);
    glVertex2f((float)(kx + kw), (float)(ky + kh));
    glVertex2f((float)kx, (float)(ky + kh));
    glEnd();
}

#define TRASH_ICON_W 13
#define TRASH_ICON_H 12
#define ACTION_ICON_W 13
#define ACTION_ICON_H 12

/* Centre an icon_w x icon_h 1bpp glyph in the (kx,ky,kw,kh) keycap box
 * and draw it with glBitmap. Shared by the trash / undo / redo / copy /
 * cut / paste statusbar chips so each glyph is just a bit table. */
static void repl_code_panel_draw_bitmap_icon(int kx, int ky, int kw, int kh,
                                             int icon_w, int icon_h,
                                             const GLubyte *bits) {
    GLint prev_align = 4;
    int rx = kx + (kw - icon_w) / 2;
    int ry = ky + (kh - icon_h) / 2;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
    glRasterPos2f((float)rx, (float)ry);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBitmap(icon_w, icon_h, 0.0f, 0.0f, 0.0f, 0.0f, bits);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
}

static void repl_code_panel_draw_trash_icon(int kx, int ky, int kw, int kh) {
    /* 13x12  1bpp trash-can glyph, drawn with glBitmap so it matches
     * the crisp pixel-art style of the Shift glyph and the bitmap
     * font.  Rows are bottom-to-top (glBitmap scan order); bit 0x80
     * of byte 0 = leftmost pixel (col 0).  The three interior slats
     * are inset a pixel from both rims so they read as ribs inside
     * the can, not bars fused to it.
     *
     *  row 11:  ....#####....   handle tab  (cols 4-8)
     *  row 10:  .###########.   lid         (cols 1-11, overhangs body)
     *  row 9:   .............   lid / body gap
     *  row 8:   ..#########..   body top    (cols 2-10)
     *  row 7:   ..#.......#..   walls only (slat inset)
     *  row 6:   ..#.#.#.#.#..   walls + 3 interior slats
     *  row 5:   ..#.#.#.#.#..
     *  row 4:   ..#.#.#.#.#..
     *  row 3:   ..#.#.#.#.#..
     *  row 2:   ..#.#.#.#.#..
     *  row 1:   ..#.......#..   walls only (slat inset)
     *  row 0:   ..#########..   body bottom (cols 2-10)               */
    static const GLubyte trash_bits[TRASH_ICON_H * 2] = {
        0x3F, 0xE0,  /* row 0  body bottom  ..#########..            */
        0x20, 0x20,  /* row 1  walls        ..#.......#..            */
        0x2A, 0xA0,  /* row 2  slats        ..#.#.#.#.#..            */
        0x2A, 0xA0,  /* row 3  slats        ..#.#.#.#.#..            */
        0x2A, 0xA0,  /* row 4  slats        ..#.#.#.#.#..            */
        0x2A, 0xA0,  /* row 5  slats        ..#.#.#.#.#..            */
        0x2A, 0xA0,  /* row 6  slats        ..#.#.#.#.#..            */
        0x20, 0x20,  /* row 7  walls        ..#.......#..            */
        0x3F, 0xE0,  /* row 8  body top     ..#########..            */
        0x00, 0x00,  /* row 9  lid gap                               */
        0x7F, 0xF0,  /* row 10 lid          .###########.            */
        0x0F, 0x80   /* row 11 handle       ....#####....            */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     TRASH_ICON_W, TRASH_ICON_H, trash_bits);
}

static void repl_code_panel_draw_undo_icon(int kx, int ky, int kw, int kh) {
    /* 13x12 1bpp "return hook" glyph: a riser down the right side that
     * bends into a leftward-pointing arrowhead, i.e. the same shape as
     * the redo icon mirrored left-right.  The chevron arms converge onto
     * the single shaft row (row 4), so the arrow point is one row, not
     * two.
     *
     *  row 11:  .........##..   riser
     *  row 10:  .........##..
     *  row 9:   .........##..
     *  row 8:   .........##..
     *  row 7:   ....##...##..   upper arm
     *  row 6:   ...##....##..
     *  row 5:   ..##.....##..
     *  row 4:   .##########..   shaft + single-row arrowhead point
     *  row 3:   ..##.........
     *  row 2:   ...##........
     *  row 1:   ....##.......   lower arm
     *  row 0:   .............                                        */
    static const GLubyte undo_bits[ACTION_ICON_H * 2] = {
        0x00, 0x00,  /* row 0  */
        0x0C, 0x00,  /* row 1  */
        0x18, 0x00,  /* row 2  */
        0x30, 0x00,  /* row 3  */
        0x7F, 0xE0,  /* row 4  */
        0x30, 0x60,  /* row 5  */
        0x18, 0x60,  /* row 6  */
        0x0C, 0x60,  /* row 7  */
        0x00, 0x60,  /* row 8  */
        0x00, 0x60,  /* row 9  */
        0x00, 0x60,  /* row 10 */
        0x00, 0x60   /* row 11 */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, undo_bits);
}

static void repl_code_panel_draw_redo_icon(int kx, int ky, int kw, int kh) {
    /* Mirror image of the undo glyph: riser down the left side bending
     * into a rightward-pointing arrowhead.  Like undo, the chevron arms
     * converge onto the single shaft row (row 4) so the arrow point is
     * one row, not two.
     *
     *  row 11:  ..##.........
     *  row 10:  ..##.........
     *  row 9:   ..##.........
     *  row 8:   ..##.........
     *  row 7:   ..##...##....   upper arm
     *  row 6:   ..##....##...
     *  row 5:   ..##.....##..
     *  row 4:   ..##########.   shaft + single-row arrowhead point
     *  row 3:   .........##..
     *  row 2:   ........##...
     *  row 1:   .......##....   lower arm
     *  row 0:   .............                                        */
    static const GLubyte redo_bits[ACTION_ICON_H * 2] = {
        0x00, 0x00,  /* row 0  */
        0x01, 0x80,  /* row 1  */
        0x00, 0xC0,  /* row 2  */
        0x00, 0x60,  /* row 3  */
        0x3F, 0xF0,  /* row 4  */
        0x30, 0x60,  /* row 5  */
        0x30, 0xC0,  /* row 6  */
        0x31, 0x80,  /* row 7  */
        0x30, 0x00,  /* row 8  */
        0x30, 0x00,  /* row 9  */
        0x30, 0x00,  /* row 10 */
        0x30, 0x00   /* row 11 */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, redo_bits);
}

static void repl_code_panel_draw_copy_icon(int kx, int ky, int kw, int kh) {
    /* Two stacked portrait pages: the front sheet (bottom-left, 7x9
     * outline) occludes the back sheet, whose top-left corner and
     * right/bottom edges peek out behind it with a 1px gap all round
     * so the outlines never fuse.
     *
     *  row 11:  .....#######.   back page top
     *  row 10:  .....#.....#.
     *  row 9:   ...........#.
     *  row 8:   .#######...#.   front page top
     *  row 7:   .#.....#...#.
     *  row 6:   .#.....#...#.
     *  row 5:   .#.....#...#.
     *  row 4:   .#.....#...#.
     *  row 3:   .#.....#.###.   back page bottom (visible part)
     *  row 2:   .#.....#.....
     *  row 1:   .#.....#.....
     *  row 0:   .#######.....   front page bottom                    */
    static const GLubyte copy_bits[ACTION_ICON_H * 2] = {
        0x7F, 0x00,  /* row 0  */
        0x41, 0x00,  /* row 1  */
        0x41, 0x00,  /* row 2  */
        0x41, 0x70,  /* row 3  */
        0x41, 0x10,  /* row 4  */
        0x41, 0x10,  /* row 5  */
        0x41, 0x10,  /* row 6  */
        0x41, 0x10,  /* row 7  */
        0x7F, 0x10,  /* row 8  */
        0x00, 0x10,  /* row 9  */
        0x04, 0x10,  /* row 10 */
        0x07, 0xF0   /* row 11 */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, copy_bits);
}

static void repl_code_panel_draw_cut_icon(int kx, int ky, int kw, int kh) {
    /* Scissors: two 2px blades crossing in an X, tapering to pointed
     * tips at the top; handles diverge below the pivot into open
     * finger-loop rings that connect to the handle ends.
     *
     *  row 11:  .#.........#.   blade tips (pointed)
     *  row 10:  ..##.....##..
     *  row 9:   ...##...##...
     *  row 8:   ....##.##....
     *  row 7:   .....###.....
     *  row 6:   ......#......   pivot
     *  row 5:   .....#.#.....
     *  row 4:   ....#...#....   handles
     *  row 3:   ..###...###..   ring tops (join handles at cols 4 / 8)
     *  row 2:   .#...#.#...#.
     *  row 1:   .#...#.#...#.
     *  row 0:   ..###...###..   ring bottoms                          */
    static const GLubyte cut_bits[ACTION_ICON_H * 2] = {
        0x38, 0xE0,  /* row 0  */
        0x45, 0x10,  /* row 1  */
        0x45, 0x10,  /* row 2  */
        0x38, 0xE0,  /* row 3  */
        0x08, 0x80,  /* row 4  */
        0x05, 0x00,  /* row 5  */
        0x02, 0x00,  /* row 6  */
        0x07, 0x00,  /* row 7  */
        0x0D, 0x80,  /* row 8  */
        0x18, 0xC0,  /* row 9  */
        0x30, 0x60,  /* row 10 */
        0x40, 0x10   /* row 11 */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, cut_bits);
}

static void repl_code_panel_draw_paste_icon(int kx, int ky, int kw, int kh) {
    /* Clipboard with a clipped sheet: the top tab and shoulders identify
     * the clipboard, while the inset page lines distinguish paste from the
     * two loose sheets used by the copy glyph.
     *
     *  row 11:  ....#####....   clip top
     *  row 10:  ...#.....#...   clip sides
     *  row 9:   .###########.   board top / clip base
     *  row 8:   .#.........#.
     *  row 7:   .#..#####..#.   page text
     *  row 6:   .#.........#.
     *  row 5:   .#..#####..#.
     *  row 4:   .#.........#.
     *  row 3:   .#..#####..#.
     *  row 2:   .#.........#.
     *  row 1:   .#.........#.
     *  row 0:   .###########.   board bottom                         */
    static const GLubyte paste_bits[ACTION_ICON_H * 2] = {
        0x7F, 0xF0,  /* row 0  */
        0x40, 0x10,  /* row 1  */
        0x40, 0x10,  /* row 2  */
        0x4F, 0x90,  /* row 3  */
        0x40, 0x10,  /* row 4  */
        0x4F, 0x90,  /* row 5  */
        0x40, 0x10,  /* row 6  */
        0x4F, 0x90,  /* row 7  */
        0x40, 0x10,  /* row 8  */
        0x7F, 0xF0,  /* row 9  */
        0x10, 0x40,  /* row 10 */
        0x0F, 0x80   /* row 11 */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, paste_bits);
}

static void repl_code_panel_draw_select_all_icon(int kx, int ky, int kw, int kh) {
    /* 13x12 1bpp marquee: a dashed selection rectangle (the drag-box the
     * gutter draws) around three solid content lines, so it reads as
     * "everything inside the box is taken" next to the copy/cut/paste
     * sheets. Dashes run 2-on/1-off on all four sides; the content lines
     * are inset a pixel from the side rails. Rows bottom-to-top; bit
     * 0x80 of byte 0 = leftmost pixel (col 0).
     *
     *  row 11:  .............   pad
     *  row 10:  .##.##.##.##.   top rail (dashed)
     *  row 9:   .............   rail dash gap
     *  row 8:   .#.........#.   side rails
     *  row 7:   .#.#######.#.   rails + content line
     *  row 6:   .............   rail dash gap
     *  row 5:   .#.#######.#.   rails + content line
     *  row 4:   .#.........#.   side rails
     *  row 3:   ...#######...   content line (rail dash gap)
     *  row 2:   .#.........#.   side rails
     *  row 1:   .##.##.##.##.   bottom rail (dashed)
     *  row 0:   .............   pad                                     */
    static const GLubyte select_all_bits[ACTION_ICON_H * 2] = {
        0x00, 0x00,  /* row 0  pad          */
        0x6D, 0xB0,  /* row 1  bottom rail  */
        0x40, 0x10,  /* row 2  side rails   */
        0x1F, 0xC0,  /* row 3  content      */
        0x40, 0x10,  /* row 4  side rails   */
        0x5F, 0xD0,  /* row 5  rails+content*/
        0x00, 0x00,  /* row 6  dash gap     */
        0x5F, 0xD0,  /* row 7  rails+content*/
        0x40, 0x10,  /* row 8  side rails   */
        0x00, 0x00,  /* row 9  dash gap     */
        0x6D, 0xB0,  /* row 10 top rail     */
        0x00, 0x00   /* row 11 pad          */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H,
                                     select_all_bits);
}

static void repl_code_panel_draw_focus_icon(int kx, int ky, int kw, int kh) {
    /* 13x12 1bpp "focus frame" reticle: four L-shaped corner brackets
     * framing an empty centre, the camera/crop "reframe" glyph. It reads
     * as focusing on a region - apt for the code-focus toggle that hides
     * the surrounding boilerplate chrome. Frame spans cols 1-11, rows
     * 1-10 (1px pad all round); each arm is 3px. Rows bottom-to-top; bit
     * 0x80 of byte 0 = leftmost pixel (col 0).
     *
     *  row 10:  .###.....###.   top arms   (cols 1-3, 9-11)
     *  row 9:   .#.........#.   verticals
     *  row 8:   .#.........#.
     *  row 7:   .............
     *  ...             (open centre)
     *  row 3:   .............
     *  row 2:   .#.........#.   verticals
     *  row 1:   .#.........#.
     *  row 0/11 blank (pad)                                             */
    static const GLubyte focus_bits[ACTION_ICON_H * 2] = {
        0x00, 0x00,  /* row 0  pad          */
        0x40, 0x10,  /* row 1  verticals    */
        0x40, 0x10,  /* row 2  verticals    */
        0x70, 0x70,  /* row 3  bottom arms  */
        0x00, 0x00,  /* row 4               */
        0x00, 0x00,  /* row 5               */
        0x00, 0x00,  /* row 6               */
        0x00, 0x00,  /* row 7               */
        0x70, 0x70,  /* row 8  top arms     */
        0x40, 0x10,  /* row 9  verticals    */
        0x40, 0x10,  /* row 10 verticals    */
        0x00, 0x00   /* row 11 pad          */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, focus_bits);
}

static void repl_code_panel_draw_help_icon(int kx, int ky, int kw, int kh) {
    /* 13x12 1bpp question mark: a top arc sweeping down the right into a
     * tail that arrives at a centred stem, a gap, then the dot below.
     * Reads as "help". Rows bottom-to-top; bit 0x80 of byte 0 = col 0.
     *
     *  row 11:  ....#####....   arc top   (cols 4-8)
     *  row 10:  ...#.....#...   arc sides (cols 3, 9)
     *  row 9:   .........#...   right descent (col 9)
     *  row 8:   ........#....   curve in  (col 8)
     *  row 7:   .......#.....   curve in  (col 7)
     *  row 6:   ......#......   tail meets stem (col 6)
     *  row 5:   ......#......   stem
     *  row 4:   ......#......   stem
     *  row 3:   .............   gap
     *  row 2:   ......#......   dot       (col 6)
     *  row 1/0  blank                                                    */
    static const GLubyte help_bits[ACTION_ICON_H * 2] = {
        0x00, 0x00,  /* row 0             */
        0x00, 0x00,  /* row 1             */
        0x02, 0x00,  /* row 2  dot        */
        0x00, 0x00,  /* row 3  gap        */
        0x02, 0x00,  /* row 4  stem       */
        0x02, 0x00,  /* row 5  stem       */
        0x02, 0x00,  /* row 6  tail/stem  */
        0x01, 0x00,  /* row 7  curve      */
        0x00, 0x80,  /* row 8  curve      */
        0x00, 0x40,  /* row 9  descent    */
        0x10, 0x40,  /* row 10 arc sides  */
        0x0F, 0x80   /* row 11 arc top    */
    };
    repl_code_panel_draw_bitmap_icon(kx, ky, kw, kh,
                                     ACTION_ICON_W, ACTION_ICON_H, help_bits);
}

static void draw_undo_icon(const StatusbarSlot *slot, const StatusbarPrepared *p,
                           int x, int y, int w, int h) {
    (void)slot;
    ui_clr(p->active ? UI_TOK_TEXT_PRIMARY : UI_TOK_TEXT_MUTED);
    repl_code_panel_draw_undo_icon(x, y, w, h);
}

static void draw_redo_icon(const StatusbarSlot *slot, const StatusbarPrepared *p,
                           int x, int y, int w, int h) {
    (void)slot;
    ui_clr(p->active ? UI_TOK_TEXT_PRIMARY : UI_TOK_TEXT_MUTED);
    repl_code_panel_draw_redo_icon(x, y, w, h);
}

static void draw_select_all_icon(const StatusbarSlot *slot,
                                const StatusbarPrepared *p,
                                int x, int y, int w, int h) {
    (void)slot;
    ui_clr(p->active ? UI_TOK_TEXT_PRIMARY : UI_TOK_TEXT_MUTED);
    repl_code_panel_draw_select_all_icon(x, y, w, h);
}

static void draw_copy_icon(const StatusbarSlot *slot, const StatusbarPrepared *p,
                           int x, int y, int w, int h) {
    (void)slot; (void)p;
    ui_clr(UI_TOK_TEXT_PRIMARY);
    repl_code_panel_draw_copy_icon(x, y, w, h);
}

static void draw_cut_icon(const StatusbarSlot *slot, const StatusbarPrepared *p,
                          int x, int y, int w, int h) {
    (void)slot; (void)p;
    ui_clr(UI_TOK_TEXT_PRIMARY);
    repl_code_panel_draw_cut_icon(x, y, w, h);
}

static void draw_paste_icon(const StatusbarSlot *slot, const StatusbarPrepared *p,
                            int x, int y, int w, int h) {
    (void)slot; (void)p;
    ui_clr(UI_TOK_TEXT_PRIMARY);
    repl_code_panel_draw_paste_icon(x, y, w, h);
}

static void draw_trash_icon(const StatusbarSlot *slot, const StatusbarPrepared *p,
                            int x, int y, int w, int h) {
    (void)slot; (void)p;
    ui_clr(UI_TOK_STATUS_ERR);
    repl_code_panel_draw_trash_icon(x, y, w, h);
}

static void draw_focus_icon(const StatusbarSlot *slot, const StatusbarPrepared *p,
                            int x, int y, int w, int h) {
    (void)slot;
    ui_clr(p->active ? UI_TOK_ACCENT : UI_TOK_TEXT_PRIMARY);
    repl_code_panel_draw_focus_icon(x, y, w, h);
}

static void draw_help_icon(const StatusbarSlot *slot, const StatusbarPrepared *p,
                           int x, int y, int w, int h) {
    (void)slot;
    ui_clr(p->active ? UI_TOK_ACCENT : UI_TOK_TEXT_PRIMARY);
    repl_code_panel_draw_help_icon(x, y, w, h);
}

/*
 * Table order is visual order within an align cluster. Reordering Overlay
 * scope vs Vertex labels is moving two rows - do not restyle draw/hit.
 *
 * cull_rank is the overflow policy and is independent of that order:
 * poly(5) > vlabel(4) > scope(3) > aa_passes(2) > aa(1). A mid-width
 * panel drops PH before AA even though PH is last on screen.
 *
 * GLR_* macros expand to (key, mods), not a single id. Use KM_KEY /
 * KM_MODS. is_special=1 for GLUT_KEY_* (F1/F7/F8); Redo is Shift.
 */
static const StatusbarItem k_statusbar_items[] = {
    /* Left: perf-hint warning first so it outranks document stats under
     * scissor, then document stats. Never culled; overflow is scissored. */
    {
        .align = STATUSBAR_ALIGN_LEFT,
        .gap_before = STATUSBAR_GAP_NONE,
        .hit = UI_HIT_CODE_PERF_HINT,
        .tooltip = "Expensive setting is costing frames",
        .prepare = prepare_perf,
        .draw = draw_perf,
    },
    {
        .align = STATUSBAR_ALIGN_LEFT,
        .gap_before = STATUSBAR_GAP_PACK,
        .hit = UI_HIT_CODE_PERF_HINT_FIX,
        .tooltip = "Step the named setting back",
        .prepare = prepare_perf_fix,
        .draw = draw_perf_chip,
    },
    {
        .align = STATUSBAR_ALIGN_LEFT,
        .gap_before = STATUSBAR_GAP_PAIR,
        .hit = UI_HIT_CODE_PERF_HINT_DISMISS,
        .tooltip = "Hide until this setting changes or FPS recovers",
        .prepare = prepare_perf_dismiss,
        .draw = draw_perf_chip,
    },
    {
        .align = STATUSBAR_ALIGN_LEFT,
        .gap_before = STATUSBAR_GAP_SEP,
        .prepare = prepare_cmds,
        .draw = draw_cmds,
    },
    {
        .align = STATUSBAR_ALIGN_LEFT,
        .gap_before = STATUSBAR_GAP_SEP,
        .prepare = prepare_line,
        .draw = draw_muted_text,
    },
    {
        .align = STATUSBAR_ALIGN_LEFT,
        .gap_before = STATUSBAR_GAP_SEP,
        .prepare = prepare_func,
        .draw = draw_muted_text,
    },
    {
        .align = STATUSBAR_ALIGN_LEFT,
        .gap_before = STATUSBAR_GAP_SEP,
        .prepare = prepare_unbal,
        .draw = draw_unbal,
    },
    {
        .align = STATUSBAR_ALIGN_LEFT,
        .gap_before = STATUSBAR_GAP_SEP,
        .prepare = prepare_cost,
        .draw = draw_cost,
    },

    /* Center: state readouts. Centered in the panel so a cursor-cost
     * change on the left cannot shift them. */
    {
        .align = STATUSBAR_ALIGN_CENTER,
        .gap_before = STATUSBAR_GAP_NONE,
        .hit = UI_HIT_CODE_AA_STATUS,
        .tooltip = "Accum effect",
        .key_code = KM_KEY(GLR_ACCUM_EFFECT),
        .modifiers = KM_MODS(GLR_ACCUM_EFFECT),
        .cull_rank = 1,
        .prepare = prepare_aa,
        .draw = draw_state_text,
    },
    {
        .align = STATUSBAR_ALIGN_CENTER,
        .gap_before = STATUSBAR_GAP_PACK,
        .hit = UI_HIT_CODE_AA_PASSES_STATUS,
        .tooltip = "Accum passes",
        .cull_rank = 2,
        .prepare = prepare_aa_passes,
        .draw = draw_state_text,
    },
    {
        .align = STATUSBAR_ALIGN_CENTER,
        .gap_before = STATUSBAR_GAP_SEP,
        .hit = UI_HIT_CODE_OVERLAY_SCOPE_STATUS,
        .tooltip = "Overlay scope",
        .key_code = KM_KEY(GLR_OVERLAY_SCOPE),
        .modifiers = KM_MODS(GLR_OVERLAY_SCOPE),
        .is_special = 1,
        .group_id = STATUSBAR_GROUP_ID_OVERLAY,
        .is_group_host = 1,
        .cull_rank = 3,
        .prepare = prepare_scope,
        .draw = draw_state_text,
    },
    {
        .align = STATUSBAR_ALIGN_CENTER,
        .gap_before = STATUSBAR_GAP_SEP,
        .hit = UI_HIT_CODE_VERTEX_LABELS_STATUS,
        .tooltip = "Vertex labels",
        .key_code = KM_KEY(GLR_VERTEX_LABELS),
        .modifiers = KM_MODS(GLR_VERTEX_LABELS),
        .is_special = 1,
        .group_id = STATUSBAR_GROUP_ID_OVERLAY,
        .cull_rank = 4,
        .prepare = prepare_vlabel,
        .draw = draw_state_text,
    },
    {
        .align = STATUSBAR_ALIGN_CENTER,
        .gap_before = STATUSBAR_GAP_SEP,
        .hit = UI_HIT_CODE_POLY_HIGHLIGHT_STATUS,
        .tooltip = "Polygon highlight",
        .key_code = KM_KEY(GLR_POLY_HIGHLIGHT),
        .modifiers = KM_MODS(GLR_POLY_HIGHLIGHT),
        .group_id = STATUSBAR_GROUP_ID_OVERLAY,
        .cull_rank = 5,
        .prepare = prepare_poly,
        .draw = draw_state_text,
    },

    /* Right: editor chips, packed from the panel's right edge. Positions
     * are final at place time; collision hide never slides survivors. */
    {
        .align = STATUSBAR_ALIGN_RIGHT,
        .gap_before = STATUSBAR_GAP_NONE,
        .hit = UI_HIT_CODE_UNDO,
        .tooltip = "Undo",
        .key_code = KM_KEY(GLR_UNDO),
        .modifiers = KM_MODS(GLR_UNDO),
        .has_keycap = 1,
        .prepare = prepare_undo,
        .draw = draw_undo_icon,
    },
    {
        .align = STATUSBAR_ALIGN_RIGHT,
        .gap_before = STATUSBAR_GAP_PAIR,
        .hit = UI_HIT_CODE_REDO,
        .tooltip = "Redo",
        .key_code = KM_KEY(GLR_REDO),
        .modifiers = KM_MODS(GLR_REDO),
        .has_keycap = 1,
        .prepare = prepare_redo,
        .draw = draw_redo_icon,
    },
    {
        .align = STATUSBAR_ALIGN_RIGHT,
        .gap_before = STATUSBAR_GAP_GROUP,
        .hit = UI_HIT_CODE_SELECT_ALL,
        /* No keymap slot free (Ctrl+A is Line start, Ctrl+Shift+A is
         * Audio), so this is the label-only tooltip form. */
        .tooltip = "Select all",
        .has_keycap = 1,
        .prepare = prepare_select_all,
        .draw = draw_select_all_icon,
    },
    {
        .align = STATUSBAR_ALIGN_RIGHT,
        .gap_before = STATUSBAR_GAP_PAIR,
        .hit = UI_HIT_CODE_COPY,
        .tooltip = "Copy",
        .key_code = KM_KEY(GLR_COPY),
        .modifiers = KM_MODS(GLR_COPY),
        .has_keycap = 1,
        .prepare = prepare_always,
        .draw = draw_copy_icon,
    },
    {
        .align = STATUSBAR_ALIGN_RIGHT,
        .gap_before = STATUSBAR_GAP_PAIR,
        .hit = UI_HIT_CODE_CUT,
        .tooltip = "Cut",
        .key_code = KM_KEY(GLR_CUT),
        .modifiers = KM_MODS(GLR_CUT),
        .has_keycap = 1,
        .prepare = prepare_always,
        .draw = draw_cut_icon,
    },
    {
        .align = STATUSBAR_ALIGN_RIGHT,
        .gap_before = STATUSBAR_GAP_PAIR,
        .hit = UI_HIT_CODE_PASTE,
        .tooltip = "Paste",
        .key_code = KM_KEY(GLR_PASTE),
        .modifiers = KM_MODS(GLR_PASTE),
        .has_keycap = 1,
        .prepare = prepare_always,
        .draw = draw_paste_icon,
    },
    {
        .align = STATUSBAR_ALIGN_RIGHT,
        .gap_before = STATUSBAR_GAP_GROUP,
        .hit = UI_HIT_CODE_CLEAR_ALL,
        .tooltip = "Clear all",
        .has_keycap = 1,
        .prepare = prepare_always,
        .draw = draw_trash_icon,
    },
    {
        .align = STATUSBAR_ALIGN_RIGHT,
        .gap_before = STATUSBAR_GAP_CLUSTER,
        .hit = UI_HIT_CODE_FOCUS_TOGGLE,
        .tooltip = "Code focus",
        .key_code = KM_KEY(GLR_CODE_FOCUS),
        .modifiers = KM_MODS(GLR_CODE_FOCUS),
        .has_keycap = 1,
        .prepare = prepare_focus,
        .draw = draw_focus_icon,
    },
    {
        .align = STATUSBAR_ALIGN_RIGHT,
        .gap_before = STATUSBAR_GAP_CLUSTER,
        .hit = UI_HIT_HELP_TOGGLE,
        .tooltip = "Help",
        .key_code = KM_KEY(GLR_HELP),
        .modifiers = KM_MODS(GLR_HELP),
        .is_special = 1,
        .has_keycap = 1,
        .prepare = prepare_help,
        .draw = draw_help_icon,
    },
};

STATIC_ASSERT(ARRAY_LEN(k_statusbar_items) <= STATUSBAR_ITEM_MAX,
              "k_statusbar_items exceeds STATUSBAR_ITEM_MAX");

static int statusbar_center_width(const StatusbarLayout *L, const int *keep) {
    int first = 1;
    int width = 0;

    for (int i = 0; i < L->count; i++) {
        if (k_statusbar_items[i].align != STATUSBAR_ALIGN_CENTER)
            continue;
        if (!keep[i])
            continue;
        if (!first)
            width += statusbar_gap_px(k_statusbar_items[i].gap_before);
        width += L->items[i].w;
        first = 0;
    }
    return width;
}

static int statusbar_highest_cull(const StatusbarLayout *L, const int *keep) {
    int best = -1;
    int best_rank = -1;

    for (int i = 0; i < L->count; i++) {
        if (k_statusbar_items[i].align != STATUSBAR_ALIGN_CENTER)
            continue;
        if (!keep[i])
            continue;
        if (k_statusbar_items[i].cull_rank > best_rank) {
            best_rank = k_statusbar_items[i].cull_rank;
            best = i;
        }
    }
    return best;
}

/* One solve, three consumers (draw / hit / tooltip). Order is load-bearing:
 *  1. prepare every item (eligible / text / width)
 *  2. pack left (eligible items stay visible; overflow is scissored)
 *  3. place eligible right chips from the right edge - positions are final
 *  4. fit the center cluster; drop highest cull_rank until it sits in
 *     (left_edge + gap, undo_x - gap)
 *  5. hide right chips whose already-assigned box reaches left_bound
 *     Do not re-pack: Help/Focus must keep the same x when Undo hides.
 */
static StatusbarLayout statusbar_solve(const UiRenderSnapshot *snap,
                                       int sx, int sy, int sw, int sh) {
    StatusbarLayout L;
    StatusbarSlot slot;
    int left_edge;
    int right_limit;
    int left_bound;
    int keep[STATUSBAR_ITEM_MAX];
    int n = (int)ARRAY_LEN(k_statusbar_items);

    memset(&L, 0, sizeof L);
    L.text_y = sy + (sh - FONT_SMALL_H) / 2 + 1;
    /* Inset so the top/bottom divider grab band is not swallowed. */
    L.ky = sy + 3;
    L.kh = sh - 6;
    L.count = n;

    slot.snap = snap;
    slot.sx = sx;
    slot.sy = sy;
    slot.sw = sw;
    slot.sh = sh;
    slot.text_y = L.text_y;
    slot.ky = L.ky;
    slot.kh = L.kh;

    for (int i = 0; i < n; i++) {
        L.items[i].item_idx = i;
        L.items[i].y = L.ky;
        L.items[i].h = L.kh;
        L.items[i].visible = 0;
        k_statusbar_items[i].prepare(&slot, &L.items[i].prep);
        /* Keycaps are square-ish (kh+4); text items measure their label. */
        if (k_statusbar_items[i].has_keycap)
            L.items[i].w = L.kh + 4;
        else
            L.items[i].w = L.items[i].prep.width;
    }

    {
        int cursor = sx + CODE_MARGIN_X;
        int first = 1;
        for (int i = 0; i < n; i++) {
            if (k_statusbar_items[i].align != STATUSBAR_ALIGN_LEFT)
                continue;
            if (!L.items[i].prep.eligible)
                continue;
            if (!first)
                cursor += statusbar_gap_px(k_statusbar_items[i].gap_before);
            L.items[i].x = cursor;
            L.items[i].visible = 1;
            cursor += L.items[i].w;
            first = 0;
        }
        left_edge = cursor;
    }

    /* Walk the table right-to-left so Help (last) is rightmost. Each
     * item's gap_before is the space to its left *eligible* neighbor.
     * Ineligible chips reserve no space and stay invisible. */
    {
        int cursor = sx + sw - CODE_MARGIN_X;
        int undo_x = sx + sw;
        for (int i = n - 1; i >= 0; i--) {
            if (k_statusbar_items[i].align != STATUSBAR_ALIGN_RIGHT)
                continue;
            if (!L.items[i].prep.eligible)
                continue;
            L.items[i].x = cursor - L.items[i].w;
            if (L.items[i].x < undo_x)
                undo_x = L.items[i].x;
            cursor = L.items[i].x - statusbar_gap_px(k_statusbar_items[i].gap_before);
        }
        /* Same job as the old statusbar_undo_kx: center must not cover
         * the editor chips, so it treats the full right cluster as reserved. */
        right_limit = undo_x - FONT_SMALL_W;
    }

    for (int i = 0; i < n; i++)
        keep[i] = (k_statusbar_items[i].align == STATUSBAR_ALIGN_CENTER &&
                   L.items[i].prep.eligible);

    {
        int gap = FONT_SMALL_W;
        int left_limit = left_edge + gap;
        int start_x = 0;
        int end_x = 0;
        int width;
        int count;

        for (;;) {
            width = statusbar_center_width(&L, keep);
            count = 0;
            for (int i = 0; i < n; i++) {
                if (keep[i])
                    count++;
            }
            if (count == 0) {
                start_x = end_x = 0;
                break;
            }
            /* Centered in the panel, not after the left cluster, so a
             * cost readout appearing on the left cannot shift AA/OS/VL. */
            start_x = sx + (sw - width) / 2;
            end_x = start_x + width;
            if (start_x >= left_limit && end_x <= right_limit)
                break;
            /* Prefer hiding a readout over hiding an editor chip. */
            int drop = statusbar_highest_cull(&L, keep);
            if (drop < 0)
                break;
            keep[drop] = 0;
        }

        if (count > 0) {
            int cx = start_x;
            int first = 1;
            for (int i = 0; i < n; i++) {
                if (!keep[i])
                    continue;
                if (!first)
                    cx += statusbar_gap_px(k_statusbar_items[i].gap_before);
                L.items[i].x = cx;
                L.items[i].visible = 1;
                cx += L.items[i].w;
                first = 0;
            }
            left_bound = end_x;
        } else {
            left_bound = left_edge;
        }
    }

    /* Collision hide only. Re-packing would slide Help left and fail
     * the 360 px left-layout test (help + focus stay put). */
    for (int i = 0; i < n; i++) {
        if (k_statusbar_items[i].align != STATUSBAR_ALIGN_RIGHT)
            continue;
        if (!L.items[i].prep.eligible) {
            L.items[i].visible = 0;
            continue;
        }
        L.items[i].visible = (L.items[i].x >= left_bound + FONT_SMALL_W);
    }

    return L;
}

/* Keycaps win on any overlap with a text readout. They should not
 * overlap after collision hide; this is the backstop. Hit boxes use
 * ky/kh, not the full strip, so the divider grab band stays free. */
static int statusbar_hover_idx(const StatusbarLayout *L, int mx, int gl_y) {
    int hit = -1;

    for (int i = 0; i < L->count; i++) {
        if (!L->items[i].visible)
            continue;
        if (k_statusbar_items[i].hit == UI_HIT_NONE)
            continue;
        if (!statusbar_point_in_rect(mx, gl_y,
                                     L->items[i].x, L->ky,
                                     L->items[i].w, L->kh))
            continue;
        if (k_statusbar_items[i].has_keycap)
            return i;
        if (hit < 0)
            hit = i;
    }
    return hit;
}

static int statusbar_hit_from_layout(const StatusbarLayout *L, int mx, int gl_y) {
    int idx = statusbar_hover_idx(L, mx, gl_y);
    if (idx < 0)
        return UI_HIT_CODE_PANEL_CHROME;
    return k_statusbar_items[idx].hit;
}

/* Asymmetric on purpose: hovering Overlay Scope (the host) lights
 * OS+VL+PH; hovering VL or PH does not. Do not special-case the hit
 * kind - is_group_host is the rule. */
static int statusbar_group_band_span(const StatusbarLayout *L, int hover_idx,
                                     int *out_x0, int *out_x1) {
    if (hover_idx < 0)
        return 0;
    if (!k_statusbar_items[hover_idx].is_group_host)
        return 0;
    int gid = k_statusbar_items[hover_idx].group_id;
    if (gid == 0)
        return 0;

    int x0 = 0;
    int x1 = 0;
    for (int i = 0; i < L->count; i++) {
        if (!L->items[i].visible)
            continue;
        if (k_statusbar_items[i].group_id != gid)
            continue;
        if (x1 == 0) {
            x0 = L->items[i].x;
            x1 = L->items[i].x + L->items[i].w;
        } else {
            if (L->items[i].x < x0)
                x0 = L->items[i].x;
            if (L->items[i].x + L->items[i].w > x1)
                x1 = L->items[i].x + L->items[i].w;
        }
    }
    if (x1 <= x0)
        return 0;
    if (out_x0) *out_x0 = x0;
    if (out_x1) *out_x1 = x1;
    return 1;
}

static void statusbar_draw_tooltip(const UiRenderSnapshot *snap,
                                   const StatusbarLayout *L,
                                   int hover_idx,
                                   int cp_x, int cp_w, int sy, int sh) {
    const StatusbarItem *item;
    char text[KEYMAP_SHORTCUT_LABEL_MAX + 32];
    int tw;
    int th = FONT_SMALL_H + 8;
    int tx;
    int ty = sy + sh + 5;
    int min_x = cp_x + 4;
    int max_x;

    if (hover_idx < 0)
        return;
    item = &k_statusbar_items[hover_idx];
    if (L->items[hover_idx].prep.tooltip[0] != '\0')
        snprintf(text, sizeof text, "%s", L->items[hover_idx].prep.tooltip);
    else if (!item->tooltip)
        return;
    else if (item->key_code != 0) {
        char shortcut[KEYMAP_SHORTCUT_LABEL_MAX];
        keymap_binding_to_string(shortcut, (int)sizeof shortcut,
                                 item->key_code, item->modifiers,
                                 item->is_special);
        snprintf(text, sizeof text, "%s %c %s", item->tooltip,
                 TEXT_PANEL_VRULE_GLYPH, shortcut);
    } else {
        snprintf(text, sizeof text, "%s", item->tooltip);
    }

    tw = (int)strlen(text) * FONT_SMALL_W + 12;
    tx = L->items[hover_idx].x + (L->items[hover_idx].w - tw) / 2;
    max_x = cp_x + cp_w - tw - 4;
    if (max_x < min_x)
        return;
    if (tx < min_x)
        tx = min_x;
    if (tx > max_x)
        tx = max_x;

    ui_clr_a(UI_TOK_RAISED, 0.98f);
    glRectf((float)tx, (float)ty, (float)(tx + tw), (float)(ty + th));
    ui_clr(UI_TOK_BORDER);
    glBegin(GL_LINE_LOOP);
    glVertex2f((float)tx + 0.5f, (float)ty + 0.5f);
    glVertex2f((float)(tx + tw) - 0.5f, (float)ty + 0.5f);
    glVertex2f((float)(tx + tw) - 0.5f, (float)(ty + th) - 0.5f);
    glVertex2f((float)tx + 0.5f, (float)(ty + th) - 0.5f);
    glEnd();
    ui_clr(UI_TOK_TEXT_PRIMARY);
    gl2d_draw_string((float)(tx + 6), (float)(ty + 4), text, FONT_SMALL);
}

void repl_code_panel_statusbar_draw(const UiRenderSnapshot *snap,
                                    const UiTextPanelRect *slot) {
    if (!snap || !slot || slot->h <= 0)
        return;

    int sy = slot->y;
    int sh = slot->h;
    int cp_x = slot->x;
    int cp_w = slot->w;
    StatusbarLayout L = statusbar_solve(snap, cp_x, sy, cp_w, sh);
    StatusbarSlot draw_slot;
    draw_slot.snap = snap;
    draw_slot.sx = cp_x;
    draw_slot.sy = sy;
    draw_slot.sw = cp_w;
    draw_slot.sh = sh;
    draw_slot.text_y = L.text_y;
    draw_slot.ky = L.ky;
    draw_slot.kh = L.kh;

    glPushAttrib(GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Clip items to the strip so a narrow split-layout panel cannot
     * paint left-cluster text onto the 3D scene. The tooltip sits at
     * sy+sh+5 - disable scissor before drawing it. */
    if (cp_w > 0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(cp_x, sy, cp_w, sh);
    }

    ui_clr_a(UI_TOK_SURFACE, 0.98f);
    glRectf((float)cp_x, (float)sy,
            (float)(cp_x + cp_w), (float)(sy + sh));
    glColor4f(0.0f, 0.0f, 0.0f, 1.0f); /* hard top edge of the strip */
    glBegin(GL_LINES);
    glVertex2f((float)cp_x, (float)(sy + sh));
    glVertex2f((float)(cp_x + cp_w), (float)(sy + sh));
    glEnd();

    int gl_y = snap->viewport.window_h - snap->pointer.mouse_y;
    int hover_idx = statusbar_hover_idx(&L, snap->pointer.mouse_x, gl_y);

    /* Band first so every group member draws over it. */
    int gx0, gx1;
    if (statusbar_group_band_span(&L, hover_idx, &gx0, &gx1))
        statusbar_group_band(gx0, gx1, L.ky, L.kh);

    prof_begin(PROF_CODE_PANEL_OVERLAY_STATUS_TEXT);
    for (int i = 0; i < L.count; i++) {
        if (!L.items[i].visible)
            continue;
        if (k_statusbar_items[i].align == STATUSBAR_ALIGN_RIGHT)
            continue;
        if (k_statusbar_items[i].gap_before == STATUSBAR_GAP_SEP) {
            int has_prev = 0;
            for (int j = 0; j < i; j++) {
                if (k_statusbar_items[j].align != k_statusbar_items[i].align)
                    continue;
                if (L.items[j].visible)
                    has_prev = 1;
            }
            if (has_prev)
                statusbar_draw_sep_at(L.items[i].x - STATUSBAR_SEP_W / 2,
                                      sy, sh);
        }
        k_statusbar_items[i].draw(&draw_slot, &L.items[i].prep,
                                  L.items[i].x, L.items[i].y,
                                  L.items[i].w, L.items[i].h);
    }
    prof_end(PROF_CODE_PANEL_OVERLAY_STATUS_TEXT);

    /* Keycaps first, then glyphs, so consecutive glBitmap calls stay
     * batched for gl4es. Mixing glRect into the glyph pass breaks that. */
    prof_begin(PROF_CODE_PANEL_OVERLAY_STATUS_ACTIONS);
    for (int i = 0; i < L.count; i++) {
        if (!L.items[i].visible || !k_statusbar_items[i].has_keycap)
            continue;
        repl_code_panel_draw_keycap(L.items[i].x, L.items[i].y,
                                    L.items[i].w, L.items[i].h);
    }
    for (int i = 0; i < L.count; i++) {
        if (!L.items[i].visible || !k_statusbar_items[i].has_keycap)
            continue;
        k_statusbar_items[i].draw(&draw_slot, &L.items[i].prep,
                                  L.items[i].x, L.items[i].y,
                                  L.items[i].w, L.items[i].h);
    }
    prof_end(PROF_CODE_PANEL_OVERLAY_STATUS_ACTIONS);

    glDisable(GL_SCISSOR_TEST);
    statusbar_draw_tooltip(snap, &L, hover_idx, cp_x, cp_w, sy, sh);
    glDisable(GL_BLEND);
    glPopAttrib();
}

int repl_code_panel_statusbar_hit_kind(const UiRenderSnapshot *snap,
                                       const UiTextPanelSnapshot *text_snap,
                                       int mx, int gl_y) {
    StatusbarLayout L;

    if (!snap || !text_snap)
        return UI_HIT_NONE;
    /* Use the same reserved height render wrote into the text snapshot
     * so a future STATUSBAR_H change cannot split the two paths. */
    L = statusbar_solve(snap, text_snap->cp_x, text_snap->cp_y,
                        text_snap->cp_w,
                        text_snap->statusbar_h > 0
                            ? text_snap->statusbar_h : STATUSBAR_H);
    return statusbar_hit_from_layout(&L, mx, gl_y);
}

int repl_code_panel_statusbar_group_band_for_test(
    const UiRenderSnapshot *snap, int sx, int sy, int sw, int sh,
    int *out_x0, int *out_x1) {
    StatusbarLayout L;
    int gl_y;
    int hover_idx;

    if (!snap)
        return 0;
    L = statusbar_solve(snap, sx, sy, sw, sh);
    gl_y = snap->viewport.window_h - snap->pointer.mouse_y;
    hover_idx = statusbar_hover_idx(&L, snap->pointer.mouse_x, gl_y);
    return statusbar_group_band_span(&L, hover_idx, out_x0, out_x1);
}

int repl_code_panel_statusbar_cull_rank_for_test(int hit) {
    int n = (int)ARRAY_LEN(k_statusbar_items);

    for (int i = 0; i < n; i++) {
        if (k_statusbar_items[i].hit == (UiHitKind)hit)
            return k_statusbar_items[i].cull_rank;
    }
    return -1;
}
