/*
 * src/repl/attrib_bits.c - glPushAttrib/glPopAttrib attribute-bit mapping.
 *
 * See attrib_bits.h for the contract. Pure: no GL calls, no live-GL reads.
 * The collectors read the live REPL *source* document (repl_state_document_*)
 * the same way the bracket matchers in autonormal.c do.
 */
#include <string.h>

#include "repl/attrib_bits.h"
#include "repl/command_spec.h"  /* repl_attrib_bit_entries() */
#include "repl/state_views.h"   /* repl_state_document_cmds/_count (read-only) */

/* --- Atomic state-cell identity --------------------------------------------
 *
 * Each cell is a 32-bit id: (kind << 16) | key. One kind per state family; the
 * key disambiguates within a family (cap enum, face x property, plane index,
 * light-model pname). A cell's covering GL_*_BIT mask is a pure function of its
 * identity (cell_cover below) and matches the attrib_bits repl_attrib_cmd_writes
 * stamps on it — the two must agree so a push saves exactly the cells a pop
 * restores. */
enum {
    ITEM_KIND_COLOR = 1,   /* current color        (CURRENT) */
    ITEM_KIND_NORMAL,      /* current normal       (CURRENT) */
    ITEM_KIND_RASTER,      /* current raster pos   (CURRENT) */
    ITEM_KIND_EDGE,        /* current edge flag    (CURRENT) */
    ITEM_KIND_CAP,         /* enable state; key = cap enum (ENABLE | group) */
    ITEM_KIND_MATERIAL,    /* key = face_idx*8 + prop_idx  (LIGHTING) */
    ITEM_KIND_CM_CONFIG,   /* glColorMaterial face+mode    (LIGHTING) */
    ITEM_KIND_SHADE,       /* glShadeModel mode            (LIGHTING) */
    ITEM_KIND_LIGHT_MODEL, /* key = pname                  (LIGHTING) */
    ITEM_KIND_POINT_SIZE,  /* (POINT) */
    ITEM_KIND_POINT_PARAM, /* (POINT) */
    ITEM_KIND_LINE_WIDTH,  /* (LINE) */
    ITEM_KIND_LINE_STIPPLE,/* (LINE) */
    ITEM_KIND_FRONT_FACE,  /* (POLYGON) */
    ITEM_KIND_CULL_FACE,   /* (POLYGON) */
    ITEM_KIND_DEPTH_FUNC,  /* (DEPTH_BUFFER) */
    ITEM_KIND_DEPTH_MASK,  /* (DEPTH_BUFFER) */
    ITEM_KIND_BLEND_FUNC,  /* (COLOR_BUFFER) */
    ITEM_KIND_COLOR_MASK,  /* (COLOR_BUFFER) */
    ITEM_KIND_CLEAR_COLOR, /* (COLOR_BUFFER) */
    ITEM_KIND_CLIP_PLANE,  /* key = plane idx; equation  (TRANSFORM) */
    ITEM_KIND_FOG          /* key = pname (mode/density/start/end/color)  (FOG) */
};

#define ITEM_ID(kind_, key_) (((unsigned)(kind_) << 16) | ((unsigned)(key_) & 0xFFFFu))

/* Which attribute group a capability's enable state belongs to (in addition to
 * GL_ENABLE_BIT, which saves every enable). GL_MULTISAMPLE and any unknown cap
 * map to 0 -> GL_ENABLE_BIT only (GL_MULTISAMPLE_BIT is unsupported). */
static unsigned cap_group_bit(GLenum cap) {
    switch (cap) {
    case GL_LIGHTING:
    case GL_LIGHT0: case GL_LIGHT1: case GL_LIGHT2: case GL_LIGHT3:
    case GL_COLOR_MATERIAL:
        return GL_LIGHTING_BIT;
    case GL_LINE_SMOOTH: case GL_LINE_STIPPLE:
        return GL_LINE_BIT;
    case GL_POINT_SMOOTH:
        return GL_POINT_BIT;
    case GL_CULL_FACE:
        return GL_POLYGON_BIT;
    case GL_DEPTH_TEST:
        return GL_DEPTH_BUFFER_BIT;
    case GL_FOG:
        return GL_FOG_BIT;
    case GL_BLEND:
        return GL_COLOR_BUFFER_BIT;
    case GL_CLIP_PLANE0: case GL_CLIP_PLANE1: case GL_CLIP_PLANE2:
    case GL_CLIP_PLANE3: case GL_CLIP_PLANE4: case GL_CLIP_PLANE5:
    case GL_NORMALIZE:
        return GL_TRANSFORM_BIT;
    default:
        return 0;
    }
}

/* Covering GL_*_BIT mask of a cell, from its identity alone. */
static unsigned cell_cover(unsigned item_id) {
    unsigned kind = item_id >> 16;
    unsigned key  = item_id & 0xFFFFu;
    switch (kind) {
    case ITEM_KIND_COLOR: case ITEM_KIND_NORMAL:
    case ITEM_KIND_RASTER: case ITEM_KIND_EDGE:
        return GL_CURRENT_BIT;
    case ITEM_KIND_CAP:
        return GL_ENABLE_BIT | cap_group_bit((GLenum)key);
    case ITEM_KIND_MATERIAL: case ITEM_KIND_CM_CONFIG:
    case ITEM_KIND_SHADE: case ITEM_KIND_LIGHT_MODEL:
        return GL_LIGHTING_BIT;
    case ITEM_KIND_POINT_SIZE: case ITEM_KIND_POINT_PARAM:
        return GL_POINT_BIT;
    case ITEM_KIND_LINE_WIDTH: case ITEM_KIND_LINE_STIPPLE:
        return GL_LINE_BIT;
    case ITEM_KIND_FRONT_FACE: case ITEM_KIND_CULL_FACE:
        return GL_POLYGON_BIT;
    case ITEM_KIND_DEPTH_FUNC: case ITEM_KIND_DEPTH_MASK:
        return GL_DEPTH_BUFFER_BIT;
    case ITEM_KIND_BLEND_FUNC: case ITEM_KIND_COLOR_MASK:
    case ITEM_KIND_CLEAR_COLOR:
        return GL_COLOR_BUFFER_BIT;
    case ITEM_KIND_CLIP_PLANE:
        return GL_TRANSFORM_BIT;
    case ITEM_KIND_FOG:
        return GL_FOG_BIT;
    default:
        return 0;
    }
}

static unsigned clip_plane_idx(GLenum plane) {
    if (plane >= GL_CLIP_PLANE0 && plane <= GL_CLIP_PLANE5)
        return (unsigned)(plane - GL_CLIP_PLANE0);
    return 0;
}

/* --- Public: coarse mask + bit index --------------------------------------- */

unsigned repl_attrib_bits_for_cmd(const GLCmd *cmd) {
    if (!cmd)
        return 0;
    switch (cmd->type) {
    case CMD_COLOR3F: case CMD_COLOR4F:
    case CMD_RASTER_POS3F: case CMD_NORMAL3F: case CMD_EDGE_FLAG:
        return GL_CURRENT_BIT;
    case CMD_ENABLE: case CMD_DISABLE:
        return GL_ENABLE_BIT | cap_group_bit((GLenum)cmd->args[0]);
    case CMD_SHADE_MODEL: case CMD_LIGHT_MODEL_I: case CMD_COLOR_MATERIAL:
    case CMD_MATERIALFV: case CMD_MATERIALF:
        return GL_LIGHTING_BIT;
    case CMD_LINE_WIDTH: case CMD_LINE_STIPPLE:
        return GL_LINE_BIT;
    case CMD_POINT_SIZE: case CMD_POINT_PARAMETER_FV:
        return GL_POINT_BIT;
    case CMD_CULL_FACE: case CMD_FRONT_FACE:
        return GL_POLYGON_BIT;
    case CMD_DEPTH_FUNC: case CMD_DEPTH_MASK:
        return GL_DEPTH_BUFFER_BIT;
    case CMD_BLEND_FUNC: case CMD_COLOR_MASK: case CMD_CLEAR_COLOR:
        return GL_COLOR_BUFFER_BIT;
    case CMD_CLIP_PLANE:
        return GL_TRANSFORM_BIT;
    case CMD_FOG_I: case CMD_FOG_F: case CMD_FOG_FV:
        return GL_FOG_BIT;
    default:
        return 0;
    }
}

unsigned repl_attrib_bits_for_type(CmdType type, unsigned enum_arg0) {
    GLCmd probe;
    memset(&probe, 0, sizeof(probe));
    probe.type = type;
    probe.num_args = 1;
    probe.args[0] = (float)enum_arg0;
    return repl_attrib_bits_for_cmd(&probe);
}

int repl_attrib_bit_index(unsigned single_bit) {
    const ReplEnumEntry *bits = repl_attrib_bit_entries();
    for (int i = 0; bits[i].name; i++)
        if ((unsigned)bits[i].value == single_bit)
            return i;
    return -1;
}

int repl_attrib_bit_token_spans(const char *text, ReplAttribTokenSpan *out,
                                int max) {
    const ReplEnumEntry *bits = repl_attrib_bit_entries();
    const char *open;
    const char *close;
    int n = 0;

    if (!text || !out || max <= 0)
        return 0;
    open = strchr(text, '(');
    close = open ? strchr(open, ')') : NULL;
    if (!open || !close)
        return 0;

    for (int i = 0; bits[i].name && n < max; i++) {
        const char *hit = strstr(open + 1, bits[i].name);
        if (!hit || hit >= close)
            continue;
        out[n++] = (ReplAttribTokenSpan){
            .char_start = (int)(hit - text),
            .char_end = (int)(hit - text) + (int)strlen(bits[i].name),
            .bit_idx = i,
        };
    }
    return n;
}

/* --- Public: per-command cell writes (flow-sensitive) ---------------------- */

void repl_attrib_flow_init(ReplAttribFlowState *flow) {
    if (!flow)
        return;
    flow->color_material_enabled = 0;
    flow->color_material_face = GL_FRONT_AND_BACK;
    flow->color_material_mode = GL_AMBIENT_AND_DIFFUSE;
}

/* Expand a material face/property enum to its atomic indices
 * (GL_FRONT_AND_BACK -> {front, back}; GL_AMBIENT_AND_DIFFUSE -> {ambient,
 * diffuse}). Returns the count (0..2). */
static int expand_face(GLenum face, int out[2]) {
    switch (face) {
    case GL_FRONT:          out[0] = 0; return 1;
    case GL_BACK:           out[0] = 1; return 1;
    case GL_FRONT_AND_BACK: out[0] = 0; out[1] = 1; return 2;
    default:                out[0] = 0; out[1] = 1; return 2;
    }
}

static int expand_prop(GLenum prop, int out[2]) {
    switch (prop) {
    case GL_AMBIENT:              out[0] = 0; return 1;
    case GL_DIFFUSE:             out[0] = 1; return 1;
    case GL_SPECULAR:            out[0] = 2; return 1;
    case GL_EMISSION:            out[0] = 3; return 1;
    case GL_SHININESS:           out[0] = 4; return 1;
    case GL_AMBIENT_AND_DIFFUSE: out[0] = 0; out[1] = 1; return 2;
    default:                     return 0;
    }
}

static int emit_material_cells(GLenum face, GLenum prop,
                               ReplAttribCellWrite *out, int n, int max) {
    int fi[2], pi[2];
    int nf = expand_face(face, fi);
    int np = expand_prop(prop, pi);
    for (int a = 0; a < nf; a++) {
        for (int b = 0; b < np; b++) {
            if (n >= max)
                return n;
            out[n].item_id = ITEM_ID(ITEM_KIND_MATERIAL, fi[a] * 8 + pi[b]);
            out[n].attrib_bits = GL_LIGHTING_BIT;
            n++;
        }
    }
    return n;
}

static int emit_one(ReplAttribCellWrite *out, int n, int max,
                    unsigned item_id, unsigned bits) {
    if (n >= max)
        return n;
    out[n].item_id = item_id;
    out[n].attrib_bits = bits;
    return n + 1;
}

int repl_attrib_cmd_writes(const GLCmd *cmd, ReplAttribFlowState *flow,
                           ReplAttribCellWrite out[REPL_ATTRIB_MAX_ITEMS_PER_CMD]) {
    ReplAttribFlowState local;
    const int MAX = REPL_ATTRIB_MAX_ITEMS_PER_CMD;
    int n = 0;

    if (!cmd || !out)
        return 0;
    if (!flow) {
        repl_attrib_flow_init(&local);
        flow = &local;
    }

    switch (cmd->type) {
    case CMD_COLOR3F: case CMD_COLOR4F:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_COLOR, 0), GL_CURRENT_BIT);
        if (flow->color_material_enabled)
            n = emit_material_cells(flow->color_material_face,
                                    flow->color_material_mode, out, n, MAX);
        break;
    case CMD_NORMAL3F:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_NORMAL, 0), GL_CURRENT_BIT);
        break;
    case CMD_RASTER_POS3F:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_RASTER, 0), GL_CURRENT_BIT);
        break;
    case CMD_EDGE_FLAG:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_EDGE, 0), GL_CURRENT_BIT);
        break;
    case CMD_ENABLE:
    case CMD_DISABLE: {
        GLenum cap = (GLenum)cmd->args[0];
        int enabled = (cmd->type == CMD_ENABLE);
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_CAP, cap),
                     GL_ENABLE_BIT | cap_group_bit(cap));
        if (cap == GL_COLOR_MATERIAL) {
            flow->color_material_enabled = enabled;
            /* Turning color material on makes this line the latest writer of
             * the material cells it now drives. */
            if (enabled)
                n = emit_material_cells(flow->color_material_face,
                                        flow->color_material_mode, out, n, MAX);
        }
        break;
    }
    case CMD_COLOR_MATERIAL: {
        GLenum face = (GLenum)cmd->args[0];
        GLenum mode = (GLenum)cmd->args[1];
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_CM_CONFIG, 0), GL_LIGHTING_BIT);
        flow->color_material_face = face;
        flow->color_material_mode = mode;
        /* Reconfiguring while enabled re-selects (and re-writes) the material
         * cells the new face/mode drives. */
        if (flow->color_material_enabled)
            n = emit_material_cells(face, mode, out, n, MAX);
        break;
    }
    case CMD_MATERIALFV:
    case CMD_MATERIALF:
        n = emit_material_cells((GLenum)cmd->args[0], (GLenum)cmd->args[1],
                                out, n, MAX);
        break;
    case CMD_SHADE_MODEL:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_SHADE, 0), GL_LIGHTING_BIT);
        break;
    case CMD_LIGHT_MODEL_I:
        n = emit_one(out, n, MAX,
                     ITEM_ID(ITEM_KIND_LIGHT_MODEL, (GLenum)cmd->args[0]),
                     GL_LIGHTING_BIT);
        break;
    case CMD_LINE_WIDTH:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_LINE_WIDTH, 0), GL_LINE_BIT);
        break;
    case CMD_LINE_STIPPLE:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_LINE_STIPPLE, 0), GL_LINE_BIT);
        break;
    case CMD_POINT_SIZE:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_POINT_SIZE, 0), GL_POINT_BIT);
        break;
    case CMD_POINT_PARAMETER_FV:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_POINT_PARAM, 0), GL_POINT_BIT);
        break;
    case CMD_FRONT_FACE:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_FRONT_FACE, 0), GL_POLYGON_BIT);
        break;
    case CMD_CULL_FACE:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_CULL_FACE, 0), GL_POLYGON_BIT);
        break;
    case CMD_DEPTH_FUNC:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_DEPTH_FUNC, 0),
                     GL_DEPTH_BUFFER_BIT);
        break;
    case CMD_DEPTH_MASK:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_DEPTH_MASK, 0),
                     GL_DEPTH_BUFFER_BIT);
        break;
    case CMD_BLEND_FUNC:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_BLEND_FUNC, 0),
                     GL_COLOR_BUFFER_BIT);
        break;
    case CMD_COLOR_MASK:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_COLOR_MASK, 0),
                     GL_COLOR_BUFFER_BIT);
        break;
    case CMD_CLEAR_COLOR:
        n = emit_one(out, n, MAX, ITEM_ID(ITEM_KIND_CLEAR_COLOR, 0),
                     GL_COLOR_BUFFER_BIT);
        break;
    case CMD_CLIP_PLANE:
        n = emit_one(out, n, MAX,
                     ITEM_ID(ITEM_KIND_CLIP_PLANE, clip_plane_idx((GLenum)cmd->args[0])),
                     GL_TRANSFORM_BIT);
        break;
    case CMD_FOG_I:
    case CMD_FOG_F:
    case CMD_FOG_FV:
        /* Each fog parameter is its own cell, keyed by pname (GL_FOG_MODE /
         * GL_FOG_DENSITY / GL_FOG_START / GL_FOG_END / GL_FOG_COLOR), so
         * setting one does not supersede another. All ride GL_FOG_BIT. */
        n = emit_one(out, n, MAX,
                     ITEM_ID(ITEM_KIND_FOG, (GLenum)cmd->args[0]), GL_FOG_BIT);
        break;
    default:
        break;
    }
    return n;
}

/* --- Masked attribute-stack fold + collectors ------------------------------
 *
 * The collectors fold the source document with real nested LIFO
 * attribute-stack semantics over the block-unaware source-order policy the
 * indentation model and collect_unbalanced already use. File-scoped scratch
 * (like collect_unbalanced's stacks) keeps the fold allocation-free. */

#define FOLD_MAX_CELLS 80   /* > the ~55 distinct atomic cells */
#define FOLD_MAX_DEPTH 64   /* nesting past this degrades to plain balancing */

typedef struct {
    unsigned item_id;
    int      writer_line;   /* -1 = no writer yet */
} FoldCell;

typedef struct {
    unsigned            mask;
    int                 push_line;
    ReplAttribFlowState flow_saved;
    int                 count;
    FoldCell            saved[FOLD_MAX_CELLS];
} FoldFrame;

typedef struct {
    FoldCell            cells[FOLD_MAX_CELLS];
    int                 cell_count;
    FoldFrame           frames[FOLD_MAX_DEPTH];
    int                 depth;
    ReplAttribFlowState flow;
} Fold;

static Fold g_fold;

static FoldCell *fold_cell(Fold *f, unsigned item_id) {
    for (int i = 0; i < f->cell_count; i++)
        if (f->cells[i].item_id == item_id)
            return &f->cells[i];
    if (f->cell_count >= FOLD_MAX_CELLS)
        return NULL;
    FoldCell *c = &f->cells[f->cell_count++];
    c->item_id = item_id;
    c->writer_line = -1;
    return c;
}

static void fold_apply(Fold *f, const GLCmd *cmd, int line) {
    ReplAttribCellWrite w[REPL_ATTRIB_MAX_ITEMS_PER_CMD];
    int nw = repl_attrib_cmd_writes(cmd, &f->flow, w);
    for (int i = 0; i < nw; i++) {
        FoldCell *c = fold_cell(f, w[i].item_id);
        if (c)
            c->writer_line = line;
    }
}

static void fold_push(Fold *f, unsigned mask, int line) {
    if (f->depth < FOLD_MAX_DEPTH) {
        FoldFrame *fr = &f->frames[f->depth];
        fr->mask = mask;
        fr->push_line = line;
        fr->flow_saved = f->flow;
        fr->count = 0;
        for (int i = 0; i < f->cell_count; i++) {
            if ((cell_cover(f->cells[i].item_id) & mask) &&
                fr->count < FOLD_MAX_CELLS) {
                fr->saved[fr->count++] = f->cells[i];
            }
        }
    }
    f->depth++;
}

/* Look up a cell's saved writer within a frame; -1 if it was untouched at
 * push time (and thus must be reset to "no writer" on restore). */
static int frame_saved_writer(const FoldFrame *fr, unsigned item_id) {
    for (int i = 0; i < fr->count; i++)
        if (fr->saved[i].item_id == item_id)
            return fr->saved[i].writer_line;
    return -1;
}

static void fold_pop(Fold *f) {
    if (f->depth <= 0)
        return;                 /* orphan pop: ignore, matching executor safety */
    f->depth--;
    if (f->depth >= FOLD_MAX_DEPTH)
        return;                 /* overflow push had no frame */
    FoldFrame *fr = &f->frames[f->depth];
    for (int i = 0; i < f->cell_count; i++) {
        if (!(cell_cover(f->cells[i].item_id) & fr->mask))
            continue;
        f->cells[i].writer_line = frame_saved_writer(fr, f->cells[i].item_id);
    }
    /* Restore only the flow the frame's mask actually saved (color-material
     * enable rides ENABLE|LIGHTING; its face/mode ride LIGHTING). */
    if (fr->mask & (GL_ENABLE_BIT | GL_LIGHTING_BIT))
        f->flow.color_material_enabled = fr->flow_saved.color_material_enabled;
    if (fr->mask & GL_LIGHTING_BIT) {
        f->flow.color_material_face = fr->flow_saved.color_material_face;
        f->flow.color_material_mode = fr->flow_saved.color_material_mode;
    }
}

/* Fold source lines [0, end) into g_fold. */
static void fold_run(Fold *f, const GLCmd *cmds, int count, int end) {
    memset(f, 0, sizeof(*f));
    repl_attrib_flow_init(&f->flow);
    if (end > count)
        end = count;
    for (int i = 0; i < end; i++) {
        if (!cmds[i].valid)
            continue;
        if (cmds[i].type == CMD_PUSH_ATTRIB)
            fold_push(f, (unsigned)cmds[i].args[0], i);
        else if (cmds[i].type == CMD_POP_ATTRIB)
            fold_pop(f);
        else
            fold_apply(f, &cmds[i], i);
    }
}

/* GL_*_BIT mask -> union of canonical bit-index bits (1 << idx). */
static unsigned bits_to_idx_mask(unsigned bits) {
    unsigned m = 0;
    const ReplEnumEntry *tbl = repl_attrib_bit_entries();
    for (int i = 0; tbl[i].name; i++)
        if (bits & (unsigned)tbl[i].value)
            m |= (1u << i);
    return m;
}

static void add_highlight(ReplAttribHighlightLine *out, int max, int *n,
                          int line, unsigned idx_mask) {
    for (int i = 0; i < *n; i++) {
        if (out[i].line_idx == line) {
            out[i].bit_idx_mask |= idx_mask;
            return;
        }
    }
    if (*n < max) {
        out[*n].line_idx = line;
        out[*n].bit_idx_mask = idx_mask;
        (*n)++;
    }
}

int repl_attrib_collect_push_saved(int push_line,
                                   ReplAttribHighlightLine *out, int max) {
    int count = repl_state_document_count();
    const GLCmd *cmds;
    unsigned mask;
    int n = 0;

    if (push_line < 0 || push_line >= count || !out || max <= 0)
        return 0;
    cmds = repl_state_document_cmds();
    if (!cmds || !cmds[push_line].valid ||
        cmds[push_line].type != CMD_PUSH_ATTRIB)
        return 0;
    mask = (unsigned)cmds[push_line].args[0];

    fold_run(&g_fold, cmds, count, push_line);  /* lines 0..push_line-1 */

    /* Emit the current writer of every cell the target push mask selects.
     * Completed inner pairs before the push have already restored their cells,
     * so their setters cannot leak in. */
    for (int i = 0; i < g_fold.cell_count; i++) {
        unsigned cover;
        if (g_fold.cells[i].writer_line < 0)
            continue;
        cover = cell_cover(g_fold.cells[i].item_id) & mask;
        if (!cover)
            continue;
        add_highlight(out, max, &n, g_fold.cells[i].writer_line,
                      bits_to_idx_mask(cover));
    }
    return n;
}

int repl_attrib_collect_pop_reverted(int pop_line,
                                     ReplAttribHighlightLine *out, int max) {
    int count = repl_state_document_count();
    const GLCmd *cmds;
    const FoldFrame *fr;
    unsigned mask;
    int push_line, n = 0;

    if (pop_line < 0 || pop_line >= count || !out || max <= 0)
        return 0;
    cmds = repl_state_document_cmds();
    if (!cmds || !cmds[pop_line].valid ||
        cmds[pop_line].type != CMD_POP_ATTRIB)
        return 0;

    fold_run(&g_fold, cmds, count, pop_line);   /* lines 0..pop_line-1 */
    if (g_fold.depth <= 0 || g_fold.depth > FOLD_MAX_DEPTH)
        return 0;                               /* orphan pop / overflow */
    fr = &g_fold.frames[g_fold.depth - 1];      /* matching push, by LIFO */
    mask = fr->mask;
    push_line = fr->push_line;

    /* Emit the body setter whose cell the pop reverts: covered by the push
     * mask, current writer differs from the saved writer, and lives strictly
     * inside the push/pop body (so overwritten setters and setters an inner
     * pop already reverted are not highlighted). */
    for (int i = 0; i < g_fold.cell_count; i++) {
        unsigned cover = cell_cover(g_fold.cells[i].item_id) & mask;
        int cur = g_fold.cells[i].writer_line;
        int saved;
        if (!cover)
            continue;
        saved = frame_saved_writer(fr, g_fold.cells[i].item_id);
        if (cur == saved)
            continue;
        if (cur <= push_line || cur >= pop_line)
            continue;
        add_highlight(out, max, &n, cur, bits_to_idx_mask(cover));
    }
    return n;
}
