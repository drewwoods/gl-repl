# Plan: Explicit GLU Tessellator Commands

## Context

Currently `glBegin(GL_POLYGON)` silently hijacks the GLU tessellator in `execute_commands()` - the vertex callback uses only `glVertex3dv`, discarding per-vertex normal/color data, and the combine callback ignores interpolation. The user wants explicit tessellator commands with full per-vertex data (position + normal + color), a weighted combine callback, and removal of the implicit tessellator hijack so `glBegin(GL_POLYGON)` goes back to a plain GL call.

## New REPL Syntax → Execution Mapping

| User types | REPL executes |
|------------|--------------|
| `gluBegin(GLU_POLYGON)` | reset pool; `gluTessBeginPolygon(g_tess, NULL)` |
| `gluBegin(GLU_CONTOUR)` | `gluTessBeginContour(g_tess)` |
| `gluEnd()` | `gluTessEndContour` or `gluTessEndPolygon` (depth-tracked stack) |
| `gluNormal(x,y,z)` | store in local `tess_current_normal[3]` |
| `gluColor(r,g,b[,a])` | store in local `tess_current_color[4]` |
| `gluVertex(x,y,z)` | alloc `TessVertex` from pool, fill pos/normal/color, call `gluTessVertex` |

## Files to Modify

| File | Changes |
|------|---------|
| `sample.h` | Add `TessVertex` struct; 6 new `CmdType` values; update `g_tess_verts` extern |
| `sample.c` | Global def, `tess_vertex_callback`, updated `tess_combine_callback`, `init_gl`, `parse_command_internal`, `execute_commands`, `save_output`, `g_func_completions` |
| `ui_panels.c` | `color_for_type()` - new cases for 6 new types |

---

## Step 1 - sample.h

### 1a. Add `TessVertex` struct (after `TESS_VERT_BUF_SIZE` macro):
```c
typedef struct {
    GLdouble pos[3];
    GLdouble normal[3]; /* per-vertex normal, default (0,0,1) */
    GLdouble color[4];  /* per-vertex RGBA,   default (1,1,1,1) */
} TessVertex;
```

### 1b. Add 6 new `CmdType` values (before `CMD_TYPE_COUNT`):
```c
CMD_TESS_BEGIN_POLYGON,
CMD_TESS_BEGIN_CONTOUR,
CMD_TESS_END,
CMD_TESS_NORMAL,
CMD_TESS_COLOR,
CMD_TESS_VERTEX,
```

### 1c. Update extern declaration:
```c
// OLD: extern GLdouble g_tess_verts[TESS_VERT_BUF_SIZE][3];
extern TessVertex g_tess_verts[TESS_VERT_BUF_SIZE];
```

---

## Step 2 - sample.c: globals & callbacks

### 2a. Change global definition:
```c
// OLD: GLdouble g_tess_verts[TESS_VERT_BUF_SIZE][3];
TessVertex g_tess_verts[TESS_VERT_BUF_SIZE];
```

### 2b. Add `tess_vertex_callback` (new, before `tess_combine_callback`):
```c
static void tess_vertex_callback(void *vertex_data) {
    TessVertex *v = (TessVertex *)vertex_data;
    glNormal3dv(v->normal);
    glColor4dv(v->color);
    glVertex3dv(v->pos);
}
```

### 2c. Update `tess_combine_callback` - interpolate normal & color with weights:
```c
static void tess_combine_callback(GLdouble coords[3], void *vertex_data[4],
                                   GLfloat weight[4], void **out_data) {
    if (g_tess_vert_count >= TESS_VERT_BUF_SIZE) { *out_data = NULL; return; }
    TessVertex *v = &g_tess_verts[g_tess_vert_count++];
    v->pos[0] = coords[0]; v->pos[1] = coords[1]; v->pos[2] = coords[2];
    for (int c = 0; c < 3; c++) v->normal[c] = 0.0;
    for (int c = 0; c < 4; c++) v->color[c]  = 0.0;
    for (int j = 0; j < 4; j++) {
        if (!vertex_data[j]) continue;
        TessVertex *src = (TessVertex *)vertex_data[j];
        for (int c = 0; c < 3; c++) v->normal[c] += weight[j] * src->normal[c];
        for (int c = 0; c < 4; c++) v->color[c]  += weight[j] * src->color[c];
    }
    /* Renormalize interpolated normal */
    double len = sqrt(v->normal[0]*v->normal[0] + v->normal[1]*v->normal[1]
                    + v->normal[2]*v->normal[2]);
    if (len > 1e-9) { v->normal[0]/=len; v->normal[1]/=len; v->normal[2]/=len; }
    *out_data = v;
}
```

### 2d. Update `init_gl` - swap vertex callback:
```c
// OLD: gluTessCallback(g_tess, GLU_TESS_VERTEX, (void (*)())glVertex3dv);
gluTessCallback(g_tess, GLU_TESS_VERTEX, (void (*)())tess_vertex_callback);
```

---

## Step 3 - sample.c: parse_command_internal

Add 6 new function-name handlers, placed after the `glutSolidTorus` block and before the `goto` block:

| Input | Parsed as | args[] |
|-------|-----------|--------|
| `gluBegin(GLU_POLYGON)` | CMD_TESS_BEGIN_POLYGON | - |
| `gluBegin(GLU_CONTOUR)` | CMD_TESS_BEGIN_CONTOUR | - |
| `gluEnd()` | CMD_TESS_END | - |
| `gluNormal(x,y,z)` | CMD_TESS_NORMAL | [0-2] = xyz |
| `gluColor(r,g,b[,a])` | CMD_TESS_COLOR | [0-3] = rgba (a defaults to 1) |
| `gluVertex(x,y,z)` | CMD_TESS_VERTEX | [0-2] = xyz |

Source strings (use `indent` variable already computed in the parser):
- `gluBegin(GLU_POLYGON)` → `"  gluBegin(GLU_POLYGON);"`
- `gluBegin(GLU_CONTOUR)` → `"    gluBegin(GLU_CONTOUR);"`
- `gluEnd()` → `"%sgluEnd();"` with `indent`
- `gluNormal/gluColor/gluVertex` → `"%sgluNormal(%g, %g, %g);"` etc. with `indent`

`gluColor` accepts 3 or 4 args; if 3, `args[3] = 1.0f`.

---

## Step 4 - sample.c: execute_commands

### 4a. Remove implicit tessellator:
- Remove `static GLdouble tess_buf[256][3];` local
- Remove `int tess_n = 0;` and `int in_polygon = 0;` locals
- In `CMD_BEGIN`: remove `if (mode == GL_POLYGON && g_tess)` branch - use plain `glBegin` always
- In `CMD_END`: remove `in_polygon` branch - keep only `glEnd(); in_begin = 0;`
- In `CMD_VERTEX3F`: remove `in_polygon` branch
- At `execute_done`: remove tess cleanup for in_polygon

### 4b. Add new locals at top of execute_commands:
```c
int tess_depth = 0; /* 0=outside, 1=in polygon, 2=in contour */
GLdouble tess_current_normal[3] = {0.0, 0.0, 1.0};
GLdouble tess_current_color[4]  = {1.0, 1.0, 1.0, 1.0};
```

### 4c. Add new switch cases (before `CMD_LABEL`):
```c
case CMD_TESS_BEGIN_POLYGON:
    if (in_begin) { glEnd(); in_begin = 0; }
    if (g_tess) { g_tess_vert_count = 0; gluTessBeginPolygon(g_tess, NULL); tess_depth = 1; }
    break;
case CMD_TESS_BEGIN_CONTOUR:
    if (g_tess && tess_depth == 1) { gluTessBeginContour(g_tess); tess_depth = 2; }
    break;
case CMD_TESS_END:
    if (g_tess && tess_depth == 2) { gluTessEndContour(g_tess); tess_depth = 1; }
    else if (g_tess && tess_depth == 1) { gluTessEndPolygon(g_tess); tess_depth = 0; }
    break;
case CMD_TESS_NORMAL:
    tess_current_normal[0] = g_flat_cmds[pc].args[0];
    tess_current_normal[1] = g_flat_cmds[pc].args[1];
    tess_current_normal[2] = g_flat_cmds[pc].args[2];
    break;
case CMD_TESS_COLOR:
    tess_current_color[0] = g_flat_cmds[pc].args[0];
    tess_current_color[1] = g_flat_cmds[pc].args[1];
    tess_current_color[2] = g_flat_cmds[pc].args[2];
    tess_current_color[3] = (g_flat_cmds[pc].num_args >= 4) ? g_flat_cmds[pc].args[3] : 1.0;
    break;
case CMD_TESS_VERTEX:
    if (g_tess && tess_depth == 2 && g_tess_vert_count < TESS_VERT_BUF_SIZE) {
        TessVertex *v = &g_tess_verts[g_tess_vert_count++];
        v->pos[0] = g_flat_cmds[pc].args[0];
        v->pos[1] = g_flat_cmds[pc].args[1];
        v->pos[2] = g_flat_cmds[pc].args[2];
        memcpy(v->normal, tess_current_normal, sizeof(v->normal));
        memcpy(v->color,  tess_current_color,  sizeof(v->color));
        gluTessVertex(g_tess, v->pos, v);
    }
    break;
```

### 4d. Add to `execute_done` cleanup:
```c
if (tess_depth == 2 && g_tess) { gluTessEndContour(g_tess); tess_depth = 1; }
if (tess_depth == 1 && g_tess) { gluTessEndPolygon(g_tess); }
```

---

## Step 5 - sample.c: save_output

### 5a. Detect tess usage before snippet:
```c
int has_tess = 0;
for (int i = 0; i < g_num_cmds; i++)
    if (g_cmds[i].valid && g_cmds[i].type >= CMD_TESS_BEGIN_POLYGON
                        && g_cmds[i].type <= CMD_TESS_VERTEX)
        has_tess = 1;
```

### 5b. If `has_tess`, emit tess preamble (before `// Snippet start`):

Emit `TessVertex` typedef, static pool + state globals, `_tess_vtx_cb`, `_tess_comb_cb`, and a `__attribute__((constructor))` function that creates/configures `g_tess` and registers the callbacks. Emit `static GLUtesselator *g_tess = NULL;` as an additional static global.

### 5c. Track `int save_tess_depth = 0` in the main command-writing loop. Add cases:

```c
case CMD_TESS_BEGIN_POLYGON:
    fprintf(f, "  { _tv_n=0; gluTessBeginPolygon(g_tess,NULL); }\n");
    save_tess_depth = 1; break;
case CMD_TESS_BEGIN_CONTOUR:
    fprintf(f, "    gluTessBeginContour(g_tess);\n");
    save_tess_depth = 2; break;
case CMD_TESS_END:
    if (save_tess_depth == 2) {
        fprintf(f, "    gluTessEndContour(g_tess);\n"); save_tess_depth = 1;
    } else {
        fprintf(f, "  gluTessEndPolygon(g_tess);\n"); save_tess_depth = 0;
    } break;
case CMD_TESS_NORMAL:
    fprintf(f, "      { _tn[0]=%g; _tn[1]=%g; _tn[2]=%g; }\n",
            g_cmds[i].args[0], g_cmds[i].args[1], g_cmds[i].args[2]); break;
case CMD_TESS_COLOR:
    fprintf(f, "      { _tc[0]=%g; _tc[1]=%g; _tc[2]=%g; _tc[3]=%g; }\n",
            g_cmds[i].args[0], g_cmds[i].args[1], g_cmds[i].args[2], g_cmds[i].args[3]); break;
case CMD_TESS_VERTEX:
    fprintf(f, "      { TessVertex *_v=&_tv[_tv_n++];"
               " _v->pos[0]=%g;_v->pos[1]=%g;_v->pos[2]=%g;"
               " memcpy(_v->normal,_tn,24); memcpy(_v->color,_tc,32);"
               " gluTessVertex(g_tess,_v->pos,_v); }\n",
            g_cmds[i].args[0], g_cmds[i].args[1], g_cmds[i].args[2]); break;
```

---

## Step 6 - sample.c: g_func_completions

Add to array:
```c
"gluBegin(GLU_POLYGON)",
"gluBegin(GLU_CONTOUR)",
"gluEnd()",
"gluNormal(",
"gluColor(",
"gluVertex(",
```

---

## Step 7 - ui_panels.c: color_for_type

Add to switch:
```c
case CMD_TESS_BEGIN_POLYGON:
case CMD_TESS_BEGIN_CONTOUR:
case CMD_TESS_END:          glColor3f(0.70f, 0.55f, 0.90f); break; /* violet */
case CMD_TESS_NORMAL:       glColor3f(0.40f, 0.80f, 0.95f); break; /* cyan (same as CMD_NORMAL3F) */
case CMD_TESS_COLOR:        glColor3f(0.95f, 0.85f, 0.30f); break; /* yellow (same as CMD_COLOR3F) */
case CMD_TESS_VERTEX:       glColor3f(0.40f, 0.90f, 0.40f); break; /* green (same as CMD_VERTEX3F) */
```

---

## Verification

1. `make clean && make` - zero errors/warnings
2. In REPL, enter a concave polygon:
   ```
   gluBegin(GLU_POLYGON);
   gluBegin(GLU_CONTOUR);
   gluNormal(0, 0, 1);
   gluColor(1, 0.5, 0, 1);
   gluVertex(-1, -1, 0);
   gluVertex(1, -1, 0);
   gluVertex(0.5, 0.5, 0);
   gluVertex(-0.5, 0.5, 0);
   gluEnd();
   gluEnd();
   ```
   Expect: orange tessellated polygon, no infinite loop, correct normal
3. Verify syntax colors: tess begin/end show violet, gluVertex green, gluNormal cyan, gluColor yellow
4. Press Ctrl+S → `output.c` compiles standalone: `glut_compile output.c`
5. Verify `glBegin(GL_POLYGON)` still works as a plain GL call (no tessellator hijacking)
6. Test combine callback: non-convex shape forcing tessellation - interpolated vertices should carry weighted normal/color
