# SPDX-License-Identifier: GPL-3.0-or-later
# (code that runs inside Blender and uses its Python API follows Blender's license, see docs/LICENSING.md)
"""Blender toolkit for Skywalker scripts (runs inside Blender, 3.2 or newer).

The functions here are the building blocks of the dcc_convert / dcc_export / dcc_edit_asset /
dcc_generate tools and are available to your own scripts:

    from skywalker_dcc import blender as B
    B.reset_scene()
    objs = B.import_file(sky.project_path("downloads/pack/tree.fbx"))
    B.decimate(objs, 0.3)
    B.export_glb(sky.out_path("tree_lod1.glb"), objs)

Conventions: Blender units are meters; glTF export converts to Y-up; "objects" arguments are
lists of bpy objects (default: every exportable object in the scene).
"""

import math
import os

import bmesh
import bpy
from mathutils import Matrix, Vector

import skywalker_dcc as sky

_HAS_AUTO_SMOOTH = "use_auto_smooth" in bpy.types.Mesh.bl_rna.properties

# Object types the glTF exporter turns into meshes.
MESH_LIKE = ("MESH", "CURVE", "SURFACE", "FONT", "META")


# ---------------------------------------------------------------------------
# Scene & add-on housekeeping
# ---------------------------------------------------------------------------

def enable_addons():
    """Make sure the bundled importers/exporters are on (they are in factory settings, but a
    user profile may have switched them off)."""
    import addon_utils
    available = {m.__name__ for m in addon_utils.modules()}
    for name in ("io_scene_gltf2", "io_scene_fbx", "io_scene_3ds", "io_scene_x3d"):
        if name not in available:
            continue  # this Blender version does not ship it
        try:
            if not addon_utils.check(name)[1]:
                addon_utils.enable(name, default_set=False, persistent=False)
        except Exception:  # noqa: BLE001
            pass


def has_operator(idname):
    """True if this Blender version has the operator ('import_scene.fbx'). hasattr() cannot
    tell: bpy.ops resolves any name lazily."""
    category, _, name = idname.partition(".")
    try:
        getattr(getattr(bpy.ops, category), name).get_rna_type()
        return True
    except Exception:  # noqa: BLE001
        return False


def reset_scene():
    """An empty scene (no default cube, camera or light)."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    enable_addons()


def _filtered(op, **kwargs):
    """Call a Blender operator with only the properties this Blender version knows (operator
    arguments are renamed between releases)."""
    known = {p.identifier for p in op.get_rna_type().properties}
    result = op(**{k: v for k, v in kwargs.items() if k in known})
    if "FINISHED" not in result:
        raise RuntimeError("operator %s did not finish: %s" % (op.idname(), sorted(result)))
    return result


def exportable_objects(visible_only=True):
    """Objects the glTF exporter will include."""
    bpy.context.view_layer.update()  # objects linked a moment ago only show up after an update
    out = []
    for o in bpy.context.scene.objects:
        if o.type not in MESH_LIKE:
            continue
        if visible_only:
            if not _in_view_layer(o) or o.hide_viewport or o.hide_get():
                continue
        out.append(o)
    return out


def _in_view_layer(obj):
    return obj.name in bpy.context.view_layer.objects


def mesh_objects(objects=None):
    """The mesh objects among `objects` (default: the whole scene)."""
    pool = objects if objects is not None else list(bpy.context.scene.objects)
    return [o for o in pool if o.type == "MESH"]


def select(objects):
    """Select exactly `objects` and make the first one active."""
    for o in list(bpy.context.view_layer.objects):
        try:
            o.select_set(False)
        except RuntimeError:
            pass
    bpy.context.view_layer.update()
    first = None
    for o in objects:
        try:
            o.hide_set(False)
            o.hide_viewport = False
            o.select_set(True)
            first = first or o
        except RuntimeError:
            continue
    if first is not None:
        bpy.context.view_layer.objects.active = first
    return first


# ---------------------------------------------------------------------------
# Import
# ---------------------------------------------------------------------------

def _axis(value, new_style):
    """'-Z' -> 'NEGATIVE_Z' for the operators that spell axes out."""
    if value is None:
        return None
    value = value.upper()
    if new_style and value.startswith("-"):
        return "NEGATIVE_" + value[1:]
    return value.lstrip("+")


def _import_blend(path, **_):
    with bpy.data.libraries.load(path, link=False) as (src, dst):
        dst.collections = list(src.collections)
        dst.objects = list(src.objects)
    scene = bpy.context.scene
    in_collection = set()
    for coll in dst.collections:
        for o in coll.all_objects:
            in_collection.add(o.name)
    child_names = {c.name for coll in dst.collections for c in coll.children}
    for coll in dst.collections:
        if coll.name not in child_names:
            scene.collection.children.link(coll)
    for o in dst.objects:
        if o is not None and o.name not in in_collection:
            scene.collection.objects.link(o)


def _import_fbx(path, scale=1.0, axis_forward=None, axis_up=None, **_):
    kw = {"filepath": path, "use_image_search": True, "global_scale": scale}
    if axis_forward:
        kw["axis_forward"] = _axis(axis_forward, False)
    if axis_up:
        kw["axis_up"] = _axis(axis_up, False)
    _filtered(bpy.ops.import_scene.fbx, **kw)


def _import_obj(path, scale=1.0, axis_forward=None, axis_up=None, **_):
    if has_operator("wm.obj_import"):  # 3.2+
        kw = {"filepath": path, "global_scale": scale}
        if axis_forward:
            kw["forward_axis"] = _axis(axis_forward, True)
        if axis_up:
            kw["up_axis"] = _axis(axis_up, True)
        _filtered(bpy.ops.wm.obj_import, **kw)
    else:
        kw = {"filepath": path, "global_scale": scale}
        if axis_forward:
            kw["axis_forward"] = _axis(axis_forward, False)
        if axis_up:
            kw["axis_up"] = _axis(axis_up, False)
        _filtered(bpy.ops.import_scene.obj, **kw)


def _import_dae(path, **_):
    _filtered(bpy.ops.wm.collada_import, filepath=path)


def _import_usd(path, scale=1.0, **_):
    _filtered(bpy.ops.wm.usd_import, filepath=path, scale=scale)


def _import_3ds(path, scale=1.0, **_):
    for op in ("import_scene.max3ds", "import_scene.autodesk_3ds"):  # renamed in Blender 3.6
        if has_operator(op):
            _filtered(getattr(bpy.ops.import_scene, op.split(".")[1]), filepath=path, global_scale=scale)
            return
    raise RuntimeError("this Blender (%s) has no 3DS importer; convert the file to FBX/OBJ elsewhere or use Blender 3.6+" % bpy.app.version_string)


def _import_ply(path, scale=1.0, **_):
    if has_operator("wm.ply_import"):
        _filtered(bpy.ops.wm.ply_import, filepath=path, global_scale=scale)
    else:
        _filtered(bpy.ops.import_mesh.ply, filepath=path)


def _import_stl(path, scale=1.0, **_):
    if has_operator("wm.stl_import"):
        _filtered(bpy.ops.wm.stl_import, filepath=path, global_scale=scale)
    else:
        _filtered(bpy.ops.import_mesh.stl, filepath=path, global_scale=scale)


def _import_abc(path, scale=1.0, **_):
    _filtered(bpy.ops.wm.alembic_import, filepath=path, scale=scale)


def _import_gltf(path, **_):
    _filtered(bpy.ops.import_scene.gltf, filepath=path)


_IMPORTERS = {
    ".blend": _import_blend,
    ".fbx": _import_fbx,
    ".obj": _import_obj,
    ".dae": _import_dae,
    ".usd": _import_usd, ".usda": _import_usd, ".usdc": _import_usd, ".usdz": _import_usd,
    ".3ds": _import_3ds,
    ".ply": _import_ply,
    ".stl": _import_stl,
    ".abc": _import_abc,
    ".glb": _import_gltf, ".gltf": _import_gltf,
}

# Importers that take a scale factor natively; for the rest `scale` is applied to the roots.
_NATIVE_SCALE = {".fbx", ".obj", ".usd", ".usda", ".usdc", ".usdz", ".3ds", ".ply", ".stl", ".abc"}


def supported_import_formats():
    return sorted(_IMPORTERS)


def import_file(path, scale=1.0, axis_forward=None, axis_up=None):
    """Import a model file and return the new objects.

    Supports .fbx .obj .dae .usd/.usda/.usdc/.usdz .3ds .ply .stl .abc .glb/.gltf .blend.
    `scale` multiplies the size (use 0.01 for a pack authored in centimeters that the importer
    did not convert); `axis_forward` / `axis_up` ('-Z', 'Y', ...) override the source's
    orientation for FBX and OBJ.
    """
    path = os.path.abspath(path)
    ext = os.path.splitext(path)[1].lower()
    if ext not in _IMPORTERS:
        raise ValueError("cannot import %s files (supported: %s)" % (ext or "extension-less", ", ".join(supported_import_formats())))
    if not os.path.exists(path):
        raise FileNotFoundError(path)
    before = {o.name for o in bpy.data.objects}
    native = ext in _NATIVE_SCALE
    _IMPORTERS[ext](path, scale=scale if native else 1.0, axis_forward=axis_forward, axis_up=axis_up)
    new = [o for o in bpy.data.objects if o.name not in before]
    if scale != 1.0 and not native and new:
        _scale_roots(new, scale)
    return new


def _scale_roots(objects, factor):
    names = {o.name for o in objects}
    for o in objects:
        if o.parent is None or o.parent.name not in names:
            o.scale = [s * factor for s in o.scale]
            o.location = [l * factor for l in o.location]


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------

def export_glb(path, objects=None, apply_modifiers=True, animations=False, materials=True, extra=None):
    """Write `objects` (default: every exportable object) to a self-contained .glb, Y-up,
    textures embedded. Returns the path."""
    enable_addons()
    path = os.path.abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    kw = dict(
        filepath=path,
        export_format="GLB",
        use_selection=objects is not None,
        export_apply=apply_modifiers,
        export_yup=True,
        export_cameras=False,
        export_lights=False,
        export_animations=animations,
        export_materials="EXPORT" if materials else "NONE",
        export_image_format="AUTO",
        export_texcoords=True,
        export_normals=True,
        export_colors=True,
        export_extras=False,
    )
    if extra:
        kw.update(extra)
    if objects is not None:
        if not select(objects):
            raise RuntimeError("nothing to export: none of the objects can be selected")
    elif not exportable_objects():
        raise RuntimeError("nothing to export: the scene has no visible mesh objects")
    _filtered(bpy.ops.export_scene.gltf, **kw)
    if not os.path.exists(path):
        raise RuntimeError("the glTF exporter produced no file")
    return path


def export_fbx(path, objects=None):
    path = os.path.abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if objects is not None:
        select(objects)
    _filtered(bpy.ops.export_scene.fbx, filepath=path, use_selection=objects is not None,
              apply_scale_options="FBX_SCALE_ALL", path_mode="COPY", embed_textures=True)
    return path


def export_obj(path, objects=None):
    path = os.path.abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if objects is not None:
        select(objects)
    if has_operator("wm.obj_export"):
        _filtered(bpy.ops.wm.obj_export, filepath=path, export_selected_objects=objects is not None)
    else:
        _filtered(bpy.ops.export_scene.obj, filepath=path, use_selection=objects is not None)
    return path


# ---------------------------------------------------------------------------
# Measuring
# ---------------------------------------------------------------------------

def world_bounds(objects=None):
    """(min, max) corners in Blender world space (Z up, meters), or None for no geometry."""
    objects = exportable_objects() if objects is None else objects
    lo = Vector((math.inf,) * 3)
    hi = Vector((-math.inf,) * 3)
    found = False
    deps = bpy.context.evaluated_depsgraph_get()
    for o in objects:
        if o.type not in MESH_LIKE:
            continue
        ev = o.evaluated_get(deps)
        for corner in ev.bound_box:
            w = ev.matrix_world @ Vector(corner)
            for i in range(3):
                lo[i] = min(lo[i], w[i])
                hi[i] = max(hi[i], w[i])
            found = True
    return (lo, hi) if found else None


def stats(objects=None):
    """Triangle/vertex counts, material count and the size in glTF axes (x, y-up, z)."""
    objects = exportable_objects() if objects is None else objects
    deps = bpy.context.evaluated_depsgraph_get()
    verts = tris = 0
    mats = set()
    for o in objects:
        if o.type not in MESH_LIKE:
            continue
        ev = o.evaluated_get(deps)
        me = ev.to_mesh()
        try:
            verts += len(me.vertices)
            me.calc_loop_triangles()
            tris += len(me.loop_triangles)
        finally:
            ev.to_mesh_clear()
        for slot in o.material_slots:
            if slot.material:
                mats.add(slot.material.name)
    out = {"objects": len([o for o in objects if o.type in MESH_LIKE]), "vertices": verts, "triangles": tris, "materials": len(mats)}
    b = world_bounds(objects)
    if b:
        lo, hi = b
        out["size_m"] = [round(hi[0] - lo[0], 4), round(hi[2] - lo[2], 4), round(hi[1] - lo[1], 4)]  # x, up, z
        out["min_m"] = [round(lo[0], 4), round(lo[2], 4), round(lo[1], 4)]
    return out


# ---------------------------------------------------------------------------
# Mesh operations
# ---------------------------------------------------------------------------

def _make_single_user(obj):
    if obj.data is not None and obj.data.users > 1:
        obj.data = obj.data.copy()


def apply_modifiers(objects):
    """Bake every modifier into the mesh (no operator context needed)."""
    deps = bpy.context.evaluated_depsgraph_get()
    for o in mesh_objects(objects):
        if not o.modifiers:
            continue
        ev = o.evaluated_get(deps)
        new_mesh = bpy.data.meshes.new_from_object(ev, preserve_all_data_layers=True, depsgraph=deps)
        old = o.data
        o.modifiers.clear()
        o.data = new_mesh
        new_mesh.name = old.name
        if old.users == 0:
            bpy.data.meshes.remove(old)
        deps = bpy.context.evaluated_depsgraph_get()


def decimate(objects, ratio=0.5):
    """Collapse-decimate to `ratio` of the triangles (0.5 = half)."""
    ratio = max(0.01, min(1.0, float(ratio)))
    for o in mesh_objects(objects):
        _make_single_user(o)
        mod = o.modifiers.new("sky_decimate", "DECIMATE")
        mod.decimate_type = "COLLAPSE"
        mod.ratio = ratio
        mod.use_collapse_triangulate = False
    apply_modifiers(objects)


def bevel(objects, width=0.02, segments=2, angle_deg=30.0):
    """Bevel sharp edges (angle limit), the cheap way to catch highlights on hard-surface models."""
    for o in mesh_objects(objects):
        _make_single_user(o)
        mod = o.modifiers.new("sky_bevel", "BEVEL")
        mod.width = width
        mod.segments = int(segments)
        mod.limit_method = "ANGLE"
        mod.angle_limit = math.radians(angle_deg)
    apply_modifiers(objects)


def merge_by_distance(objects, distance=0.0001):
    """Weld vertices closer than `distance`. Returns the number of vertices removed."""
    removed = 0
    for o in mesh_objects(objects):
        _make_single_user(o)
        bm = bmesh.new()
        bm.from_mesh(o.data)
        before = len(bm.verts)
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=distance)
        removed += before - len(bm.verts)
        bm.to_mesh(o.data)
        bm.free()
        o.data.update()
    return removed


def triangulate(objects):
    for o in mesh_objects(objects):
        _make_single_user(o)
        bm = bmesh.new()
        bm.from_mesh(o.data)
        bmesh.ops.triangulate(bm, faces=bm.faces[:])
        bm.to_mesh(o.data)
        bm.free()
        o.data.update()


def shade_smooth(objects, angle_deg=40.0):
    """Smooth shading with an auto-smooth angle so hard edges stay hard."""
    for o in mesh_objects(objects):
        _make_single_user(o)
        me = o.data
        me.polygons.foreach_set("use_smooth", [True] * len(me.polygons))
        if _HAS_AUTO_SMOOTH:  # removed in Blender 4.1 (smooth by angle is an operator now)
            me.use_auto_smooth = True
            me.auto_smooth_angle = math.radians(angle_deg)
        me.update()
    if not _HAS_AUTO_SMOOTH and has_operator("object.shade_smooth_by_angle"):
        for o in mesh_objects(objects):
            select([o])
            bpy.ops.object.shade_smooth_by_angle(angle=math.radians(angle_deg))


def smart_uv(objects, angle_deg=66.0, margin=0.02):
    """Smart UV project every mesh (needed before baking textures)."""
    for o in mesh_objects(objects):
        _make_single_user(o)
        select([o])
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project(angle_limit=math.radians(angle_deg), island_margin=margin)
        bpy.ops.object.mode_set(mode="OBJECT")


def bake_transforms(objects):
    """Freeze location/rotation/scale (and parenting) into the vertices of mesh objects."""
    for o in mesh_objects(objects):
        _make_single_user(o)
        m = o.matrix_world.copy()
        o.parent = None
        o.data.transform(m)
        o.matrix_world = Matrix.Identity(4)
        o.data.update()


def recenter(objects, where="bottom"):
    """Move the geometry so the bounding box is centered on the origin in X/Y, with its
    bottom (`where='bottom'`) or middle (`'center'`) at height 0. Origin-at-the-feet is what
    makes assets drop onto the ground correctly in Skywalker."""
    objs = mesh_objects(objects)
    bake_transforms(objs)
    b = world_bounds(objs)
    if not b:
        return
    lo, hi = b
    cz = lo.z if where == "bottom" else (lo.z + hi.z) / 2
    offset = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, cz))
    for o in objs:
        o.data.transform(Matrix.Translation(-offset))
        o.data.update()


def join(objects, name="merged"):
    """Merge mesh objects into one (materials are kept as separate slots)."""
    objs = mesh_objects(objects)
    if not objs:
        return None
    if len(objs) == 1:
        objs[0].name = name
        return objs[0]
    bake_transforms(objs)
    active = objs[0]
    with bpy.context.temp_override(active_object=active, object=active, selected_objects=objs,
                                   selected_editable_objects=objs):
        bpy.ops.object.join()
    active.name = name
    return active


def bake_ao(objects, samples=32, distance=1.0, target="vertex", size=1024):
    """Bake ambient occlusion into the mesh with Cycles.

    target='vertex' stores it in the vertex colors (Skywalker multiplies them into the albedo);
    target='texture' bakes an image, which needs UVs (see smart_uv) and is multiplied into each
    material's base color. Returns the number of meshes baked.
    """
    objs = mesh_objects(objects)
    if not objs:
        return 0
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = int(samples)
    scene.cycles.device = "CPU"
    if scene.world is None:
        scene.world = bpy.data.worlds.new("sky_world")
    scene.world.light_settings.distance = float(distance)
    scene.world.light_settings.use_ambient_occlusion = True
    for o in objs:
        _make_single_user(o)
    if target == "vertex":
        for o in objs:
            me = o.data
            if hasattr(me, "color_attributes"):
                attr = me.color_attributes.get("AO") or me.color_attributes.new("AO", "FLOAT_COLOR", "CORNER")
                me.color_attributes.active_color = attr
            else:
                layer = me.vertex_colors.get("AO") or me.vertex_colors.new(name="AO")
                me.vertex_colors.active = layer
        select(objs)
        # Operator spelling changed over releases.
        last = None
        for tgt in ("VERTEX_COLORS", "COLOR_ATTRIBUTES"):
            try:
                bpy.ops.object.bake(type="AO", target=tgt)
                last = None
                break
            except TypeError as e:
                last = e
        if last:
            raise last
    else:
        for o in objs:
            if not o.data.uv_layers:
                smart_uv([o])
            img = bpy.data.images.new("%s_ao" % o.name, int(size), int(size))
            slots = o.material_slots
            if not slots:
                o.data.materials.append(bpy.data.materials.new("%s_mat" % o.name))
            for slot in o.material_slots:
                mat = slot.material
                mat.use_nodes = True
                node = mat.node_tree.nodes.new("ShaderNodeTexImage")
                node.image = img
                node.select = True
                mat.node_tree.nodes.active = node
            select([o])
            bpy.ops.object.bake(type="AO", target="IMAGE_TEXTURES")
            img.pack()
            for slot in o.material_slots:
                _multiply_base_color(slot.material, img)
    return len(objs)


def _multiply_base_color(mat, image):
    nt = mat.node_tree
    bsdf = next((n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        return
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = image
    mix = nt.nodes.new("ShaderNodeMixRGB")
    mix.blend_type = "MULTIPLY"
    mix.inputs["Fac"].default_value = 1.0
    base = bsdf.inputs["Base Color"]
    if base.links:
        nt.links.new(base.links[0].from_socket, mix.inputs["Color1"])
    else:
        mix.inputs["Color1"].default_value = base.default_value
    nt.links.new(tex.outputs["Color"], mix.inputs["Color2"])
    nt.links.new(mix.outputs["Color"], base)


APPLY_OPS = {
    "decimate": lambda objs, o: decimate(objs, o.get("ratio", 0.5)),
    "bevel": lambda objs, o: bevel(objs, o.get("width", 0.02), o.get("segments", 2), o.get("angle", 30.0)),
    "smart_uv": lambda objs, o: smart_uv(objs, o.get("angle", 66.0), o.get("margin", 0.02)),
    "merge_by_distance": lambda objs, o: merge_by_distance(objs, o.get("distance", 0.0001)),
    "triangulate": lambda objs, o: triangulate(objs),
    "shade_smooth": lambda objs, o: shade_smooth(objs, o.get("angle", 40.0)),
    "recenter": lambda objs, o: recenter(objs, o.get("where", "bottom")),
    "apply_modifiers": lambda objs, o: apply_modifiers(objs),
    "bake_ao": lambda objs, o: bake_ao(objs, o.get("samples", 32), o.get("distance", 1.0), o.get("target", "vertex"), o.get("size", 1024)),
    "join": lambda objs, o: join(objs, o.get("name", "merged")),
}


def apply_ops(objects, ops):
    """Run a list of preset operations: [{"op": "decimate", "ratio": 0.3}, ...]."""
    for spec in ops:
        name = spec.get("op")
        fn = APPLY_OPS.get(name)
        if fn is None:
            raise ValueError("unknown op %r (available: %s)" % (name, ", ".join(sorted(APPLY_OPS))))
        sky.log("op", name, {k: v for k, v in spec.items() if k != "op"})
        fn(objects, spec)
        # Ops may replace or merge objects; refresh the working set.
        objects = [o for o in objects if o.name in bpy.data.objects]
        if name == "join":
            objects = exportable_objects()
    return objects


# ---------------------------------------------------------------------------
# Materials
# ---------------------------------------------------------------------------

def _input(bsdf, *names):
    for n in names:
        if n in bsdf.inputs:
            return bsdf.inputs[n]
    return None


def make_material(name, color=(0.5, 0.5, 0.5), roughness=0.8, metallic=0.0, emission=None, alpha=1.0, vertex_colors=False):
    """A Principled BSDF material exported by glTF as PBR metallic-roughness. With
    vertex_colors=True the mesh's active color attribute drives the albedo."""
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next((n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    rgba = tuple(color)[:3] + (alpha,)
    _input(bsdf, "Base Color").default_value = rgba
    _input(bsdf, "Roughness").default_value = roughness
    _input(bsdf, "Metallic").default_value = metallic
    a = _input(bsdf, "Alpha")
    if a is not None:
        a.default_value = alpha
    if alpha < 1.0:
        # blend_method was removed in 4.2 (alpha blending is the default there).
        if hasattr(mat, "blend_method"):
            mat.blend_method = "BLEND"
    if emission is not None:
        e = _input(bsdf, "Emission Color", "Emission")
        if e is not None:
            e.default_value = tuple(emission)[:3] + (1.0,)
        s = _input(bsdf, "Emission Strength")
        if s is not None:
            s.default_value = 1.0
    if vertex_colors:
        node = nt.nodes.new("ShaderNodeVertexColor")
        nt.links.new(node.outputs["Color"], _input(bsdf, "Base Color"))
    return mat


def set_vertex_colors(obj, colors_per_face, name="Col"):
    """Write one RGB per polygon (flat tint) into a color attribute; used for stone/terrain variation."""
    me = obj.data
    if hasattr(me, "color_attributes"):
        attr = me.color_attributes.get(name) or me.color_attributes.new(name, "BYTE_COLOR", "CORNER")
        me.color_attributes.active_color = attr
        data = attr.data
    else:
        layer = me.vertex_colors.get(name) or me.vertex_colors.new(name=name)
        data = layer.data
    for poly in me.polygons:
        c = colors_per_face[poly.index]
        rgba = (c[0], c[1], c[2], 1.0)
        for li in poly.loop_indices:
            data[li].color = rgba
    me.update()


def link_to_scene(obj, collection=None):
    coll = collection or bpy.context.scene.collection
    if obj.name not in coll.objects:
        coll.objects.link(obj)
    return obj
