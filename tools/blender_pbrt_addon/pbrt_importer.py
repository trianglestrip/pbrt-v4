"""pbrt -> Blender scene importer (standalone addon + MCP-injectable).

This module is both:
  * a Blender addon exposing ``File > Import > PBRT Scene (.pbrt)``
    (with ``bl_info`` + the ``IMPORT_OT_pbrt`` operator), and
  * a script that the blender-mcp server can ``exec()`` to call
    ``import_pbrt()`` directly. Both paths share the same parsing core.

Supports: Film/Camera/LookAt/Scale/Rotate/Translate/Identity/WorldBegin,
AttributeBegin/End, Shape "trianglemesh"/"plymesh", Material,
MakeNamedMaterial/NamedMaterial, Texture (named imagemap), LightSource,
AreaLightSource, Include. Transforms are baked into vertices.

Material model (mirrors the assetstream PbrtParser material capture):
  - named textures declared via ``Texture "name" "spectrum" "imagemap"
    "string filename" [...]`` are recorded in a name->path table, and a
    material's ``"texture reflectance" "rgb:name"`` reference is resolved
    through that table to the real image file (the previous version used
    the texture reference name directly as a file path, so every base
    color silently fell back to grey -- the "materials don't match" bug).
  - reflectance/Kd -> Base Color (texture or rgb), normalmap/bumpmap ->
    Normal Map node, roughness, metallic (metal/conductor), transmission
    + IOR (glass/dielectric), tint -> Base Color for glass.
"""

import bpy
import mathutils
import os
import math


# ---------------------------------------------------------------------------
# PLY reader (binary, little-endian)
# ---------------------------------------------------------------------------
def _read_ply(filepath):
    """Read a binary PLY file and return (P, indices, N, uv)."""
    import struct
    with open(filepath, 'rb') as f:
        data = f.read()
    end = data.index(b'end_header') + len(b'end_header\n')
    header = data[:end].decode('ascii')
    lines = header.split('\n')
    vcount = fcount = prop_count = 0
    for l in lines:
        if 'element vertex' in l:
            vcount = int(l.split()[-1])
        elif 'element face' in l:
            fcount = int(l.split()[-1])
        elif l.strip().startswith('property float') and 'element face' not in header[:header.index(l)]:
            prop_count += 1
    stride = prop_count * 4
    vertex_size = vcount * stride
    vd = data[end:end + vertex_size]
    fd = data[end + vertex_size:]
    P, N, uv = [], [], []
    for i in range(vcount):
        off = i * stride
        vals = struct.unpack_from(f'{prop_count}f', vd, off)
        P.extend(vals[:3])
        if prop_count >= 6:
            N.extend(vals[3:6])
        if prop_count >= 8:
            uv.extend(vals[6:8])
    indices, pos = [], 0
    for i in range(fcount):
        cnt = fd[pos]; pos += 1
        face = struct.unpack_from(f'{cnt}i', fd, pos)
        pos += cnt * 4
        indices.extend(face[:3] if cnt == 3 else face)
    return P, indices, N, uv


def _import_ply(ply_path, state):
    """Import a PLY file into Blender with CTM baked into vertices."""
    P, idx, N, uv = _read_ply(ply_path)
    me = bpy.data.meshes.new('ply_mesh')
    m = state['ctm']
    verts = []
    n = 0
    while n + 2 < len(P):
        v = m @ mathutils.Vector((_f(P, n), _f(P, n + 1), _f(P, n + 2)))
        verts.append(v[:])
        n += 3
    faces = [tuple(idx[i:i + 3]) for i in range(0, len(idx) - 2, 3)]
    faces = [f for f in faces if max(f) < len(verts)]
    me.from_pydata(verts, [], faces)
    if uv:
        uvl = me.uv_layers.new(name='UVMap')
        for poly in me.polygons:
            for li in poly.loop_indices:
                vi = me.loops[li].vertex_index
                uvl.data[li].uv = (_f(uv, 2 * vi), _f(uv, 2 * vi + 1))
    ob = bpy.data.objects.new('ply_%04d' % state['count'], me)
    if state['mat']:
        ob.data.materials.append(state['mat'])
    bpy.context.scene.collection.objects.link(ob)
    state['count'] += 1


# ---------------------------------------------------------------------------
# Tokenizer / param parser
# ---------------------------------------------------------------------------
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
    """Read a pbrt param block: '"type name" value-or-[...]'.

    Returns dict keyed by param NAME. The value is a (type, value) tuple.
    type is the first word of the declaration (e.g. 'texture', 'rgb',
    'string', 'float', 'point3'); value is either a single scalar/string
    (for bare tokens) or a list (for bracketed arrays).
    """
    params = {}
    while True:
        p = tk.peek()
        if p is None or p[0] != 'str':
            break
        tk.next()
        decl = p[1]  # e.g. "texture reflectance", "float roughness", "point3 P"
        parts = decl.split(None, 1)
        ptype = parts[0] if len(parts) > 1 else ''
        pname = parts[1] if len(parts) > 1 else decl
        v = tk.next()
        if v is None:
            break
        if v[0] == '[':
            arr = []
            while True:
                x = tk.next()
                if x is None or x[0] == ']':
                    break
                arr.append(x[1])
            params[pname] = (ptype, arr)
        else:
            params[pname] = (ptype, v[1])
    return params


def _f(arr, k):
    return float(arr[k]) if k < len(arr) else 0.0


# ---------------------------------------------------------------------------
# pbrt value helpers (operate on (type, value) tuples from _parse_params)
# ---------------------------------------------------------------------------
def _param_str(v):
    if v is None:
        return ''
    val = v[1]
    if isinstance(val, list):
        return val[0] if val else ''
    return val


def _param_float(v, default=0.0):
    if v is None:
        return default
    val = v[1]
    if isinstance(val, list):
        return float(val[0]) if val else default
    try:
        return float(val)
    except (TypeError, ValueError):
        return default


def _clamp01(x):
    return 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)


def _strip_tex_prefix(name):
    """pbrt texture references are written as 'rgb:path' / 'spectrum:path'
    / 'float:path'. Drop the type prefix, leaving the bare path."""
    if ':' in name:
        head = name.split(':', 1)[0]
        if head in ('rgb', 'color', 'spectrum', 'float', 'alpha',
                    'blackbody', 'fourier'):
            return name.split(':', 1)[1]
    return name


_IMG_EXTS = ('.png', '.jpg', '.jpeg', '.dds', '.exr', '.tif', '.tiff',
             '.bmp', '.tga')


def _resolve_image(relpath, scene_dir):
    """Resolve a pbrt texture path (relative, optional extension) to an
    existing absolute file path, or None."""
    if not relpath:
        return None
    base = relpath
    if not os.path.isabs(base):
        base = os.path.join(scene_dir, relpath)
    if os.path.exists(base):
        return base
    for ext in _IMG_EXTS:
        if os.path.exists(base + ext):
            return base + ext
    return None


def _load_image(path, name):
    if not path or not os.path.exists(path):
        return None
    try:
        img = bpy.data.images.load(path, check_existing=True)
        try:
            img['pbrt_import'] = True
        except Exception:
            pass
        return img
    except Exception:
        return None


def _image_is_blank(img, max_samples=3000):
    """Return True if an image failed to decode (Blender reads all-zero
    pixels). Some .exr/.hdr skies in this environment decode to blank,
    which would leave the scene unlit/black; detect that and fall back."""
    if img is None:
        return True
    try:
        px = img.pixels
        n = len(px)
        if n == 0:
            return True
        step = max(1, n // (max_samples * 4)) * 4
        m = 0.0
        for i in range(0, min(n, max_samples * 4), step):
            if px[i] > m:
                m = px[i]
            if m > 0.0:
                break
        return m <= 0.0
    except Exception:
        return True


def _bsdf_input(bsdf, *names):
    for n in names:
        if n in bsdf.inputs:
            return bsdf.inputs[n]
    return None


def _add_image_node(nt, img, x, y, as_data=False):
    node = nt.nodes.new('ShaderNodeTexImage')
    node.image = img
    node.location = (x, y)
    if as_data and img is not None:
        try:
            img.colorspace_settings.is_data = True
        except Exception:
            pass
    return node


# ---------------------------------------------------------------------------
# Main importer
# ---------------------------------------------------------------------------
def import_pbrt(path, collection_name="PBRT", state=None):
    with open(path, 'r', encoding='utf-8', errors='ignore') as fh:
        text = fh.read()
    tk = _Tok(text)

    # --- cleanup: start from a clean scene so the import fully replaces it ---
    # On a top-level import (state is None) we wipe meshes, lights, cameras,
    # worlds, images and materials as well as the target collection, so a
    # re-import rebuilds everything (and never reuses stale/blank textures).
    # Default Blender lights/cameras are also removed -- only PBRT-provided
    # ones should remain. Linked/script data a user added elsewhere is
    # intentionally left alone only when it is not imported scene data.
    if state is None:
        for o in list(bpy.data.objects):
            if o.type in ('MESH', 'LIGHT', 'CAMERA', 'EMPTY'):
                bpy.data.objects.remove(o, do_unlink=True)
        for cam in list(bpy.data.cameras):
            bpy.data.cameras.remove(cam)
        for lt in list(bpy.data.lights):
            bpy.data.lights.remove(lt)
        for w in list(bpy.data.worlds):
            if w.name != 'World':
                bpy.data.worlds.remove(w)
        for img in list(bpy.data.images):
            if img.name not in ('Render Result', 'Viewer Node'):
                bpy.data.images.remove(img)
        for m in list(bpy.data.materials):
            bpy.data.materials.remove(m)
        if collection_name in bpy.data.collections:
            bpy.data.collections.remove(bpy.data.collections[collection_name])

    # fresh collection
    if collection_name in bpy.data.collections:
        coll = bpy.data.collections[collection_name]
    else:
        coll = bpy.data.collections.new(collection_name)
        bpy.context.scene.collection.children.link(coll)

    if state is None:
        state = {'ctm': mathutils.Matrix.Identity(4),
                 'stack': [],
                 'cam': None, 'lookat': None,
                 'res': (640, 360),
                 'mat': None, 'named': {},
                 'textures': {},            # named texture -> relative path
                 'scene_dir': os.path.dirname(os.path.abspath(path)),
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

    def make_material(name, typ, prm):
        """Build a Principled-BSDF material from a pbrt material block.

        `name` is the named-material id (used for dedupe across imports);
        pass None for inline materials.
        """
        if name and name in bpy.data.materials:
            existing = bpy.data.materials[name]
            if existing.use_nodes:
                # Reuse only if it is a PBRT import AND its texture maps are
                # still wired up. A material whose TEX_IMAGE nodes lost their
                # image (e.g. after a scene reload) must be rebuilt, otherwise
                # re-importing silently keeps blank textures.
                broken = False
                if existing.get('pbrt_import'):
                    for n in existing.node_tree.nodes:
                        if n.type == 'TEX_IMAGE' and n.image is None:
                            broken = True
                            break
                if not broken:
                    return existing

        mat = bpy.data.materials.new(name if name else ('pbrt_' + typ))
        mat.use_nodes = True
        try:
            mat['pbrt_import'] = True
        except Exception:
            pass
        nt = mat.node_tree
        bsdf = None
        for n in nt.nodes:
            if n.bl_idname == 'ShaderNodeBsdfPrincipled':
                bsdf = n
                break
        if bsdf is None:
            bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
            try:
                out = None
                for n in nt.nodes:
                    if n.bl_idname == 'ShaderNodeOutputMaterial':
                        out = n
                        break
                if out is None:
                    out = nt.nodes.new('ShaderNodeOutputMaterial')
                nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
            except Exception:
                pass

        low = (typ or '').lower()
        scene_dir = state['scene_dir']

        # --- base color: reflectance (Kd legacy) or tint for glass ---
        for slot, target_names, tex_target in (
                ('reflectance', ('Base Color',), 'color'),
                ('Kd', ('Base Color',), 'color'),
                ('tint', ('Base Color',), 'color')):
            param = prm.get(slot)
            if param is None:
                continue
            ptype, pval = param
            bc_in = _bsdf_input(bsdf, *target_names)
            if bc_in is None:
                continue
            if ptype == 'texture':
                tex_ref = _strip_tex_prefix(_param_str(param))
                rel = state['textures'].get(tex_ref) or tex_ref
                full = _resolve_image(rel, scene_dir)
                if full:
                    img = _load_image(full, tex_ref)
                    if img is not None:
                        node = _add_image_node(nt, img, -400, 300)
                        nt.links.new(node.outputs['Color'], bc_in)
            else:
                a = pval if isinstance(pval, list) else [pval] * 3
                bc_in.default_value = (_clamp01(_f(a, 0)),
                                       _clamp01(_f(a, 1)),
                                       _clamp01(_f(a, 2)), 1.0)
            if slot == 'reflectance' or slot == 'Kd':
                break  # first hit wins (don't mix Kd + reflectance)

        # --- metallic (metal / mirror / conductor) ---
        if low in ('metal', 'mirror', 'conductor', 'coatedconductor'):
            mi = _bsdf_input(bsdf, 'Metallic')
            if mi is not None:
                mi.default_value = 1.0

        # --- transmission + IOR (glass / dielectric / translucent) ---
        trans = low in ('glass', 'dielectric', 'translucent', 'water')
        eta = prm.get('eta') or prm.get('index')
        if trans:
            ti = _bsdf_input(bsdf, 'Transmission', 'Transmission Weight')
            if ti is not None:
                ti.default_value = 1.0
            if eta is not None:
                ii = _bsdf_input(bsdf, 'IOR')
                if ii is not None:
                    ii.default_value = _param_float(eta, 1.5)
            else:
                ii = _bsdf_input(bsdf, 'IOR')
                if ii is not None:
                    ii.default_value = 1.5
        elif eta is not None:
            ii = _bsdf_input(bsdf, 'IOR')
            if ii is not None:
                ii.default_value = _param_float(eta, 1.5)

        # --- roughness ---
        rough = prm.get('roughness')
        if rough is not None:
            ri = _bsdf_input(bsdf, 'Roughness')
            if ri is not None:
                ri.default_value = _clamp01(_param_float(rough, 0.5))

        # --- normal map / bump map ---
        nm = prm.get('normalmap') or prm.get('bumpmap')
        if nm is not None:
            rel = _param_str(nm)
            full = _resolve_image(rel, scene_dir)
            if full:
                img = _load_image(full, rel)
                if img is not None:
                    tex = _add_image_node(nt, img, -400, -200, as_data=True)
                    nmap = nt.nodes.new('ShaderNodeNormalMap')
                    nmap.location = (-200, -200)
                    nt.links.new(tex.outputs['Color'], nmap.inputs['Color'])
                    ni = _bsdf_input(bsdf, 'Normal')
                    if ni is not None:
                        nt.links.new(nmap.outputs['Normal'], ni)

        # --- emission (area-light / emissive materials) ---
        for eslot in ('emission', 'Le', 'L'):
            em = prm.get(eslot)
            if em is None:
                continue
            cols = em[1] if isinstance(em[1], list) else [em[1]] * 3
            ec = _bsdf_input(bsdf, 'Emission Color', 'Emission')
            es = _bsdf_input(bsdf, 'Emission Strength')
            if ec is not None:
                ec.default_value = (_f(cols, 0), _f(cols, 1),
                                    _f(cols, 2), 1.0)
            if es is not None:
                es.default_value = max(1.0, max(_f(cols, 0),
                                                _f(cols, 1), _f(cols, 2)))
            break

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
        elif kw in ('Transform', 'ConcatTransform'):
            tk.next()  # '['
            arr = []
            while True:
                x = tk.next()
                if x is None or x[0] == ']':
                    break
                arr.append(float(x[1]))
            # pbrt stores row-major with translation in the last ROW
            # (row-vector convention); Blender needs the transpose.
            M = mathutils.Matrix((
                (arr[0], arr[4], arr[8], arr[12]),
                (arr[1], arr[5], arr[9], arr[13]),
                (arr[2], arr[6], arr[10], arr[14]),
                (arr[3], arr[7], arr[11], arr[15])))
            state['ctm'] = M if kw == 'Transform' else (state['ctm'] @ M)
        elif kw == 'TransformTimes':
            tk.next(); tk.next()
        elif kw == 'CoordSysTransform':
            tk.next()
        elif kw in ('ActiveTransformAll', 'ActiveTransformEndTime',
                    'ActiveTransformStartTime', 'ReverseOrientation',
                    'ObjectEnd', 'WorldEnd', 'EndOfFiles'):
            pass
        elif kw in ('ObjectBegin', 'ObjectInstance'):
            tk.next()  # name
        elif kw == 'Attribute':
            tk.next()
            _parse_params(tk)
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
                rx = prm.get('xresolution')
                ry = prm.get('yresolution')
                if rx and ry:
                    state['res'] = (int(_param_float(rx)),
                                    int(_param_float(ry)))
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
            if isinstance(typ, list):
                typ = typ[0] if typ else 'diffuse'
            state['mat'] = make_material(None, typ, prm)
        elif kw == 'MakeNamedMaterial':
            nm = tk.next()[1]  # name token (bare quoted string)
            prm = _parse_params(tk)
            typ = _param_str(prm.get('type')) or 'diffuse'
            state['named'][nm] = make_material(nm, typ, prm)
        elif kw == 'NamedMaterial':
            nm = tk.next()[1]
            state['mat'] = state['named'].get(nm)
        elif kw == 'Texture':
            # Texture "name" "spectrum"/"float" "imagemap"
            #   "string filename" [ "path" ]
            nm = tk.next()[1]          # texture name (may carry rgb: prefix)
            tk.next()                   # value type (spectrum/float)
            tk.next()                   # impl ("imagemap")
            prm = _parse_params(tk)
            fn = _param_str(prm.get('filename'))
            if fn:
                # store under the un-prefixed reference name so material
                # "texture reflectance" "rgb:name" lookups resolve.
                key = _strip_tex_prefix(nm)
                state['textures'][nm] = fn
                state['textures'][key] = fn
        elif kw == 'AreaLightSource':
            # record but don't create a Blender light here (geometry-bound)
            tk.next()  # type token
            _parse_params(tk)
        elif kw == 'LightSource':
            typ = tk.next()[1]  # type token
            prm = _parse_params(tk)
            inten = prm.get('I') or prm.get('L') or prm.get('color')
            e = 1000.0
            if inten:
                e = 1000.0 * sum(map(float, inten[1][:3])) / 3.0
            light_name = 'pbrt_' + typ
            # remove any prior light with this name to avoid .001 accumulation
            for existing in list(bpy.data.objects):
                if existing.name == light_name:
                    bpy.data.objects.remove(existing, do_unlink=True)
            for existing in list(bpy.data.lights):
                if existing.name == light_name:
                    bpy.data.lights.remove(existing)
            if typ in ('distant', 'infinite'):
                # Environment/infinite light is represented by the world
                # background (sky image), not a Blender lamp object.
                w = bpy.data.worlds.get('pbrt_world') or \
                    bpy.data.worlds.new('pbrt_world')
                bpy.context.scene.world = w
                w.use_nodes = True
                bg = None
                for n in w.node_tree.nodes:
                    if n.bl_idname == 'ShaderNodeBackground':
                        bg = n
                        break
                if bg is None:
                    bg = w.node_tree.nodes.new('ShaderNodeBackground')
                    try:
                        w.node_tree.links.new(
                            bg.outputs['Background'],
                            w.node_tree.nodes['World Output'].inputs['Surface']
                            if 'World Output' in w.node_tree.nodes else
                            next(n for n in w.node_tree.nodes
                                 if n.bl_idname == 'ShaderNodeOutputWorld').inputs['Surface'])
                    except Exception:
                        pass
                # pbrt infinite light may carry either an I/L/color or a
                # "filename" sky image (+ optional "scale"); honor both.
                img_path = _param_str(prm.get('filename'))
                scale = _param_float(prm.get('scale'), 1.0)
                env_img = None
                if img_path:
                    full = _resolve_image(img_path, state['scene_dir'])
                    if full:
                        env_img = _load_image(full, img_path)
                        # This Blender build fails to decode certain sky
                        # .exr/.hdr files (pixels come back all-zero), which
                        # would leave the scene black. Treat a blank decode
                        # as "no usable environment map".
                        if _image_is_blank(env_img):
                            env_img = None
                if env_img is not None:
                    # Real environment map: use it as global illumination.
                    try:
                        env_img.colorspace_settings.is_data = False
                    except Exception:
                        pass
                    tex = _add_image_node(w.node_tree, env_img, -300, 0)
                    try:
                        w.node_tree.links.new(
                            tex.outputs['Color'], bg.inputs['Color'])
                        bg.inputs['Strength'].default_value = (
                            scale if 0.0 < scale <= 5.0 else 1.0)
                    except Exception:
                        pass
                elif inten:
                    # No image, but an explicit light color was given.
                    bg.inputs['Color'].default_value = (
                        _f(inten[1], 0), _f(inten[1], 1), _f(inten[1], 2), 1)
                    bg.inputs['Strength'].default_value = scale
                else:
                    # No usable sky image and no explicit color: fall back
                    # to a procedural sky so the scene is actually lit
                    # (otherwise it renders black).
                    sky = w.node_tree.nodes.new('ShaderNodeTexSky')
                    sky.location = (-320, 120)
                    try:
                        w.node_tree.links.new(
                            sky.outputs['Color'], bg.inputs['Color'])
                        bg.inputs['Strength'].default_value = 2.0
                    except Exception:
                        pass
            else:
                ld = bpy.data.lights.new(light_name, 'POINT')
                ld.energy = e
                ob = bpy.data.objects.new(light_name, ld)
                frm = prm.get('from')
                if frm:
                    ob.location = (_f(frm[1], 0), _f(frm[1], 1), _f(frm[1], 2))
                coll.objects.link(ob)
        elif kw == 'Shape':
            typ = tk.next()[1]  # type token
            prm = _parse_params(tk)
            if typ == 'trianglemesh':
                P = prm.get('P', ('', []))[1]
                idx = [int(float(x)) for x in
                       prm.get('indices', ('', []))[1]]
                uvp = prm.get('uv')
                uv = uvp[1] if uvp else []
                make_mesh('shape_%04d' % state['count'], P, idx,
                          list(map(float, uv)))
            elif typ == 'plymesh':
                fn = prm.get('filename', ('', ''))[1]
                ply_path = os.path.join(os.path.dirname(path), fn)
                if os.path.exists(ply_path):
                    try:
                        _import_ply(ply_path, state)
                    except Exception as e:
                        print('PLY import failed:', ply_path, e)
        elif kw == 'Include':
            fn = tk.next()[1]
            sub = os.path.join(os.path.dirname(path), fn)
            if os.path.exists(sub):
                import_pbrt(sub, collection_name, state)
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

    # camera -- Blender cameras look down local -Z; use to_track_quat so the
    # camera reliably faces the pbrt LookAt target.
    cam_name = 'pbrt_camera'
    for existing in list(bpy.data.objects):
        if existing.name == cam_name:
            bpy.data.objects.remove(existing, do_unlink=True)
    for existing in list(bpy.data.cameras):
        if existing.name == cam_name:
            bpy.data.cameras.remove(existing)
    cam_data = bpy.data.cameras.new(cam_name)
    cam_ob = bpy.data.objects.new(cam_name, cam_data)
    if state['lookat']:
        e = mathutils.Vector(state['lookat'][0:3])
        t = mathutils.Vector(state['lookat'][3:6])
        cam_ob.location = e
        direction = (t - e).normalized()
        # local -Z tracks the target direction; local +Y tries to align up.
        cam_ob.rotation_mode = 'QUATERNION'
        cam_ob.rotation_quaternion = direction.to_track_quat('-Z', 'Y')
    if state['cam']:
        fov = state['cam'].get('fov')
        if fov:
            import math as _m
            cam_data.lens = 12.0 / _m.tan(_m.radians(float(_param_float(fov))) / 2)
    coll.objects.link(cam_ob)
    bpy.context.scene.camera = cam_ob
    bpy.context.scene.render.resolution_x, bpy.context.scene.render.resolution_y = \
        state['res']
    return {'shapes': state['count'],
            'materials': len(state['named']),
            'textures': len(state['textures'])}


# ---------------------------------------------------------------------------
# Diagnostics (run inside Blender via execute_code)
# ---------------------------------------------------------------------------
def _check_materials(max_report=20):
    """Print how many pbrt materials resolved their base-color texture and
    normal map, and list the first few that did not."""
    mats = [m for m in bpy.data.materials if m.use_nodes]
    n_base, n_norm, n_total = 0, 0, len(mats)
    missing = []
    for m in mats:
        nt = m.node_tree
        has_base_tex = has_norm = False
        for n in nt.nodes:
            if n.type == 'TEX_IMAGE' and n.image:
                # is it feeding a color socket (Base Color) or a normal map?
                for lnk in nt.links:
                    if lnk.from_node == n:
                        to = lnk.to_socket.name
                        if 'Base Color' in to or 'Color' in to:
                            has_base_tex = True
                        if 'Normal' in to:
                            has_norm = True
        if has_base_tex:
            n_base += 1
        if has_norm:
            n_norm += 1
        if not has_base_tex and len(missing) < max_report:
            missing.append(m.name)
    print('MATERIALS', n_total, 'WITH_BASE_TEX', n_base,
          'WITH_NORMAL', n_norm)
    for name in missing:
        print('  NO_BASE_TEX', name)
    imgs = [i for i in bpy.data.images if i.name != 'Render Result']
    print('IMAGES', len(imgs))


# ---------------------------------------------------------------------------
# Blender addon wrapper (File > Import > PBRT Scene)
# ---------------------------------------------------------------------------
bl_info = {
    "name": "PBRT Scene Importer",
    "author": "pbrt-v4 tooling",
    "version": (1, 0, 0),
    "blender": (3, 0, 0),
    "location": "File > Import > PBRT Scene (.pbrt)",
    "description": "Import a PBRT-v4 scene file as meshes + Principled "
                   "BSDF materials (named texture maps, normal maps, etc.)",
    "category": "Import-Export",
}


class IMPORT_OT_pbrt(bpy.types.Operator):
    """Import a PBRT scene (.pbrt)"""
    bl_idname = "import_scene.pbrt"
    bl_label = "Import PBRT Scene"
    bl_options = {'PRESET', 'UNDO'}

    filepath: bpy.props.StringProperty(
        subtype='FILE_PATH',
        name="File Path",
        description="Path to the .pbrt scene file to import",
    )
    filter_glob: bpy.props.StringProperty(
        default="*.pbrt", options={'HIDDEN'},
    )
    collection_name: bpy.props.StringProperty(
        name="Collection",
        description="Name of the new collection to hold imported objects",
        default="PBRT",
    )

    def execute(self, context):
        if not self.filepath:
            self.report({'ERROR'}, "No file path given")
            return {'CANCELLED'}
        if not os.path.isfile(self.filepath):
            self.report({'ERROR'}, "File not found: %s" % self.filepath)
            return {'CANCELLED'}
        try:
            res = import_pbrt(self.filepath, self.collection_name)
        except Exception as e:
            self.report({'ERROR'}, "PBRT import failed: %s" % e)
            return {'CANCELLED'}
        self.report(
            {'INFO'},
            "PBRT import: %d shapes, %d materials, %d textures"
            % (res.get('shapes', 0), res.get('materials', 0),
               res.get('textures', 0)),
        )
        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}


def _menu_func_import(self, context):
    self.layout.operator(IMPORT_OT_pbrt.bl_idname, text="PBRT Scene (.pbrt)")


def register():
    bpy.utils.register_class(IMPORT_OT_pbrt)
    bpy.types.TOPBAR_MT_file_import.append(_menu_func_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(_menu_func_import)
    bpy.utils.unregister_class(IMPORT_OT_pbrt)


if __name__ == "__main__":
    register()
