"""pbrt -> Blender scene importer (v1 subset).

Runs INSIDE Blender (sent via the blender-mcp socket as execute_code).
Supports: Film/Camera/LookAt/Scale/Rotate/Translate/Identity/WorldBegin,
AttributeBegin/End, Shape "trianglemesh" (point3 P, integer indices,
optional point2 uv), Material diffuse|coateddiffuse, LightSource
point|distant|infinite, Include. Transforms are baked into vertices.
ObjectBegin/Instance and exotic materials are skipped (v1).
"""
import bpy
import mathutils
import os
import math


class _Tok:
    def __init__(self, text):
        self.t = self._tokens(text)
        self.i = 0

    @staticmethod
    def _tokens(text):
        out, i, n = [], 0, len(text)
        while i < n:
            c = text[i]
            if c == '#':
                while i < n and text[i] != '\n':
                    i += 1
            elif c.isspace():
                i += 1
            elif c == '"':
                j = text.index('"', i + 1)
                out.append(('str', text[i + 1:j]))
                i = j + 1
            elif c in '[]':
                out.append((c, c))
                i += 1
            else:
                j = i
                while j < n and not text[j].isspace() and text[j] not in '[]"#':
                    j += 1
                out.append(('w', text[i:j]))
                i = j
        return out

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def next(self):
        v = self.peek()
        self.i += 1
        return v


def _parse_params(tk):
    """Read name/value pairs: "name" then str | word | [ ... ]."""
    params = {}
    while True:
        p = tk.peek()
        if p is None or p[0] != 'str':
            break
        tk.next()
        name = p[1]
        v = tk.next()
        if v[0] == '[':
            arr = []
            while True:
                x = tk.next()
                if x[0] == ']':
                    break
                arr.append(x[1])
            params[name] = ('array', arr)
        else:
            params[name] = ('val', v[1])
    return params


def _f(arr, k):
    return float(arr[k]) if k < len(arr) else 0.0


def import_pbrt(path, collection_name="PBRT"):
    with open(path, 'r', encoding='utf-8', errors='ignore') as fh:
        text = fh.read()
    tk = _Tok(text)

    # fresh collection
    if collection_name in bpy.data.collections:
        coll = bpy.data.collections[collection_name]
    else:
        coll = bpy.data.collections.new(collection_name)
        bpy.context.scene.collection.children.link(coll)

    state = {'ctm': mathutils.Matrix.Identity(4),
             'stack': [],
             'cam': None, 'lookat': None,
             'res': (640, 360),
             'mat': None, 'named': {},
             'count': 0}

    def make_mesh(name, P, idx, uv):
        me = bpy.data.meshes.new(name)
        pts = []
        m = state['ctm']
        n = 0
        while n + 2 < len(P):
            v = m @ mathutils.Vector((_f(P, n), _f(P, n + 1), _f(P, n + 2)))
            pts.append(v[:])
            n += 3
        faces = [tuple(idx[i:i + 3]) for i in range(0, len(idx) - 2, 3)]
        faces = [f for f in faces if max(f) < len(pts)]
        me.from_pydata(pts, [], faces)
        if uv:
            uvl = me.uv_layers.new(name="UVMap")
            for poly in me.polygons:
                for li in poly.loop_indices:
                    vi = me.loops[li].vertex_index
                    uvl.data[li].uv = (_f(uv, 2 * vi), _f(uv, 2 * vi + 1))
        ob = bpy.data.objects.new(name, me)
        if state['mat']:
            ob.data.materials.append(state['mat'])
        coll.objects.link(ob)
        state['count'] += 1
        return ob

    def make_material(typ, prm):
        mat = bpy.data.materials.get('pbrt_' + typ + '_' +
                                     str(len(bpy.data.materials)))
        mat = bpy.data.materials.new('pbrt_' + typ)
        mat.use_nodes = True
        bsdf = mat.node_tree.nodes.get('Principled BSDF')
        refl = prm.get('rgb reflectance') or prm.get('rgb Kd')
        if refl and bsdf:
            a = refl[1]
            bsdf.inputs['Base Color'].default_value = (
                _f(a, 0), _f(a, 1), _f(a, 2), 1.0)
        rough = prm.get('float roughness')
        if rough and bsdf:
            try:
                bsdf.inputs['Roughness'].default_value = float(rough[1][0])
            except Exception:
                pass
        return mat

    def stmt(kw, tk):
        if kw in ('Scale', 'Translate'):
            a = [tk.next()[1] for _ in range(3)]
            x, y, z = map(float, a)
            M = (mathutils.Matrix.Scale(x, 4, (1, 0, 0)) @
                 mathutils.Matrix.Scale(y, 4, (0, 1, 0)) @
                 mathutils.Matrix.Scale(z, 4, (0, 0, 1))) \
                if kw == 'Scale' else mathutils.Matrix.Translation((x, y, z))
            state['ctm'] = state['ctm'] @ M
        elif kw == 'Rotate':
            a = [tk.next()[1] for _ in range(4)]
            ang, ax, ay, az = map(float, a)
            state['ctm'] = state['ctm'] @ mathutils.Matrix.Rotation(
                math.radians(ang), 4, (ax, ay, az))
        elif kw == 'Identity':
            state['ctm'] = mathutils.Matrix.Identity(4)
        elif kw == 'LookAt':
            a = [tk.next()[1] for _ in range(9)]
            f = list(map(float, a))
            state['lookat'] = f
        elif kw in ('Camera', 'Film', 'Sampler', 'Integrator',
                    'PixelFilter', 'Option'):
            prm = _parse_params(tk)
            if kw == 'Camera':
                state['cam'] = prm
            elif kw == 'Film':
                rx = prm.get('integer xresolution')
                ry = prm.get('integer yresolution')
                if rx:
                    state['res'] = (int(rx[1][0]), int(ry[1][0]))
        elif kw == 'WorldBegin':
            state['ctm'] = mathutils.Matrix.Identity(4)
        elif kw == 'WorldEnd':
            pass
        elif kw == 'AttributeBegin':
            state['stack'].append((state['ctm'], state['mat']))
        elif kw == 'AttributeEnd':
            if state['stack']:
                state['ctm'], state['mat'] = state['stack'].pop()
        elif kw == 'Material':
            typ = tk.next()[1]  # type token
            prm = _parse_params(tk)
            state['mat'] = make_material(typ, prm)
        elif kw == 'MakeNamedMaterial':
            tk.next()  # name token
            prm = _parse_params(tk)
            nm = prm.pop('string name', ('val', 'mat'))[1]
            typ = prm.pop('string type', ('val', 'diffuse'))[1]
            state['named'][nm] = make_material(typ, prm)
        elif kw == 'NamedMaterial':
            nm = tk.next()[1]
            state['mat'] = state['named'].get(nm)
        elif kw == 'AreaLightSource':
            tk.next()  # type token
            _parse_params(tk)
        elif kw == 'LightSource':
            typ = tk.next()[1]  # type token
            prm = _parse_params(tk)
            typ = typ
            inten = prm.get('rgb I') or prm.get('rgb L')
            e = 1000.0
            if inten:
                e = 1000.0 * sum(map(float, inten[1][:3])) / 3.0
            ld = bpy.data.lights.new('pbrt_' + typ, 'POINT')
            ld.energy = e
            ob = bpy.data.objects.new('pbrt_' + typ, ld)
            frm = prm.get('point3 from')
            if frm:
                ob.location = (_f(frm[1], 0), _f(frm[1], 1), _f(frm[1], 2))
            if typ in ('distant', 'infinite'):
                ld.type = 'SUN'
                ld.energy = max(1.0, e / 50.0)
                w = bpy.data.worlds.get('pbrt_world') or \
                    bpy.data.worlds.new('pbrt_world')
                bpy.context.scene.world = w
                w.use_nodes = True
                bg = w.node_tree.nodes.get('Background')
                if bg and inten:
                    bg.inputs['Color'].default_value = (
                        _f(inten[1], 0), _f(inten[1], 1), _f(inten[1], 2), 1)
            coll.objects.link(ob)
        elif kw == 'Shape':
            typ = tk.next()[1]  # type token
            prm = _parse_params(tk)
            typ = typ
            if typ == 'trianglemesh':
                P = prm.get('point3 P', ('arr', []))[1]
                idx = [int(float(x)) for x in
                       prm.get('integer indices', ('arr', []))[1]]
                uvp = prm.get('point2 uv')
                uv = uvp[1] if uvp else []
                make_mesh('shape_%04d' % state['count'], P, idx,
                          list(map(float, uv)))
        elif kw == 'Include':
            fn = tk.next()[1]
            sub = os.path.join(os.path.dirname(path), fn)
            if os.path.exists(sub):
                import_pbrt(sub, collection_name)
        else:
            # unknown keyword: consume its parameter block if any
            while True:
                p = tk.peek()
                if p is None or p[0] != 'str':
                    break
                _parse_params(tk)
                break

    while True:
        p = tk.next()
        if p is None:
            break
        if p[0] == 'w':
            try:
                stmt(p[1], tk)
            except Exception as e:
                print('pbrt_importer: skip', p[1], e)

    # camera
    cam_data = bpy.data.cameras.new('pbrt_camera')
    cam_ob = bpy.data.objects.new('pbrt_camera', cam_data)
    if state['lookat']:
        e = state['lookat'][0:3]
        t = state['lookat'][3:6]
        u = state['lookat'][6:9]
        eye = mathutils.Vector(e)
        z = (eye - mathutils.Vector(t)).normalized()   # camera looks down -Z
        x = mathutils.Vector(u).cross(z).normalized()
        y = z.cross(x)
        cam_ob.matrix_world = mathutils.Matrix((
            (x.x, x.y, x.z, eye.x),
            (y.x, y.y, y.z, eye.y),
            (z.x, z.y, z.z, eye.z),
            (0.0, 0.0, 0.0, 1.0)))
    if state['cam']:
        fov = state['cam'].get('float fov')
        if fov:
            import math as _m
            cam_data.lens = 12.0 / _m.tan(_m.radians(float(fov[1][0])) / 2)
    coll.objects.link(cam_ob)
    bpy.context.scene.camera = cam_ob
    bpy.context.scene.render.resolution_x, bpy.context.scene.render.resolution_y = \
        state['res']
    return {'shapes': state['count'], 'materials': len(state['named'])}



