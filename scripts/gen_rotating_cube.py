#!/usr/bin/env python3
"""Generate the rotating open-cube mark as a self-contained animated SVG.

The mark is the 4-faced open cube from examples/scenes/lit-cube.glr (both Z
faces absent), palette sampled from the app's own render: quiet near-white
exterior, azure interior wall + magenta interior floor showing through the
openings, white edges, dark ground assumed.  The cube spins about Y at the
splash.c isometric pitch, starting from the resting down-the-diagonal angle
(the static logo.svg frame); twice per revolution the openings line up and
the cube reads see-through.

The rotation is baked into SMIL keyframes (same technique as the wordmark
hero cube): every face gets two polygons — an interior copy in a back layer
and an exterior copy in a front layer — with animated `points`, `fill`
(shading), and discrete `fill-opacity` flips at the visibility boundaries.
For a convex solid the back faces never overlap each other and neither do
the front faces, so the two fixed document layers replace per-frame depth
sorting.  Edges get the same two-layer treatment: an edge is "front" while
any adjacent existing face is front-facing, else it is an interior edge seen
through an opening (drawn dimmer, occluded by the exterior layer).

Usage:
  python3 scripts/gen_rotating_cube.py [OUT.svg] [--frames N] [--dur SECS]
  python3 scripts/gen_rotating_cube.py --static F [OUT.svg]

Defaults: OUT docs/images/logo-rotating.svg, 90 frames, 12 s/revolution.
--static F emits a non-animated snapshot of frame F (debugging / previews;
rasterizers like rsvg-convert only show SMIL frame 0 otherwise).
"""

import math
import sys

# ---- geometry -------------------------------------------------------------

VERTS = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
         (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]

# The 4 faces lit-cube.glr draws (outward-CCW vertex order); +x/-x are open
# (at the resting view the -x opening faces the camera on the left, the +z
# wall is the solid right face, exactly as in the adopted render).
FACES = {
    '+y': (2, 3, 7, 6),
    '-y': (0, 1, 5, 4),
    '+z': (4, 5, 6, 7),
    '-z': (0, 3, 2, 1),
}

# All 12 cube edges survive the missing faces (each is shared with a kept
# face); adjacency below lists only the faces that still exist.
EDGES = [
    ((0, 1), ('-y', '-z')), ((1, 2), ('-z',)),
    ((2, 3), ('+y', '-z')), ((3, 0), ('-z',)),
    ((4, 5), ('-y', '+z')), ((5, 6), ('+z',)),
    ((6, 7), ('+y', '+z')), ((7, 4), ('+z',)),
    ((0, 4), ('-y',)), ((1, 5), ('-y',)),
    ((2, 6), ('+y',)), ((3, 7), ('+y',)),
]

# ---- palette (sampled from the lit-cube.glr render) ------------------------

EXT = {'+z': (210, 213, 219), '-z': (214, 216, 222),
       '+y': (199, 202, 222), '-y': (185, 188, 198)}
INN = {'+z': (79, 169, 240), '-z': (61, 168, 245),
       '+y': (143, 150, 187), '-y': (248, 65, 224)}

EDGE_FRONT = '#ffffff'
EDGE_BACK = '#ffffff'
EDGE_FRONT_OPACITY = 0.96
EDGE_BACK_OPACITY = 0.72

PITCH = math.atan(1.0 / math.sqrt(2.0))   # 35.264 deg, splash.c's iso pitch
REST_YAW = math.pi / 4.0                  # the static-logo resting angle

# viewBox matches docs/images/logo.svg
VIEW_W, VIEW_H = 240, 244
CX, CY, SCALE = 120.0, 124.0, 48.0


def rotate(v, yaw):
    """splash.c projection: rotY then rotX; returns full 3D view coords."""
    cy, sy = math.cos(yaw), math.sin(yaw)
    x = v[0] * cy + v[2] * sy
    z1 = -v[0] * sy + v[2] * cy
    cb, sb = math.cos(PITCH), math.sin(PITCH)
    y = v[1] * cb - z1 * sb
    z = v[1] * sb + z1 * cb
    return (x, y, z)


def screen(p):
    return (CX + p[0] * SCALE, CY - p[1] * SCALE)


def face_normal_z(rot_verts, face):
    a, b, c = (rot_verts[i] for i in face[:3])
    e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    e2 = (c[0] - b[0], c[1] - b[1], c[2] - b[2])
    nx = e1[1] * e2[2] - e1[2] * e2[1]
    ny = e1[2] * e2[0] - e1[0] * e2[2]
    nz = e1[0] * e2[1] - e1[1] * e2[0]
    n = math.sqrt(nx * nx + ny * ny + nz * nz)
    return nz / n if n else 0.0


def shade(rgb, k):
    return 'rgb(%d,%d,%d)' % tuple(min(255, int(c * k)) for c in rgb)


def bake(frames):
    """Per frame: screen points, front flag, and normal-z for every face."""
    data = {name: {'pts': [], 'front': [], 'nz': []} for name in FACES}
    for f in range(frames + 1):                      # +1: wrap frame = frame 0
        yaw = REST_YAW + 2.0 * math.pi * f / frames
        rv = [rotate(v, yaw) for v in VERTS]
        for name, face in FACES.items():
            nz = face_normal_z(rv, face)
            pts = ' '.join('%.1f,%.1f' % screen(rv[i]) for i in face)
            data[name]['pts'].append(pts)
            data[name]['front'].append(nz > 0.0)
            data[name]['nz'].append(nz)
    # sanity: the resting frame must match the static logo's reading —
    # top (+y) and the right wall (+z) face the viewer; the floor (-y) and
    # far wall (-z) are the interior seen through the -x opening.
    assert data['+y']['front'][0] and data['+z']['front'][0], \
        'front-face calibration broke: top/right must face the viewer at rest'
    assert not data['-y']['front'][0] and not data['-z']['front'][0], \
        'front-face calibration broke: floor/wall must be interior at rest'
    return data


def edge_points(frames):
    pts = []
    for f in range(frames + 1):
        yaw = REST_YAW + 2.0 * math.pi * f / frames
        rv = [rotate(v, yaw) for v in VERTS]
        pts.append(['%.1f,%.1f %.1f,%.1f'
                    % (screen(rv[a]) + screen(rv[b])) for (a, b), _ in EDGES])
    return pts


def anim(attr, values, dur, discrete=False):
    mode = ' calcMode="discrete"' if discrete else ''
    return ('      <animate attributeName="%s" dur="%ss" repeatCount='
            '"indefinite"%s values="%s" />\n' % (attr, dur, mode,
                                                 ';'.join(values)))


def face_polygon(name, data, layer, dur, static_frame=None):
    """One polygon: interior copy (layer='inn') or exterior copy ('ext')."""
    d = data[name]
    want_front = (layer == 'ext')
    fills, opac = [], []
    for front, nz in zip(d['front'], d['nz']):
        if layer == 'ext':
            fills.append(shade(EXT[name], 0.86 + 0.14 * max(nz, 0.0)))
        else:
            fills.append(shade(INN[name], 0.78 + 0.22 * max(-nz, 0.0)))
        opac.append('1' if front == want_front else '0')
    if static_frame is not None:
        f = static_frame
        return ('  <polygon points="%s" fill="%s" fill-opacity="%s"/>\n'
                % (d['pts'][f], fills[f], opac[f]))
    out = '    <polygon points="%s" fill="%s" fill-opacity="%s">\n' \
        % (d['pts'][0], fills[0], opac[0])
    out += anim('points', d['pts'], dur)
    out += anim('fill', fills, dur)
    out += anim('fill-opacity', opac, dur, discrete=True)
    out += '    </polygon>\n'
    return out


def edge_polyline(idx, epts, data, layer, dur, static_frame=None):
    (a, b), adj = EDGES[idx]
    frames = len(epts) - 1
    want_front = (layer == 'front')
    color = EDGE_FRONT if want_front else EDGE_BACK
    width = '2.2' if want_front else '1.5'
    base_op = EDGE_FRONT_OPACITY if want_front else EDGE_BACK_OPACITY
    pts, opac = [], []
    for f in range(frames + 1):
        pts.append(epts[f][idx])
        is_front = any(data[n]['front'][f] for n in adj)
        opac.append(str(base_op) if is_front == want_front else '0')
    if static_frame is not None:
        f = static_frame
        return ('  <polyline points="%s" fill="none" stroke="%s" '
                'stroke-width="%s" stroke-opacity="%s" '
                'stroke-linecap="round"/>\n' % (pts[f], color, width, opac[f]))
    out = ('    <polyline points="%s" fill="none" stroke="%s" '
           'stroke-width="%s" stroke-opacity="%s" stroke-linecap="round">\n'
           % (pts[0], color, width, opac[0]))
    out += anim('points', pts, dur)
    out += anim('stroke-opacity', opac, dur, discrete=True)
    out += '    </polyline>\n'
    return out


def generate(out_path, frames, dur, static_frame=None):
    data = bake(frames)
    epts = edge_points(frames)

    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
           'viewBox="0 0 %d %d" role="img" aria-label="gl-repl logo: '
           'rotating open cube, azure and magenta interior showing through '
           'the missing faces">\n' % (VIEW_W * 2, VIEW_H * 2, VIEW_W, VIEW_H))
    svg += ('  <!-- Generated by scripts/gen_rotating_cube.py — do not edit '
            'by hand.\n       Animated twin of docs/images/logo.svg '
            '(frame 0 is that resting angle). -->\n')
    svg += '  <rect width="%d" height="%d" fill="#000000"/>\n' % (VIEW_W,
                                                                  VIEW_H)
    layers = (
        ('interior faces (seen through the openings)', 'inn', 'back'),
        ('exterior faces', 'ext', 'front'),
    )
    for comment, face_layer, edge_layer in layers:
        svg += '  <!-- %s -->\n  <g>\n' % comment
        for name in FACES:
            svg += face_polygon(name, data, face_layer, dur, static_frame)
        for i in range(len(EDGES)):
            svg += edge_polyline(i, epts, data, edge_layer, dur, static_frame)
        svg += '  </g>\n'
    svg += '</svg>\n'

    with open(out_path, 'w') as fh:
        fh.write(svg)
    what = ('static frame %d' % static_frame) if static_frame is not None \
        else '%d keyframes, %ss/rev' % (frames, dur)
    print('wrote %s (%s)' % (out_path, what))


def main(argv):
    out, frames, dur, static_frame = 'docs/images/logo-rotating.svg', 90, 12, None
    args = list(argv[1:])
    while args:
        a = args.pop(0)
        if a == '--frames':
            frames = int(args.pop(0))
        elif a == '--dur':
            dur = float(args.pop(0))
        elif a == '--static':
            static_frame = int(args.pop(0))
        elif a in ('-h', '--help'):
            print(__doc__)
            return 0
        else:
            out = a
    if static_frame is not None and not 0 <= static_frame <= frames:
        raise SystemExit('--static frame out of range 0..%d' % frames)
    generate(out, frames, dur, static_frame)
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
