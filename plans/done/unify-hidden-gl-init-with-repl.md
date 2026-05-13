# Unify Visible Startup Bootstrap with Live `init_gl`

## Context

The code panel footer's `init()` body and the runtime `init_gl()` function drift
apart today because they are maintained independently:

- **Runtime `init_gl()`** (`repl_core.c:7794-7818`) calls:
  - `glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE)`
  - `glLightModelfv(GL_LIGHT_MODEL_AMBIENT, {0.15, 0.15, 0.20, 1.0})` *(host-only, no REPL cmd exists for float-array light model)*
  - `glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE)`
  - `gluNewQuadric()` + `gluQuadricNormals(SMOOTH)` + `gluQuadricTexture(FALSE)`
  - `gluNewTess()` + 5 `gluTessCallback(...)` wires

- **Static footer `g_footer[]`** (`repl_core.c:268-314`, init body at 293-298) hard-codes an `init()` body that is **incomplete**:
  - Missing `glLightModelfv` ambient setup
  - Missing `gluQuadricTexture(GL_FALSE)`
  - Missing all tessellator setup
  - Missing the new point-attenuation default

Users reading the visible footer see something that does not match what the program
actually runs at startup, which defeats the "less magic" design intent. The goal is
**one source of truth** for scriptable startup GL state, plus **one generator** that
renders the `init()` body consistently in the code panel footer, the `--dump-code`
text dump, and `repl_save_output()`.

Scriptable GL state commands (`CMD_COLOR_MATERIAL`, `CMD_LIGHT_MODEL_I`,
`CMD_POINT_PARAMETER_FV`, …) are already handled by the switch inside
`execute_commands()` (`repl_core.c:5136+`), so runtime application of parsed
bootstrap commands only needs a small refactor: extract the state-only subset into
a shared `apply_state_cmd()` helper that both `execute_commands()` and
`init_gl()` can call.

## Scope

### Bootstrap list contents (first pass)

```
glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1.0, 0.0, 0.02);  // gated by toggle
```

The third line is **new** (not in today's `init_gl()`). Per user decision, it ships as
a default but is **gated by a new `CfgItem` toggle** so users can opt out.

### Included

1. `g_init_bootstrap_repl[]` - an array of `{ repl_line, optional toggle pointer }`
   entries in `repl_core.c`, near `g_footer[]`.
2. Parse bootstrap lines once at startup into `g_init_bootstrap_cmds[]` (separate
   from `g_cmds[]`; never seeded into the user buffer). Abort loudly if any line
   fails to parse - these are developer-authored.
3. Refactor `init_gl()` to:
   - Host-only plumbing first (`glLightModelfv` ambient, quadric alloc/config, tess
     alloc/callbacks).
   - Then walk `g_init_bootstrap_cmds[]` and call `apply_state_cmd(&cmd)` for each
     entry whose toggle is enabled (or NULL).
4. Extract `apply_state_cmd(GLCmd*)` from `execute_commands()` covering
   `CMD_COLOR_MATERIAL`, `CMD_LIGHT_MODEL_I`, `CMD_POINT_PARAMETER_FV`,
   `CMD_SHADE_MODEL`, `CMD_FRONT_FACE`, `CMD_ENABLE`, `CMD_DISABLE`,
   `CMD_BLEND_FUNC`, `CMD_MATERIALF`. `execute_commands()` calls the same helper
   for those cases so there is zero behavior divergence.
5. Split `g_footer[]` into `g_footer_pre_init[]` and `g_footer_post_init[]`; delete
   the old hard-coded init body (`repl_core.c:293-298`). Add a parallel static list
   `g_init_host_only_c[]` with the host-only init lines as literal C strings.
6. Emit helpers (single source used by export, panel render, dump):
   - `init_section_line_count(void)` - returns `len(g_init_host_only_c) +
     enabled bootstrap cmd count`.
   - `init_section_line(int i, char *buf, size_t n)` - fills `buf` with the nth
     generated line (host-only lines first, then bootstrap lines formatted via
     existing `write_cmd_source_as_c()`-style formatting into a buffer).
   - `emit_init_section_to_file(FILE *f)` - writes the same lines to a `FILE*`.
7. Wire `save_output()` (`repl_core.c:2961-2962`) and code panel footer rendering
   (`ui_panels.c:564`, `~1349`) to iterate `g_footer_pre_init` → init section lines
   → `g_footer_post_init`.
8. New `CfgItem`:
   - Label: `"Point attenuation"`
   - Backing var: `int g_init_attenuate_points = 1;` (default ON)
   - Max value: `2` (off/on)
   - On toggle change, re-run the bootstrap apply pass so live GL state matches
     (idempotent - applying `glPointParameterfv` with 1,0,0 disables attenuation;
     applying the original values re-enables it). Simplest implementation: wrap the
     bootstrap walk in `apply_init_bootstrap(void)` and call it from both
     `init_gl()` and the config toggle hook.
9. Tests - see below.

### Explicitly out of scope

- No new REPL commands. `glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ...)` stays host-only,
  emitted into the generated `init()` body as one of the `g_init_host_only_c[]`
  literal strings.
- No change to user-facing save/load format. The `init()` section lives in footer
  scaffolding only; the snippet markers and `g_cmds[]` contents are untouched.
- Tess export preamble (`write_tess_preamble`, `repl_core.c:1794-1831`) stays
  host-only generated C - callback wiring cannot be expressed in REPL format.
- Init body is **visible-only, permanent**: no UI hooks to edit bootstrap entries
  from the running program. Developer-authored only.
- No changes to `g_header_pre/post`, `g_render_state_lines`, or `g_lookat`.

## Key Files

| File | Changes |
|------|--------|
| `repl_core.c` | Add `g_init_bootstrap_repl[]`, `g_init_bootstrap_cmds[]`, `g_init_host_only_c[]`, `g_init_attenuate_points`. Split `g_footer[]` into pre/post init halves. Refactor `init_gl()` (7794) and `save_output()` (2865). Add `parse_init_bootstrap()`, `apply_state_cmd()`, `apply_init_bootstrap()`, `emit_init_section_to_file()`, `init_section_line_count()`, `init_section_line()`. Add new `CfgItem` entry in `g_cfg_items[]` (~line 663 per CLAUDE.md). |
| `ui_panels.c` | `code_panel_footer_row_count()` (`~564`) and footer render loop (`~1349`) iterate pre-init lines, generated init section, post-init lines. |
| `sample.h` | Extern declarations for `g_init_attenuate_points`, `init_section_line_count`, `init_section_line`. |
| `test_repl_core_io.c` | New assertions on emitted init() body contents with toggle on/off. |
| `test_repl_core_examples.c` | Unchanged - export/compile coverage continues to validate nothing broke. |

## Existing Helpers to Reuse

| Helper | Location | Why |
|--------|----------|-----|
| `parse_command()` | `repl_core.c` (wrapped as `repl_parse_command`) | Parses one REPL line into a `GLCmd`. Used once at startup for each bootstrap line. |
| `write_cmd_source_as_c()` | `repl_core.c:1631` | Formats a `GLCmd` as a C source line. The emit helpers call a buffer-returning variant or `fprintf` into a memstream to reuse it for both file and in-memory emission. |
| Existing switch in `execute_commands()` | `repl_core.c:5136+` | Source of the state-only branches that move into `apply_state_cmd()`. Keep `execute_commands()` calling the same helper so no drift. |
| `code_panel_row_count_for_text()` | `ui_panels.c:406` | Wrap-aware row counting, used by the updated footer row counter. |
| Existing `CfgItem` pattern + `g_cfg_items[]` | `repl_core.c:~663` | Drop-in for the new toggle; no UI work needed. |

## Implementation Steps

1. **Declare bootstrap data.**
   ```c
   typedef struct {
       const char *repl_line;
       const int  *toggle;   /* NULL = always enabled */
   } InitBootstrapEntry;

   int g_init_attenuate_points = 1;

   static const InitBootstrapEntry g_init_bootstrap_repl[] = {
       { "glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);", NULL },
       { "glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);",            NULL },
       { "glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, 1.0, 0.0, 0.02);",
         &g_init_attenuate_points },
   };
   #define NUM_INIT_BOOTSTRAP ((int)(sizeof(g_init_bootstrap_repl)/sizeof(g_init_bootstrap_repl[0])))
   static GLCmd g_init_bootstrap_cmds[NUM_INIT_BOOTSTRAP];

   static const char *g_init_host_only_c[] = {
       "  GLfloat lm_amb[] = { 0.15f, 0.15f, 0.20f, 1.0f };",
       "  glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);",
       "  g_quadric = gluNewQuadric();",
       "  gluQuadricNormals(g_quadric, GLU_SMOOTH);",
       "  gluQuadricTexture(g_quadric, GL_FALSE);",
       NULL,
   };
   ```

2. **Add `parse_init_bootstrap(void)`** - called from `repl_init_gl()` before
   anything else. Loops over `g_init_bootstrap_repl[]`, calls `parse_command()` on
   each `repl_line`, stores result in `g_init_bootstrap_cmds[i]`, and `fprintf(stderr,
   ...)` + `abort()` on parse failure (developer error).

3. **Extract `apply_state_cmd(const GLCmd *c)`.** Move the state-only branches of
   the `execute_commands()` switch (`repl_core.c:5181-5236` roughly) into a helper.
   Leave vertex/geometry/transform branches in place. In `execute_commands()`,
   replace those branches with `apply_state_cmd(&g_flat_cmds[pc]); break;`.

4. **Add `apply_init_bootstrap(void)`** - walks `g_init_bootstrap_cmds[]`, skipping
   entries whose `toggle` pointer is non-NULL and points to zero, and calls
   `apply_state_cmd(&cmd)` for the rest.

5. **Refactor `init_gl()`** (`repl_core.c:7794-7818`) to the new shape:
   ```c
   static void init_gl(void) {
       /* host-only plumbing */
       GLfloat lm_amb[] = { 0.15f, 0.15f, 0.20f, 1.0f };
       glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lm_amb);
       g_quadric = gluNewQuadric();
       gluQuadricNormals(g_quadric, GLU_SMOOTH);
       gluQuadricTexture(g_quadric, GL_FALSE);
       g_tess = gluNewTess();
       gluTessCallback(g_tess, GLU_TESS_BEGIN,   (void(*)())glBegin);
       gluTessCallback(g_tess, GLU_TESS_END,     (void(*)())glEnd);
       gluTessCallback(g_tess, GLU_TESS_VERTEX,  (void(*)())tess_vertex_callback);
       gluTessCallback(g_tess, GLU_TESS_COMBINE, (void(*)())tess_combine_callback);
       gluTessCallback(g_tess, GLU_TESS_ERROR,   (void(*)())tess_error_callback);
       /* scriptable bootstrap */
       apply_init_bootstrap();
   }
   ```
   `repl_init_gl()` now calls `parse_init_bootstrap()` before `init_gl()`.

6. **Split `g_footer[]`** (`repl_core.c:268-314`) into:
   - `g_footer_pre_init[]` - rows 0-291 through the `void init() {` opener.
   - `g_footer_post_init[]` - the closing `}` of init() through rows to end.
   Delete the hard-coded init body lines 294-297.

7. **Add emit helpers.**
   - `int init_section_line_count(void)` - returns count of `g_init_host_only_c[]`
     strings plus enabled bootstrap entries.
   - `void init_section_line(int i, char *out, size_t n)` - renders the nth line
     (host-only first, then enabled bootstrap lines formatted via
     `write_cmd_source_as_c`-style formatting into `out`).
   - `void emit_init_section_to_file(FILE *f)` - writes the same sequence to `f`.
     Internally uses the same line-producing logic to guarantee parity.

8. **Wire `save_output()`** (`repl_core.c:2961-2962`). Replace the raw
   `g_footer[i]` loop with:
   ```c
   for (int i = 0; g_footer_pre_init[i]; i++)
       fprintf(f, "%s\n", g_footer_pre_init[i]);
   emit_init_section_to_file(f);
   for (int i = 0; g_footer_post_init[i]; i++)
       fprintf(f, "%s\n", g_footer_post_init[i]);
   ```

9. **Wire code panel** (`ui_panels.c`).
   - `code_panel_footer_row_count()` - sum row counts over `g_footer_pre_init`,
     each generated `init_section_line()`, and `g_footer_post_init`.
   - The render loop that currently iterates `g_footer` (`~line 1349`) iterates the
     same three lists, using `RENDER_STATIC_LINE` on each produced string.

10. **Add the `CfgItem`.** In `g_cfg_items[]` (`repl_core.c:~663`):
    ```c
    { "Point attenuation", "--", &g_init_attenuate_points, 2, NULL },
    ```
    Add a hook so toggling it calls `apply_init_bootstrap()` to re-sync live GL
    state (the `glPointParameterfv` call is idempotent and cheap). If the existing
    cfg-toggle path has no post-change hook, add the smallest possible hook: after
    mutating the backing int, detect that it was `g_init_attenuate_points` and call
    `apply_init_bootstrap()` + set a flag so the footer redraws.

11. **Tests** (`test_repl_core_io.c`).
    - `each bootstrap line parses into a valid GLCmd` - iterate the array and
      assert `parse_command()` returns success.
    - `save_output emits host-only init lines in order` - grep emitted file for
      `gluNewQuadric`, `gluQuadricTexture`, and `glLightModelfv`.
    - `save_output emits enabled bootstrap lines once` - grep for each bootstrap
      source line; confirm exactly one occurrence inside the init() body.
    - `toggle off removes attenuation line` - set `g_init_attenuate_points = 0`,
      `repl_save_output()`, assert the exported file does NOT contain
      `glPointParameterfv`.

## Verification

1. `make test` - all existing suites green plus the new assertions.
2. `make sample && ./sample` - visually confirm the code-panel footer's `init()`
   body now shows host-only lines + the enabled bootstrap lines in order, and that
   the init() body matches exactly what runtime applies.
3. `./sample`, toggle `Point attenuation` in the config menu - confirm the footer
   re-renders immediately to add/remove the bootstrap line, and that point rendering
   behavior in the 3D scene changes accordingly.
4. `Ctrl+S` / Save C button → `gcc -Wall ... output.c -framework OpenGL
   -framework GLUT -o out` → run `./out` - exported program compiles and matches
   live behavior for the bootstrap-state subset.
5. `./sample --dump-code` - dumped text matches the panel render.
6. `make test_repl_core_examples` - every example still exports and compiles.
