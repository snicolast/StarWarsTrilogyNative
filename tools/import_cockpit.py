#!/usr/bin/env python3
"""Imports a 3D cockpit for the DOGFIGHT mode from a binary FBX file.

    python tools/import_cockpit.py <model.zip | model.fbx> [models/cockpit]

The game does not come with a cockpit: this takes one the user downloaded
(a Blender-exported binary FBX, alone or inside the zip it was downloaded as,
with its textures beside it) and writes what the game loads at startup:

    models/cockpit/cockpit.mesh    the triangles, one group per object
    models/cockpit/<texture>       the textures, copied as they are

Nothing of the model is kept in the repository (models/ is ignored by git).

Made for and tried with "X-Wing Cockpit 3 (alternative)" by MichaelEGA, of the
Open Galaxy project (https://github.com/MichaelEGA/Open-Galaxy), CC Attribution:
its origin is the pilot's eye, its units centimetres, and its "Screen" object
is where the game draws its own data. SWT_DOGFIGHT_COCKPIT moves the eye.

cockpit.mesh, little-endian:
    char[8]  "SWTMESH1"
    u32      groups
    per group:
        char[32]  name (the FBX object's: "Cockpit", "Screen", "Windows")
        char[64]  texture file name ("" = none)
        u32       vertices (three per triangle)
        per vertex: float position[3], normal[3], uv[2]
Positions in the file's own units and axes (x right, y up, the nose at -z);
v runs downwards (the image's rows).

Only the standard library is used.
"""
import io
import os
import struct
import sys
import zipfile
import zlib

IMAGE_TYPES = ('.jpg', '.jpeg', '.png')


def read_fbx(data):
    """The node tree of a binary FBX: (name, properties, children) tuples."""
    if data[:20] != b'Kaydara FBX Binary  ':
        raise ValueError('not a binary FBX file (ASCII FBX and other formats are not read)')
    wide = struct.unpack_from('<I', data, 23)[0] >= 7500
    scalar = {'Y': ('<h', 2), 'C': ('<?', 1), 'I': ('<i', 4), 'F': ('<f', 4), 'D': ('<d', 8), 'L': ('<q', 8)}
    array = {'f': 'f', 'd': 'd', 'l': 'q', 'i': 'i', 'b': '?'}

    def node(at):
        if wide:
            end, count, _ = struct.unpack_from('<QQQ', data, at)
            at += 24
        else:
            end, count, _ = struct.unpack_from('<III', data, at)
            at += 12
        length = data[at]
        name = data[at + 1:at + 1 + length].decode('latin1')
        at += 1 + length
        if end == 0:
            return None, at
        props = []
        for _ in range(count):
            t = chr(data[at])
            at += 1
            if t in scalar:
                fmt, size = scalar[t]
                props.append(struct.unpack_from(fmt, data, at)[0])
                at += size
            elif t in array:
                n, packed, size = struct.unpack_from('<III', data, at)
                at += 12
                raw = data[at:at + size]
                at += size
                if packed:
                    raw = zlib.decompress(raw)
                props.append(list(struct.unpack('<%d%s' % (n, array[t]), raw)))
            elif t in 'SR':
                n = struct.unpack_from('<I', data, at)[0]
                at += 4
                raw = data[at:at + n]
                at += n
                props.append(raw.decode('latin1') if t == 'S' else raw)
            else:
                raise ValueError('unknown FBX property type %r' % t)
        children = []
        while at < end:
            child, at = node(at)
            if child:
                children.append(child)
        return (name, props, children), end

    top = []
    at = 27
    while at < len(data) - 160:
        n, at = node(at)
        if not n:
            break
        top.append(n)
    return top


def child(node, name):
    for c in node[2]:
        if c[0] == name:
            return c
    return None


def object_name(node):
    return node[1][1].split('\x00')[0] if len(node[1]) > 1 and isinstance(node[1][1], str) else ''


def layer(geometry, element, values, indices, width):
    """A per-polygon-vertex reader of one layer element (normals, UVs)."""
    e = child(geometry, element)
    if not e:
        return None
    data = child(e, values)[1][0]
    mapping = child(e, 'MappingInformationType')[1][0]
    reference = child(e, 'ReferenceInformationType')[1][0]
    index = child(e, indices)[1][0] if reference == 'IndexToDirect' and child(e, indices) else None

    def get(corner, vertex):
        i = corner if mapping == 'ByPolygonVertex' else vertex if mapping in ('ByVertice', 'ByVertex') else 0
        if index is not None:
            i = index[i]
        return data[i * width:i * width + width]
    return get


def triangles(geometry):
    """The geometry as a flat list of (position, normal, uv) per triangle corner."""
    points = child(geometry, 'Vertices')[1][0]
    index = child(geometry, 'PolygonVertexIndex')[1][0]
    normal = layer(geometry, 'LayerElementNormal', 'Normals', 'NormalsIndex', 3)
    uv = layer(geometry, 'LayerElementUV', 'UV', 'UVIndex', 2)
    out = []
    polygon = []
    for corner, i in enumerate(index):
        last = i < 0
        if last:
            i = ~i
        p = points[i * 3:i * 3 + 3]
        n = normal(corner, i) if normal else None
        t = uv(corner, i) if uv else (0.0, 0.0)
        polygon.append((p, n, (t[0], 1.0 - t[1])))
        if not last:
            continue
        for k in range(1, len(polygon) - 1):   # a fan (the polygons of these models are convex)
            tri = [polygon[0], polygon[k], polygon[k + 1]]
            if tri[0][1] is None:   # no normals in the file: the face's
                a, b, c = tri[0][0], tri[1][0], tri[2][0]
                u = [b[j] - a[j] for j in range(3)]
                v = [c[j] - a[j] for j in range(3)]
                f = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
                length = sum(x * x for x in f) ** 0.5 or 1.0
                tri = [(q[0], [x / length for x in f], q[2]) for q in tri]
            out.extend(tri)
        polygon = []
    return out


def convert(fbx):
    """[(object name, texture base name, vertices)] of an FBX file's meshes."""
    top = read_fbx(fbx)
    objects = next(n for n in top if n[0] == 'Objects')
    by_id = {o[1][0]: o for o in objects[2] if o[1] and isinstance(o[1][0], int)}
    links = [c[1] for c in next(n for n in top if n[0] == 'Connections')[2]]

    def sources(target, kind):
        return [by_id[c[1]] for c in links if c[2] == target and c[1] in by_id and by_id[c[1]][0] == kind]

    groups = []
    for model in (o for o in objects[2] if o[0] == 'Model'):
        for geometry in sources(model[1][0], 'Geometry'):
            texture = ''
            for material in sources(model[1][0], 'Material'):
                for tex in sources(material[1][0], 'Texture'):
                    name = child(tex, 'RelativeFilename') or child(tex, 'FileName')
                    if name and not texture:
                        texture = os.path.basename(name[1][0].replace('\\', '/'))
            groups.append((object_name(model), texture, triangles(geometry)))
    return groups


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    source = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join('models', 'cockpit')

    # The FBX and the images beside it, from a folder or from a zip (and the zips inside it)
    files = {}

    def take_zip(z):
        for name in z.namelist():
            low = name.lower()
            if low.endswith('.zip'):
                take_zip(zipfile.ZipFile(io.BytesIO(z.read(name))))
            elif low.endswith('.fbx') or low.endswith(IMAGE_TYPES):
                files.setdefault(os.path.basename(name), z.read(name))

    if source.lower().endswith('.zip'):
        take_zip(zipfile.ZipFile(source))
    else:
        folder = os.path.dirname(os.path.abspath(source))
        for root, _, names in os.walk(folder):
            for name in names:
                if name.lower().endswith(IMAGE_TYPES) or os.path.join(root, name) == os.path.abspath(source):
                    with open(os.path.join(root, name), 'rb') as f:
                        files.setdefault(name, f.read())
    fbx = [n for n in files if n.lower().endswith('.fbx')]
    if not fbx:
        print('No .fbx file found in', source)
        return 1
    images = {os.path.splitext(n)[0].lower(): n for n in files if n.lower().endswith(IMAGE_TYPES)}

    groups = convert(files[fbx[0]])
    os.makedirs(out_dir, exist_ok=True)
    main_image = ''
    with open(os.path.join(out_dir, 'cockpit.mesh'), 'wb') as out:
        out.write(b'SWTMESH1' + struct.pack('<I', len(groups)))
        for name, texture, verts in groups:
            # The file names the texture as its author's disk had it: the image
            # beside the model with that name, or else the biggest one whose
            # name starts like the model's (and is not a variant: "_light")
            image = images.get(os.path.splitext(texture)[0].lower(), '') if texture else ''
            if texture and not image:
                if not main_image:
                    stem = os.path.splitext(fbx[0])[0].lower().split('_')[1:2] or ['']
                    plain = [n for n in images.values() if stem[0] in n.lower() and 'light' not in n.lower()]
                    plain = plain or list(images.values())
                    main_image = max(plain, key=lambda n: len(files[n])) if plain else ''
                image = main_image
            out.write(struct.pack('<32s64sI', name.encode('latin1')[:31], image.encode('latin1')[:63], len(verts)))
            for p, n, t in verts:
                out.write(struct.pack('<8f', p[0], p[1], p[2], n[0], n[1], n[2], t[0], t[1]))
            print('%-10s %6d triangles  texture: %s' % (name, len(verts) // 3, image or '(none)'))
    for name in images.values():
        with open(os.path.join(out_dir, name), 'wb') as f:
            f.write(files[name])
    print('Written to', out_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
