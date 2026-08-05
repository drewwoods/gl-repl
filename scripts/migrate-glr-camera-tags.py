#!/usr/bin/env python3
"""Migrate .glr scenes to tagged camera rows + the canonical document order.

One-shot migration tool for docs/plans/active/camera-header-tags.md phase 7.
Kept in-tree because the corpus is 46 files and the edit has to be reviewable
as a diff rather than as 46 hand edits: what it does is mechanical, and the
parts that are *not* mechanical (a dynamic yaw the format cannot express, a
pan offset folded into the dist line) it refuses to touch and reports instead.

  scripts/migrate-glr-camera-tags.py examples/scenes/*.glr
"""
import re
import sys

CFG_RE = re.compile(r'^\s*//\s*@(cfg|scene-name|workspace-dir)\b')
TRANSLATE_RE = re.compile(
    r'^\s*glTranslatef\(\s*(-?[\d.]+)f?\s*,\s*(-?[\d.]+)f?\s*,'
    r'\s*(-?[\d.]+)f?\s*\)\s*;\s*$')
ROTATE_RE = re.compile(
    r'^\s*glRotatef\(\s*(-?[\d.]+)f?\s*,\s*(-?[\d.]+)f?\s*,'
    r'\s*(-?[\d.]+)f?\s*,\s*(-?[\d.]+)f?\s*\)\s*;\s*$')
DECL_RE = re.compile(r'^\s*(static\s+)?float\s')
CONTROL_RE = re.compile(r'^\s*(for|if|while|else)\b')
FUNCDEF_RE = re.compile(r'^\s*[A-Za-z_]\w*\s*(\([^)]*\))?\s*\{\s*(//.*)?$')


def is_blank_or_comment(line):
    s = line.strip()
    return s == '' or s.startswith('//')


def find_camera_block(lines):
    """Index of the first line of a canonical 4-row camera block, or None."""
    for i in range(len(lines) - 3):
        m0 = TRANSLATE_RE.match(lines[i])
        m1 = ROTATE_RE.match(lines[i + 1])
        m2 = ROTATE_RE.match(lines[i + 2])
        m3 = TRANSLATE_RE.match(lines[i + 3])
        if not (m0 and m1 and m2 and m3):
            continue
        if abs(float(m0.group(1))) > 1e-4 or abs(float(m0.group(2))) > 1e-4:
            continue                       # a pan offset folded into dist
        if [float(m1.group(j)) for j in (2, 3, 4)] != [1.0, 0.0, 0.0]:
            continue
        if [float(m2.group(j)) for j in (2, 3, 4)] != [0.0, 1.0, 0.0]:
            continue
        return i
    return None


def tag(line, role):
    return '%s   // @camera %s' % (line.rstrip().rstrip(), role)


def classify(chunk):
    head = chunk[0]
    if DECL_RE.match(head):
        return 'decls'
    if not CONTROL_RE.match(head) and FUNCDEF_RE.match(head):
        return 'funcs'
    return 'body'


def split_chunks(lines):
    """[(leading comment/blank run, code lines, kind)] over the body region."""
    chunks = []
    pending = []
    i = 0
    while i < len(lines):
        if is_blank_or_comment(lines[i]):
            pending.append(lines[i])
            i += 1
            continue
        depth = lines[i].count('{') - lines[i].count('}')
        code = [lines[i]]
        i += 1
        while depth > 0 and i < len(lines):
            depth += lines[i].count('{') - lines[i].count('}')
            code.append(lines[i])
            i += 1
        chunks.append((pending, code, classify(code)))
        pending = []
    if pending:
        chunks.append((pending, [], 'body'))
    return chunks


def migrate(path):
    with open(path) as f:
        lines = f.read().split('\n')
    if lines and lines[-1] == '':
        lines.pop()

    head = 0
    while head < len(lines) and CFG_RE.match(lines[head]):
        head += 1
    header, rest = lines[:head], lines[head:]

    cam_at = find_camera_block(rest)
    camera = []
    if cam_at is not None:
        # Drop the block's own `// camera` banner. It is an ordinary comment
        # now, and a banner reading "--- Camera ---" left floating above the
        # body it no longer heads is worse than no banner: the tags are
        # self-describing, which is the whole argument for having them.
        j = cam_at - 1
        while j >= 0 and rest[j].strip() == '':
            j -= 1
        if j >= 0 and re.fullmatch(r'[^a-zA-Z]*camera[^a-zA-Z]*',
                                   rest[j].strip().lstrip('/').strip(),
                                   re.IGNORECASE):
            rest = rest[:j] + rest[j + 1:]
            cam_at -= 1
        camera = [tag(rest[cam_at], 'dist'),
                  tag(rest[cam_at + 1], 'rx'),
                  tag(rest[cam_at + 2], 'ry'),
                  tag(rest[cam_at + 3], 'pan')]
        rest = rest[:cam_at] + rest[cam_at + 4:]

    chunks = split_chunks(rest)
    out = list(header)
    for kind in ('decls', 'funcs'):
        for lead, code, k in chunks:
            if k != kind:
                continue
            out.extend(x for x in lead if x.strip() != '' or out)
            out.extend(code)
        if out and out[-1].strip() != '':
            out.append('')
    out.extend(camera)
    if camera:
        out.append('')
    for lead, code, k in chunks:
        if k != 'body':
            continue
        out.extend(lead)
        out.extend(code)

    # Collapse runs of blank lines the reshuffle can create.
    collapsed = []
    for line in out:
        if line.strip() == '' and collapsed and collapsed[-1].strip() == '':
            continue
        collapsed.append(line)
    while collapsed and collapsed[-1].strip() == '':
        collapsed.pop()

    with open(path, 'w') as f:
        f.write('\n'.join(collapsed) + '\n')
    return cam_at is not None


def main(argv):
    untagged = []
    for path in argv[1:]:
        if not migrate(path):
            untagged.append(path)
    if untagged:
        print('no canonical camera block found (needs a hand decision):')
        for path in untagged:
            print('  ' + path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
