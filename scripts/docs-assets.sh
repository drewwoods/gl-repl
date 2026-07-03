#!/usr/bin/env bash
# docs-assets.sh - regenerate the screenshots and GIFs under docs/images/
# (the media embedded in README.md, docs/SHOWCASE.md and docs/USER_GUIDE.md).
#
#   scripts/docs-assets.sh [-j N] [asset ...]   # default: all assets
#   scripts/docs-assets.sh --list               # print asset names and exit
#
# -j / --jobs N regenerates up to N assets in parallel. Each gl-repl render
# is single-threaded, but on the native backend every capture opens a real
# window, so parallel runs pop N windows at once and fight for focus — keep
# the default of 1 unless you don't mind the screen churn. Default 1.
#
# Requires a NATIVE build (make gl-repl) plus ImageMagick (magick) and
# ffmpeg. Override the binary with BIN=<path>.
#
# Native vs OSMesa:
#   This script drives the NATIVE windowed backend, not the headless OSMesa
#   software rasterizer. The native build renders with the real GPU driver
#   (true colors, real MSAA, correct grid/theme rendering — OSMesa's swrast
#   mis-renders the adaptive-planes grid as a brown quad), so it is what the
#   docs should show. The trade-off is that it is NOT headless: each capture
#   opens a real window on screen for a moment. Run it on a machine with a
#   display.
#
# How it works:
#   - Every capture uses the backend's record mode (FREEGLUT_CAPTURE_FRAMES=N):
#     the app renders exactly N frames to numbered PPMs and exits. Stills keep
#     the LAST frame, so anything frame-based (grid/axes theme cross-fades,
#     animation time t) has deterministically settled — no wall-clock sleeps,
#     no SIGUSR1 races.
#   - Scene states are staged by loading generated snippet files whose
#     `/* @cfg slug = value */` headers set presentation state and whose
#     optional `// camera` block poses the camera. GLR_EDIT_LINE parks the
#     cursor for cursor-bound overlays (transform guides). Gallery scenes
#     load built-in examples by NAME (--example "..."), which is stable across
#     catalog reordering — never by numeric index.
#   - Antialiasing. The native context already has MSAA (the window requests
#     GLUT_MULTISAMPLE). On top of that, stills raise the accumulation-AA
#     sample count via GLR_ACCUM_PASSES: the scene is re-rendered with a
#     sub-pixel frustum jitter per pass and accumulated, which the 2D UI
#     renders OUTSIDE of — so the GLUT bitmap fonts in the code panel stay
#     crisp while 3D edges and 1px grid/axes hairlines smooth out at full
#     weight. We do NOT supersample by rendering into an oversized window:
#     a native window is clamped to the visible screen, so an over-size
#     request comes back the wrong size. 1x window + accum/MSAA AA instead.
#   - GIFs keep stock AA (just the native MSAA): raising the accum passes
#     would multiply the cost of every recorded frame.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/build/release/gl-repl}"
OUT="${OUT:-$ROOT/docs/images}"
SHOW="$OUT/showcase"

W=1200          # target width/height of every asset's source window
H=800
GIF_FUZZ=8%     # magick -layers Optimize fuzz for GIF delta compression

# Loading an --example eases the camera into place (with damping) over the
# first second or two of captured frames — a settle the docs GIFs shouldn't
# show. gif() renders this many extra leading frames for --example clips and
# discards them, so the clip starts settled at no cost to its length. One
# captured frame is ~1/60 s of that ease, so 180 ≈ 3 s.
WARM=180

# README + docs/images core set.
CORE_ASSETS=(
    hero first-triangle vertex-overlays wireframe light-theme-studio
    grid-themes backdrops axes-compass view-mode-2d labels-orrery
    glu-tess glow-sprites transform-stress variable-panel tune-badges
    motion-blur xform-guide-still xform-guide replay animated-ring
)

# docs/SHOWCASE.md gallery set (-> docs/images/showcase/). Recordable scenes
# only; the genuinely-interactive shots (slider drag, PLY in MeshLab, output.c
# in an editor) are stand-ins, see the showcase section below.
SHOWCASE_ASSETS=(
    sc-torus-knot sc-snowfall sc-parametric-torus sc-recursive-tree
    sc-spirograph sc-ripple-ring sc-bezier sc-de-casteljau sc-orbit-plot
    sc-wave-surface sc-ringed-planet sc-lit-cube sc-grass sc-jellyfish
    sc-function-demo sc-function-polygons sc-conditional-colors
    sc-whale sc-stress-test sc-feature-time sc-feature-ply sc-feature-export-c
)

ALL_ASSETS=("${CORE_ASSETS[@]}" "${SHOWCASE_ASSETS[@]}")

JOBS=1
ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --list) printf '%s\n' "${ALL_ASSETS[@]}"; exit 0 ;;
        -j|--jobs) JOBS="${2:?docs-assets: $1 needs a count}"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        *) ARGS+=("$1"); shift ;;
    esac
done

for tool in magick ffmpeg; do
    command -v "$tool" >/dev/null || {
        echo "docs-assets: '$tool' not found" >&2; exit 1; }
done
[[ -x "$BIN" ]] || {
    echo "docs-assets: gl-repl binary not found at '$BIN'" >&2
    echo "             build it first: make gl-repl" >&2
    exit 1; }

if [[ ${#ARGS[@]} -gt 0 ]]; then
    WANTED=("${ARGS[@]}")
else
    WANTED=("${ALL_ASSETS[@]}")
fi

# Parallel mode: re-exec ourselves one asset per process via xargs -P.
# (Portable to the macOS /bin/bash 3.2 era — no `wait -n`.) Each child
# gets its own work dir, so captures can't collide. Note: native captures
# each open a window, so -j N pops N at once.
if [[ "$JOBS" -gt 1 && ${#WANTED[@]} -gt 1 ]]; then
    printf '%s\n' "${WANTED[@]}" | xargs -n1 -P "$JOBS" "$0"
    echo "docs-assets: done ($JOBS jobs)."
    exit 0
fi

want() {
    local a
    for a in "${WANTED[@]}"; do [[ "$a" == "$1" ]] && return 0; done
    return 1
}

WORK="$(mktemp -d /tmp/glr-docs-assets.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT" "$SHOW"

# render <frames> <aa> <args...> — run one capture into $FRDIR (fresh per
# call, so renders never collide), always at the 1x WxH window. aa raises the
# accumulation sample count via GLR_ACCUM_PASSES (0 = stock AA / native MSAA
# only). Extra env (GLR_EDIT_LINE etc.) is passed by exporting before the call.
render() {
    local frames=$1 aa=$2; shift 2
    FRDIR="$WORK/frames-$RANDOM$RANDOM"
    mkdir -p "$FRDIR"
    local aa_env=()
    [[ "$aa" != 0 ]] && aa_env=("GLR_ACCUM_PASSES=$aa")
    # ${arr[@]:+...} keeps an empty array safe under `set -u` (bash 3.2).
    env ${aa_env[@]:+"${aa_env[@]}"} \
        FREEGLUT_CAPTURE_FRAMES=$frames FREEGLUT_CAPTURE_FILE="$FRDIR/f" \
        "$BIN" "$@" --window ${W}x${H} --no-audio \
        >/dev/null 2>&1
}

# still <out.png> <frames> <aa> <args...> — keep the LAST frame.
still() {
    local out=$1 frames=$2 aa=$3; shift 3
    render "$frames" "$aa" "$@"
    local last
    last="$(ls "$FRDIR"/f-*.ppm | tail -1)"
    magick "$last" "$out"
    rm -rf "$FRDIR"
    echo "docs-assets: wrote $out"
}

# gif <out.gif> <frames> <step> <fps> <width> <args...> — record, take
# every <step>th frame, assemble a palette-optimized looping GIF. The kept
# frame count (frames/step) and fps set the clip length.
#
# --example clips additionally render WARM extra leading frames and discard
# them (the camera-ease warmup, see WARM), so the clip starts settled while
# keeping the exact same frames/step at the same fps — identical length, just
# past the settle. Staged scenes have no load ease, and replay's draw-by-draw
# assembly IS the content, so a clip with no --example keeps frame 0.
gif() {
    local out=$1 frames=$2 step=$3 fps=$4 width=$5; shift 5
    local skip=0 a
    for a in "$@"; do [[ "$a" == "--example" ]] && { skip=$WARM; break; }; done
    render "$((skip + frames))" 0 "$@"
    rm -rf "$WORK/sub"; mkdir -p "$WORK/sub"
    local n=0 k=0 f
    for f in "$FRDIR"/f-*.ppm; do
        if (( n >= skip && (n - skip) % step == 0 )); then
            cp "$f" "$WORK/sub/g-$(printf %04d $k).ppm"; k=$((k + 1))
        fi
        n=$((n + 1))
    done
    ffmpeg -y -framerate "$fps" -i "$WORK/sub/g-%04d.ppm" \
        -vf "scale=$width:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=128[p];[s1][p]paletteuse=dither=bayer" \
        -loop 0 "$WORK/raw.gif" >/dev/null 2>&1
    magick "$WORK/raw.gif" -fuzz "$GIF_FUZZ" -layers Optimize "$out"
    echo "docs-assets: wrote $out"
}

# stage <name> — write a staged snippet scene to $WORK/<name>.c (heredoc
# read from stdin) and echo its path.
stage() {
    cat > "$WORK/$1.c"
    echo "$WORK/$1.c"
}

# montage2x2 <out> <a> <b> <c> <d> — tile four images 2x2 with a thin black
# gutter, font-free. (We avoid `magick montage`: its default per-tile label
# needs a font, and a misconfigured ImageMagick resolves the font to '' and
# aborts the whole tiling. Plain +append/-append needs no font.)
montage2x2() {
    local out=$1 a=$2 b=$3 c=$4 d=$5
    magick \
        \( "$a" -bordercolor black -border 2 \) \
        \( "$b" -bordercolor black -border 2 \) +append "$WORK/_row1.png"
    magick \
        \( "$c" -bordercolor black -border 2 \) \
        \( "$d" -bordercolor black -border 2 \) +append "$WORK/_row2.png"
    magick "$WORK/_row1.png" "$WORK/_row2.png" -append -background black "$out"
}

# ---- staged scenes ------------------------------------------------------

stage_triangle() { stage triangle <<'EOF'
// Snippet start
glColor3f(1, 0.6, 0.1);
glBegin(GL_TRIANGLES);
glVertex3f(0, 1, 0);
glVertex3f(-1, -1, 0);
glVertex3f(1, -1, 0);
glEnd();
// Snippet end
EOF
}

stage_overlays() { stage overlays <<'EOF'
/* @cfg vertex_labels = 2 */
/* @cfg normal_vectors = 1 */
/* @cfg vertex_outlines = 1 */
/* @cfg vertex_points = 1 */
/* @cfg poly_highlight = 1 */
/* @cfg grid = GRID_THEME_FAINT */
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.2, 0.7, 1);
glBegin(GL_QUADS);
glNormal3f(0, 0, 1);
glVertex3f(-1, -1, 0);
glVertex3f(1, -1, 0);
glVertex3f(1, 1, 0);
glVertex3f(-1, 1, 0);
glEnd();
// Snippet end
EOF
}

stage_wireframe() { stage wireframe <<'EOF'
/* @cfg wireframe = 1 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.3, 0.9, 1);
glutSolidTorus(0.4, 1.2, 24, 36);
// Snippet end
EOF
}

stage_lights() { stage lights <<'EOF'
/* @cfg light_theme = LIGHT_THEME_DEFAULT */
/* @cfg grid = GRID_THEME_OFF */
/* @cfg light_indicators = 1 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
glTranslatef(0.0000f, 0.0000f, -7.0409f);
glRotatef(15.5000f, 1.0f, 0.0f, 0.0f);
glRotatef(-118.0000f, 0.0f, 1.0f, 0.0f);
glTranslatef(-0.0000f, -0.0000f, -0.0000f);
// Snippet start
glClearColor(0.0, 0.0, 0.0, 1.0);
glEnable(GL_DEPTH_TEST);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glEnable(GL_LIGHT1);
glEnable(GL_LIGHT2);
glEnable(GL_COLOR_MATERIAL);
glColor3f(0.8, 0.7, 0.6);
glutSolidTeapot(1);
// Snippet end
EOF
}

stage_grid_theme() {  # $1 = GRID_THEME_<NAME>
    stage "grid-$1" <<EOF
/* @cfg grid = $1 */
/* @cfg grid_brightness = GRID_BRIGHTNESS_BOLD */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg vertex_outlines = 1 */
/* @cfg vertex_points = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glEnable(GL_DEPTH_TEST);
//glColor3f(0.9, 0.55, 0.15);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT0);
glutSolidCube(0.8);
// Snippet end
EOF
}

stage_backdrop() {  # $1 = RENDER3D_BACKDROP_<NAME>, $2 = flat|default camera
    local cam=""
    if [[ "${2:-}" == "flat" ]]; then
        cam='// camera
glTranslatef(0.0f, 0.0f, -6.0f);
glRotatef(2.0f, 1.0f, 0.0f, 0.0f);
glRotatef(0.0f, 0.0f, 1.0f, 0.0f);
glTranslatef(0.0f, -0.5f, 0.0f);'
    fi
    stage "bd-$1" <<EOF
/* @cfg backdrop = $1 */
/* @cfg grid = GRID_THEME_OFF */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg vertex_outlines = 1 */
/* @cfg vertex_points = 0 */
/* @cfg light_indicators = 0 */
$cam
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.8, 0.8, 0.8);
glEnable(GL_LIGHTING);
glEnable(GL_LIGHT1);
glutSolidCube(0.8);
// Snippet end
EOF
}

stage_axes() { stage axes <<'EOF'
/* @cfg axes = AXES_THEME_COMPASS */
/* @cfg grid = GRID_THEME_FAINT */
/* @cfg code_panel = 3 */
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.9, 0.55, 0.15);
glutSolidCube(0.8);
// Snippet end
EOF
}

stage_tune() { stage tune <<'EOF'
/* @cfg variable_panel = 1 */
// Snippet start
float amp = 1.2; // @tune
float freq = 2; // @tune
float spread = 0.8;
glColor3f(0.4, 0.8, 1);
glBegin(GL_LINE_STRIP);
for(i, 0, 64) {
glVertex3f(-1.6 + i*0.05, sin(i*0.1*freq + t)*amp*0.4, spread*cos(i*0.05));
}
glEnd();
// Snippet end
EOF
}

stage_blur() { stage blur <<'EOF'
/* @cfg accum_effect = 2 */
/* @cfg accum_passes = 16 */
/* @cfg variable_panel = 0 */
/* @cfg vertex_outlines = 0 */
/* @cfg vertex_points = 0 */
/* @cfg light_indicators = 0 */
// Snippet start
glEnable(GL_DEPTH_TEST);
glPushMatrix();
glRotatef(t*200, 0, 1, 0);
glColor3f(0.9, 0.55, 0.15);
glTranslatef(1.2, 0, 0);
glutSolidCube(0.6);
glPopMatrix();
// Snippet end
EOF
}

# Cursor parks on line 4 (glTranslatef) via GLR_EDIT_LINE.
stage_guide() { stage guide <<'EOF'
/* @cfg variable_panel = 0 */
/* @cfg light_indicators = 0 */
/* @cfg grid = GRID_THEME_FAINT */
// Snippet start
glEnable(GL_DEPTH_TEST);
glColor3f(0.9, 0.55, 0.15);
glutSolidCube(0.5);
glPushMatrix();
glTranslatef(2, 0.8, 0);
glColor3f(0.3, 0.8, 0.9);
glutSolidCube(0.5);
glPopMatrix();
// Snippet end
EOF
}

stage_replay() { stage replay <<'EOF'
/* @cfg replay = 1 */
/* @cfg variable_panel = 0 */
// Snippet start
glEnable(GL_DEPTH_TEST);
glBegin(GL_TRIANGLES);
for(i, 0, 12) {
glColor3f(0.5 + 0.5*cos(i*TAU/12), 0.6, 0.5 + 0.5*sin(i*TAU/12));
glVertex3f(cos(i*TAU/12)*1.5, sin(i*TAU/12)*1.5, 0);
glVertex3f(cos((i+0.6)*TAU/12)*1.5, sin((i+0.6)*TAU/12)*1.5, 0);
glVertex3f(cos((i+0.3)*TAU/12)*0.7, sin((i+0.3)*TAU/12)*0.7, 0);
}
glEnd();
// Snippet end
EOF
}

# ---- example names (stable; never reference examples by index) ----------

EX_HERO="Teapot carousel (transform stacks + glow points)"
EX_ORRERY="Orrery (labels track 3D orbits)"
EX_GLU="GLU tessellator (concave arrow)"
EX_GLOW="Glow sprites (blend + point attenuation)"
EX_XFORM="Transform stress (translate/rotate/scale guides)"
EX_RING="Animated ring (for + t)"

# ---- core assets --------------------------------------------------------

# Theme cross-fades settle in ~80 frames; 90 leaves margin. Plain shots
# settle immediately; 30 frames keeps t small but past startup.
FADE_FRAMES=90
PLAIN_FRAMES=30

if want hero; then
    still "$OUT/hero.png" $PLAIN_FRAMES 16 --example "$EX_HERO" --time 3
fi

if want first-triangle; then
    still "$OUT/first-triangle.png" $PLAIN_FRAMES 16 "$(stage_triangle)"
fi

if want vertex-overlays; then
    still "$OUT/vertex-overlays.png" $PLAIN_FRAMES 16 "$(stage_overlays)"
fi

if want wireframe; then
    still "$OUT/wireframe.png" $PLAIN_FRAMES 16 "$(stage_wireframe)"
fi

if want light-theme-studio; then
    still "$OUT/light-theme-studio.png" $PLAIN_FRAMES 16 "$(stage_lights)"
fi

if want grid-themes; then
    themes=(SKETCH RADAR PLANES OCEAN)
    files=()
    for theme in "${themes[@]}"; do
        still "$WORK/grid_$theme.png" $FADE_FRAMES 16 \
            "$(stage_grid_theme GRID_THEME_$theme)"
        files+=("$WORK/grid_$theme.png")
    done
    montage2x2 "$WORK/grid-m.png" "${files[@]}"
    magick "$WORK/grid-m.png" -resize "$W" "$OUT/grid-themes.png"
    echo "docs-assets: wrote $OUT/grid-themes.png"
fi

if want backdrops; then
    backdrops=(
        "POLAR_DAY_SNOW"
        "NEBULA flat"
        "SUNSET flat"
        "AURORA flat"
    )
    files=()
    for item in "${backdrops[@]}"; do
        name="${item%% *}"
        if [[ "$item" == *" "* ]]; then
            cam="${item#* }"
        else
            cam=""
        fi
        still "$WORK/bd_$name.png" $FADE_FRAMES 16 \
            "$(stage_backdrop RENDER3D_BACKDROP_$name "$cam")"
        files+=("$WORK/bd_$name.png")
    done
    montage2x2 "$WORK/bd-m.png" "${files[@]}"
    magick "$WORK/bd-m.png" -resize "$W" "$OUT/backdrops.png"
    echo "docs-assets: wrote $OUT/backdrops.png"
fi

if want axes-compass; then
    still "$OUT/axes-compass.png" $FADE_FRAMES 16 "$(stage_axes)"
fi

if want view-mode-2d; then
    # A GIF that toggles View mode (Ctrl+Shift+V) on the animated wave surface,
    # so the doc shows the 3D->2D->3D transition rather than a frozen 2D still.
    # GLR_VIEW_TOGGLE_AT fires on gl_repl's rendered-frame clock (t = frame/60),
    # which counts the WARM leading frames gif() renders and discards for
    # --example clips. WARM=180 => 3.0s of that clock elapses before the kept
    # clip begins, so a toggle at clock second S lands at (S*60 - WARM)/(step)
    # kept frames in, i.e. output time (S*60 - WARM)/(step*fps) s. With
    # step=2 fps=20: clock 4s -> 1.5s into the clip (3D->2D), clock 6s -> 4.5s
    # (2D->3D). The 6s clip then holds ~1.5s of 3D before it loops.
    ( export GLR_VIEW_TOGGLE_AT=4,6
      gif "$OUT/view-mode-2d.gif" 240 2 20 560 \
          --example "Animated wave surface (analytic normals)" )
fi

if want labels-orrery; then
    still "$OUT/labels-orrery.png" $PLAIN_FRAMES 16 --example "$EX_ORRERY" --time 4
fi

if want glu-tess; then
    still "$OUT/glu-tess.png" $PLAIN_FRAMES 16 --example "$EX_GLU"
fi

if want glow-sprites; then
    still "$OUT/glow-sprites.png" $PLAIN_FRAMES 16 --example "$EX_GLOW" --time 2
fi

if want transform-stress; then
    still "$OUT/transform-stress.png" $PLAIN_FRAMES 16 --example "$EX_XFORM" --time 1
fi

if want variable-panel || want tune-badges; then
    still "$WORK/variable-panel.png" $PLAIN_FRAMES 16 "$(stage_tune)"
    if want variable-panel; then
        cp "$WORK/variable-panel.png" "$OUT/variable-panel.png"
        echo "docs-assets: wrote $OUT/variable-panel.png"
    fi
    if want tune-badges; then
        magick "$WORK/variable-panel.png" -crop 260x180+940+680 +repage \
            "$OUT/tune-badges.png"
        echo "docs-assets: wrote $OUT/tune-badges.png"
    fi
fi

if want motion-blur; then
    # The blur itself is the scene's @cfg accum effect; no extra AA passes.
    # The status bar's "Blur 16x" indicator is part of the shot.
    still "$OUT/motion-blur.png" 12 0 "$(stage_blur)"
fi

# Subshells: a VAR=x prefix on a shell *function* call persists after the
# call in bash, so export GLR_EDIT_LINE inside ( ) to keep it scoped.
if want xform-guide-still; then
    ( export GLR_EDIT_LINE=4
      still "$OUT/xform-guide-still.png" $PLAIN_FRAMES 16 "$(stage_guide)" )
fi

if want xform-guide; then
    ( export GLR_EDIT_LINE=4
      gif "$OUT/xform-guide.gif" 120 1 20 720 "$(stage_guide)" )
fi

if want replay; then
    # Replay advances a few commands per second; 500 frames subsampled
    # 3x shows the whole pinwheel assembling at a watchable pace.
    gif "$OUT/replay.gif" 500 3 24 720 "$(stage_replay)"
fi

if want animated-ring; then
    gif "$OUT/animated-ring.gif" 126 2 18 560 --example "$EX_RING"
fi

# ---- showcase assets (docs/images/showcase/) ----------------------------
#
# The SHOWCASE gallery shows each scene WITH its code panel (the whole point:
# "the geometry lives in the code"), so these use the default full-UI render.
# Animated examples become GIFs; static ones become stills.

# Featured.
if want sc-torus-knot; then
    gif "$SHOW/torus-knot.gif" 200 2 20 720 --example "Torus knot (animated)"
fi
if want sc-snowfall; then
    gif "$SHOW/snowfall.gif" 220 2 22 720 --example "Snowfall demo (550 particles)"
fi
if want sc-parametric-torus; then
    # Static geometry (nested for, no t) — a still, not a frozen GIF.
    still "$SHOW/parametric-torus.png" $PLAIN_FRAMES 16 \
        --example "Parametric torus (nested for)"
fi
if want sc-recursive-tree; then
    gif "$SHOW/recursive-tree.gif" 200 2 20 720 \
        --example "Recursive triangle tree (func + recursion)"
fi

# Curves & line art.
if want sc-spirograph; then
    gif "$SHOW/spirograph.gif" 200 2 20 560 --example "Animated spirograph curve"
fi
if want sc-ripple-ring; then
    gif "$SHOW/ripple-ring.gif" 200 2 20 560 --example "Traveling ripple ring"
fi
if want sc-bezier; then
    still "$SHOW/bezier.png" $PLAIN_FRAMES 16 --example "Bezier curve with guides"
fi
if want sc-de-casteljau; then
    still "$SHOW/de-casteljau.png" $PLAIN_FRAMES 16 \
        --example "Scratch arrays (de Casteljau curve)"
fi
if want sc-orbit-plot; then
    still "$SHOW/orbit-plot.png" $PLAIN_FRAMES 16 \
        --example "Annotated orbit plot (labels)"
fi

# Surfaces. (The old "Procedural terrain" example is gone; the new "Ringed
# planet" stands in for the third surface tile.)
if want sc-wave-surface; then
    gif "$SHOW/wave-surface.gif" 200 2 20 560 \
        --example "Animated wave surface (analytic normals)"
fi
if want sc-ringed-planet; then
    gif "$SHOW/ringed-planet.gif" 200 2 20 560 --example "Ringed planet (nebula skies)"
fi
if want sc-lit-cube; then
    still "$SHOW/lit-cube.png" $PLAIN_FRAMES 16 --example "Lit cube"
fi

# Particles & effects.
if want sc-grass; then
    gif "$SHOW/grass.gif" 200 2 20 560 --example "Swaying grass field (rand + t)"
fi
if want sc-jellyfish; then
    gif "$SHOW/jellyfish.gif" 220 2 22 560 --example "Jellyfish (glDepthMask translucency)"
fi

# Functions, branching & recursion.
if want sc-function-demo; then
    still "$SHOW/function-demo.png" $PLAIN_FRAMES 16 --example "Function demo (named func)"
fi
if want sc-function-polygons; then
    still "$SHOW/function-polygons.png" $PLAIN_FRAMES 16 \
        --example "Function polygons (args + for)"
fi
if want sc-conditional-colors; then
    gif "$SHOW/conditional-colors.gif" 200 2 20 560 --example "Conditional colors (if + t)"
fi

# Big scenes.
if want sc-whale; then
    gif "$SHOW/whale.gif" 240 2 22 560 --example "Whale (particle system + lit model)"
fi
if want sc-stress-test; then
    gif "$SHOW/stress-test.gif" 240 2 22 560 \
        --example "Dusk lighthouse atoll (stress test)"
fi

# "Beyond the still image" — interactive features. feature-time is a real
# t-driven clip (the narrative "freeze then play" can't be scripted headless,
# but the motion is honest). feature-ply / feature-export-c want an external
# tool (MeshLab / a text editor) we can't drive headlessly, so they reuse a
# representative scene still as a stand-in; the SHOWCASE comments document the
# ideal shot.
if want sc-feature-time; then
    gif "$SHOW/feature-time.gif" 200 2 20 560 --example "Conditional colors (if + t)"
fi
if want sc-feature-ply; then
    # Stand-in: the parametric torus is the canonical --export-ply scene.
    still "$SHOW/feature-ply.png" $PLAIN_FRAMES 16 \
        --example "Parametric torus (nested for)"
fi
if want sc-feature-export-c; then
    # Stand-in: any full-UI shot shows "it's all code in the panel".
    still "$SHOW/feature-export-c.png" $PLAIN_FRAMES 16 --example "Function demo (named func)"
fi

echo "docs-assets: done."
