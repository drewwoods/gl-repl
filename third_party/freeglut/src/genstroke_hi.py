#!/usr/bin/env python3
"""genstroke_hi.py - generate a higher-resolution GLUT stroke font.

Reads one of freeglut's machine-generated fg_stroke_*.c fonts (Roman or
MonoRoman -- the font stem is auto-detected from the header) and emits the
matching fg_stroke_*_hi.c: a drop-in font with the SAME glyph anchor points,
metrics and advance widths, but with curved strokes resampled at much higher
resolution so they render smooth at large sizes.

Method: per stroke polyline, classify each interior vertex as a hard CORNER
(turn angle above a threshold) or a smooth curve point. Runs of smooth points
between hard points are replaced by a centripetal Catmull-Rom spline that
passes exactly through every original vertex, sampled at SUBDIV points per
segment. Straight 2-point runs and corners are left untouched, so E/L/T stay
crisp while O/S/C/e get smooth. Closed loops (first vertex == last) are
resampled periodically for a seamless seam.

Usage: genstroke_hi.py <in fg_stroke_*.c> <out fg_stroke_*_hi.c>
"""
import math
import re
import sys

# ---- tunables -------------------------------------------------------------
STRAIGHT_DEG   = 10.0   # |turn| <= this => essentially straight (spline through)
CORNER_DEG     = 80.0   # |turn| >= this => hard, except recognized round marks
REVERSAL_DEG   = 150.0  # |turn| >= this => cusp/fold-back, always a hard corner
LONG_END_RATIO = 3.0    # keep a long terminal stem straight vs its next edge
SUBDIV         = 6      # samples per segment inside a smooth run (>=1; 1 == no-op)
CLOSE_EPS      = 1e-3   # first/last vertex closer than this => closed loop


def camel_snake(s):
    """'Roman' -> 'ROMAN', 'MonoRoman' -> 'MONO_ROMAN' (for the GLUT id)."""
    return re.sub(r'(?<!^)(?=[A-Z])', '_', s).upper()


def parse_font(text):
    """Return (base, name, quantity, height, {code: (right, [strip,...])}).
    `base` is the identifier stem parsed from the font header (e.g. 'Roman'
    or 'MonoRoman'), so this works for any fg_stroke_*.c. `strip` is a list of
    (x, y) float pairs, keyed by ASCII code."""
    # Font header: fgStroke<Base> = {"<Name>",128,152.381f,Stroke<Base>_chars};
    m = re.search(r'SFG_StrokeFont\s+fgStroke(\w+)\s*=\s*\{\s*"([^"]*)"\s*,\s*'
                  r'(\d+)\s*,\s*([0-9.eEf+-]+)\s*,', text)
    if not m:
        raise SystemExit("could not find an SFG_StrokeFont fgStroke* header")
    base, name = m.group(1), m.group(2)
    quantity, height = int(m.group(3)), m.group(4).rstrip('f')

    num = r'[-+0-9.eEf]+'
    pre = 'Stroke' + base
    # Vertex arrays: Stroke<Base>_ch33st0[] = { {x,y}, ... };
    verts = {}  # (code, strip_idx) -> [(x,y), ...]
    vre = re.compile(pre + r'_ch(\d+)st(\d+)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;',
                     re.DOTALL)
    pair = re.compile(r'\{\s*(' + num + r')\s*,\s*(' + num + r')\s*\}')
    for vm in vre.finditer(text):
        code, sidx = int(vm.group(1)), int(vm.group(2))
        pts = [(float(a.rstrip('f')), float(b.rstrip('f')))
               for a, b in pair.findall(vm.group(3))]
        verts[(code, sidx)] = pts

    # Char defs: Stroke<Base>_ch33 = {26.6238f,2,Stroke<Base>_ch33st};
    chars = {}
    cre = re.compile(pre + r'_ch(\d+)\s*=\s*\{\s*(' + num +
                     r')\s*,\s*(\d+)\s*,\s*' + pre + r'_ch\d+st\s*\}\s*;')
    for cm in cre.finditer(text):
        code = int(cm.group(1))
        right = float(cm.group(2).rstrip('f'))
        nstrips = int(cm.group(3))
        strips = [verts.get((code, k), []) for k in range(nstrips)]
        chars[code] = (right, strips)
    return base, name, quantity, height, chars


def parse_roman(text):
    """Back-compat shim returning (name, quantity, height, chars)."""
    _, name, quantity, height, chars = parse_font(text)
    return name, quantity, height, chars


def turn_angle(a, b, c):
    """Unsigned direction change (degrees) of the path a->b->c at b."""
    return abs(signed_turn(a, b, c))


def signed_turn(a, b, c):
    """Signed direction change (degrees) at b: + turns left, - turns right,
    0 straight. The sign lets a coarse circle (all same-direction turns, e.g. a
    punctuation dot) be told apart from a zigzag (alternating turns, e.g. Z)."""
    v1x, v1y = b[0] - a[0], b[1] - a[1]
    v2x, v2y = c[0] - b[0], c[1] - b[1]
    n1 = math.hypot(v1x, v1y)
    n2 = math.hypot(v2x, v2y)
    if n1 < 1e-9 or n2 < 1e-9:
        return 0.0
    dot = (v1x * v2x + v1y * v2y) / (n1 * n2)
    dot = max(-1.0, min(1.0, dot))
    ang = math.degrees(math.acos(dot))
    return ang if (v1x * v2y - v1y * v2x) >= 0 else -ang


def arc_extrapolate(a, b, c):
    """Continue the arc a->b->c to a 4th control point, preserving the last
    turn angle and edge length (so a terminal keeps curving along its arc
    instead of poking out straight). Falls back to linear reflection when the
    run is degenerate or near-straight, so straight terminals stay straight."""
    e1x, e1y = b[0] - a[0], b[1] - a[1]
    e2x, e2y = c[0] - b[0], c[1] - b[1]
    lin = (2 * c[0] - b[0], 2 * c[1] - b[1])
    if math.hypot(e1x, e1y) < 1e-9 or math.hypot(e2x, e2y) < 1e-9:
        return lin
    ang = math.atan2(e1x * e2y - e1y * e2x, e1x * e2x + e1y * e2y)
    if abs(ang) < math.radians(3.0):          # essentially straight
        return lin
    ca, sa = math.cos(ang), math.sin(ang)     # rotate e2 by the same turn
    return (c[0] + e2x * ca - e2y * sa, c[1] + e2x * sa + e2y * ca)


def catmull_rom(p0, p1, p2, p3, tsteps):
    """Centripetal Catmull-Rom points strictly BETWEEN p1 and p2 (exclusive),
    at tsteps-1 interior samples."""
    def tj(ti, a, b):
        d = math.hypot(b[0] - a[0], b[1] - a[1])
        return ti + math.sqrt(d) if d > 1e-12 else ti + 1e-6
    t0 = 0.0
    t1 = tj(t0, p0, p1)
    t2 = tj(t1, p1, p2)
    t3 = tj(t2, p2, p3)
    out = []
    for s in range(1, tsteps):
        t = t1 + (t2 - t1) * (s / tsteps)
        def lerp(a, b, ta, tb):
            if abs(tb - ta) < 1e-12:
                return a
            w = (t - ta) / (tb - ta)
            return (a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w)
        A1 = lerp(p0, p1, t0, t1)
        A2 = lerp(p1, p2, t1, t2)
        A3 = lerp(p2, p3, t2, t3)
        B1 = lerp(A1, A2, t0, t2)
        B2 = lerp(A2, A3, t1, t3)
        out.append(lerp(B1, B2, t1, t2))
    return out


def round_mark(pts):
    """Return an analytic high-resolution circle for a coarse 4-point mark.

    The source font draws dots as four equal-radius cardinal anchors plus the
    repeated first point. Catmull-Rom through those anchors pinches each
    quadrant inward, so it produces a rounded diamond rather than a circle.
    Recognize only a near-square, near-circular four-anchor loop; this excludes
    rectangular outlines such as underscore while preserving every source
    anchor at a SUBDIV boundary.
    """
    core = pts[:-1]
    if len(core) != 4:
        return None
    cx = sum(p[0] for p in core) / 4.0
    cy = sum(p[1] for p in core) / 4.0
    width = max(p[0] for p in core) - min(p[0] for p in core)
    height = max(p[1] for p in core) - min(p[1] for p in core)
    if width < CLOSE_EPS or height < CLOSE_EPS:
        return None
    if not 0.8 <= width / height <= 1.25:
        return None
    radii = [math.hypot(p[0] - cx, p[1] - cy) for p in core]
    radius = sum(radii) / 4.0
    if radius < CLOSE_EPS or max(abs(r - radius) for r in radii) > radius * 0.02:
        return None

    angles = [math.atan2(p[1] - cy, p[0] - cx) for p in core]
    direction = 1.0 if signed_turn(core[-1], core[0], core[1]) > 0 else -1.0
    out = []
    for i, p in enumerate(core):
        a0 = angles[i]
        a1 = angles[(i + 1) % 4]
        while direction * (a1 - a0) <= 0.0:
            a1 += direction * 2.0 * math.pi
        out.append(p)
        for step in range(1, SUBDIV):
            a = a0 + (a1 - a0) * (step / SUBDIV)
            out.append((cx + radius * math.cos(a), cy + radius * math.sin(a)))
    out.append(out[0])
    return out


def corner_flags(turns, closed):
    """Classify each vertex as a hard corner from its SIGNED turn and context.

    A moderate vertex is part of a CURVE when it continues a turn in the same
    rotational direction as a neighbour. It is a CORNER when the bend is sharp,
    isolated (both neighbours straight, e.g. the arm/stem junction of Y/K/k),
    or reverses direction relative to its neighbours (a zigzag: Z/N/M/W).
    Four-anchor round marks are recognized before this generic classifier.
    `turns[i]` is the signed turn at vertex i (0 for open-path endpoints)."""
    n = len(turns)

    def turning(j):
        return abs(turns[j]) > STRAIGHT_DEG

    hard = [False] * n
    for i in range(n):
        t = turns[i]
        if abs(t) <= STRAIGHT_DEG:
            continue                       # straight -> spline through it
        if abs(t) >= REVERSAL_DEG:
            hard[i] = True                 # cusp / fold-back
            continue
        if abs(t) >= CORNER_DEG:
            hard[i] = True                 # a real corner, not a coarse curve
            continue
        if closed:
            nbrs = ((i - 1) % n, (i + 1) % n)
        else:
            nbrs = tuple(j for j in (i - 1, i + 1) if 0 <= j < n)
        # a curve point continues a same-direction turn on at least one side
        same_dir = any(turning(j) and (turns[j] > 0) == (t > 0) for j in nbrs)
        hard[i] = not same_dir
    return hard


def smooth_strip(pts):
    """Resample one stroke polyline, preserving anchors and corners."""
    n = len(pts)
    if n < 3 or SUBDIV < 2:
        return pts
    closed = math.hypot(pts[0][0] - pts[-1][0], pts[0][1] - pts[-1][1]) < CLOSE_EPS

    if closed:
        # periodic ring; core is pts[0..n-2] (drop duplicated closing vertex)
        core = pts[:-1]
        m = len(core)
        if m < 3:
            return pts
        rounded = round_mark(pts)
        if rounded is not None:
            return rounded
        turns = [signed_turn(core[(i - 1) % m], core[i], core[(i + 1) % m])
                 for i in range(m)]
        hard = corner_flags(turns, closed=True)
        out = []
        for i in range(m):
            p1 = core[i]
            p2 = core[(i + 1) % m]
            out.append(p1)
            # smooth this segment only if neither endpoint is a hard corner
            if not hard[i] and not hard[(i + 1) % m]:
                p0 = core[(i - 1) % m]
                p3 = core[(i + 2) % m]
                out.extend(catmull_rom(p0, p1, p2, p3, SUBDIV))
        out.append(out[0])  # re-close
        return out

    # A comma/semicolon joins its four-anchor dot loop and tail in one strip.
    # Round that leading mark independently so the tail cannot distort it.
    for end in range(3, n - 1):
        if math.hypot(pts[0][0] - pts[end][0], pts[0][1] - pts[end][1]) < CLOSE_EPS:
            rounded = round_mark(pts[:end + 1])
            if rounded is not None:
                tail = smooth_strip(pts[end:])
                return rounded + tail[1:]

    # Open polyline. The two free terminals are smooth anchors, NOT corners:
    # a stroke end has no turn to measure, and forcing it hard would leave the
    # first/last segment straight (visible flat tips on c, s, e, ...). Only
    # interior corners break the spline; terminal segments curve into the tip
    # via an arc-continued phantom control point below.
    turns = [0.0] * n
    for i in range(1, n - 1):
        turns[i] = signed_turn(pts[i - 1], pts[i], pts[i + 1])
    hard = corner_flags(turns, closed=False)
    out = []
    for i in range(n - 1):
        p1, p2 = pts[i], pts[i + 1]
        out.append(p1)
        dx, dy = p2[0] - p1[0], p2[1] - p1[1]
        edge_len = math.hypot(dx, dy)
        axis_aligned = abs(dx) < CLOSE_EPS or abs(dy) < CLOSE_EPS
        long_terminal = False
        if axis_aligned and i == 0 and n > 2:
            next_len = math.hypot(pts[2][0] - p2[0], pts[2][1] - p2[1])
            long_terminal = edge_len > LONG_END_RATIO * next_len
        elif axis_aligned and i == n - 2 and n > 2:
            prev_len = math.hypot(p1[0] - pts[i - 1][0], p1[1] - pts[i - 1][1])
            long_terminal = edge_len > LONG_END_RATIO * prev_len
        if not hard[i] and not hard[i + 1] and not long_terminal:
            # Phantom controls past a free terminal follow the arc (preserve
            # curvature into the tip), not a straight reflection.
            p0 = pts[i - 1] if i - 1 >= 0 else arc_extrapolate(pts[2], pts[1], pts[0])
            p3 = pts[i + 2] if i + 2 < n else arc_extrapolate(pts[n - 3], pts[n - 2], pts[n - 1])
            out.extend(catmull_rom(p0, p1, p2, p3, SUBDIV))
    out.append(pts[-1])
    return out


def fnum(v):
    """Match the source formatting: trimmed float with an 'f' suffix.
    Always keep a decimal point so integers stay valid C float literals
    (e.g. 100.0f, not the invalid 100f)."""
    s = '{:.4f}'.format(v).rstrip('0')
    if s.endswith('.'):
        s += '0'
    return s + 'f'


HEADER = '''/*
 * Copyright (c) 1999-2000 Pawel W. Olszta. All Rights Reserved.
 * Written by Pawel W. Olszta, <olszta@sourceforge.net>
 * Creation date: Thu Dec 16 1999
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PAWEL W. OLSZTA BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* freeglut high-resolution {name} stroke font definition */
/* This file has been automatically generated by the genstroke_hi utility from
 * {src}: curved strokes are resampled with a corner-preserving
 * centripetal Catmull-Rom spline (same anchor points, metrics and advance
 * widths as {glut}; only the sampling density of curves differs).
 * Do not edit by hand -- re-run genstroke_hi.py. */

#include <GL/freeglut.h>
#ifdef FREEGLUT_GEOMETRY_STANDALONE
#  include "standalone/fg_glutshapes_shim.h"
#else
#  include "fg_internal.h"
#endif
'''


def main():
    import os
    src, dst = sys.argv[1], sys.argv[2]
    with open(src) as f:
        text = f.read()
    base, name, quantity, height, chars = parse_font(text)
    hibase = base + 'Hi'                      # e.g. RomanHi / MonoRomanHi
    glut = 'GLUT_STROKE_' + camel_snake(name)

    out = [HEADER.format(name=name, src=os.path.basename(src), glut=glut)]
    codes = sorted(chars)
    base_v = sum(len(s) for _, ss in chars.values() for s in ss)
    base_segments = sum(max(0, len(s) - 1) for _, ss in chars.values() for s in ss)
    strip_count = sum(len(ss) for _, ss in chars.values())
    hi_v = 0
    hi_segments = 0
    for code in codes:
        right, strips = chars[code]
        out.append('\n/* char: 0x%02x */\n' % code)
        strip_names = []
        for k, pts in enumerate(strips):
            if not pts:                       # NULL strips (space) carry no verts
                continue
            hp = smooth_strip(pts)
            hi_v += len(hp)
            hi_segments += max(0, len(hp) - 1)
            nm = 'Stroke%s_ch%dst%d' % (hibase, code, k)
            strip_names.append((nm, len(hp)))
            out.append('static const SFG_StrokeVertex %s[] =\n{\n' % nm)
            out.append(''.join(' {%s,%s},\n' % (fnum(x), fnum(y)) for x, y in hp))
            out.append('};\n\n')
        stripnm = 'Stroke%s_ch%dst' % (hibase, code)
        out.append('static const SFG_StrokeStrip %s[] =\n{\n' % stripnm)
        if strip_names:
            out.append(''.join(' {%d,%s},\n' % (cnt, nm) for nm, cnt in strip_names))
        else:
            out.append('  { 0, NULL }\n')
        out.append('};\n\n')
        out.append('static const SFG_StrokeChar Stroke%s_ch%d = {%s,%d,%s};\n'
                   % (hibase, code, fnum(right), len(strip_names), stripnm))

    # chars[] table: 0..quantity-1, NULL below the first present code.
    out.append('\nstatic const SFG_StrokeChar *Stroke%s_chars[] =\n{\n' % hibase)
    row = []
    for i in range(quantity):
        cell = '&Stroke%s_ch%d' % (hibase, i) if i in chars else '0'
        row.append(cell)
        if len(row) == 8:
            out.append(' ' + ', '.join(row) + ',\n')
            row = []
    if row:
        out.append(' ' + ', '.join(row) + '\n')
    out.append('};\n\n')
    out.append('SFG_StrokeFont fgStroke%s = {"%s",%d,%sf,Stroke%s_chars};\n'
               % (hibase, hibase, quantity, height, hibase))

    with open(dst, 'w') as f:
        f.write(''.join(out))

    sys.stderr.write('genstroke_hi: %d glyphs, %d strips; %d -> %d vertices, '
                     '%d -> %d line segments (%.2fx; SUBDIV=%d)\n'
                     % (len(codes), strip_count, base_v, hi_v, base_segments,
                        hi_segments, hi_segments / base_segments, SUBDIV))


if __name__ == '__main__':
    main()
