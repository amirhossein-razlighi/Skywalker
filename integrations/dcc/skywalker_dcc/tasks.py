"""Tasks the engine runs inside Blender (dcc_convert, dcc_export, dcc_edit_asset, dcc_generate).

Each task receives the JSON parameters the engine wrote (params.json) and records what it
produced with sky.result(files=[...]). The engine imports the produced .glb files.
"""

import os
import re
import runpy
import traceback

import bpy
from mathutils import Matrix, Vector

import skywalker_dcc as sky
from skywalker_dcc import blender as B


def _safe_name(name):
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("._")
    return cleaned or "asset"


def _run_user_script(path, **scope):
    scope.setdefault("sky", sky)
    scope.setdefault("B", B)
    scope.setdefault("bpy", bpy)
    runpy.run_path(path, init_globals=scope, run_name="__main__")


# ---------------------------------------------------------------------------
# convert: any supported format -> glb
# ---------------------------------------------------------------------------

def convert(params):
    results = []
    for item in params["inputs"]:
        entry = {"input": item["path"], "file": item["name"] + ".glb"}
        try:
            ext = os.path.splitext(item["path"])[1].lower()
            if ext == ".blend":
                current = os.path.realpath(bpy.data.filepath) if bpy.data.filepath else ""
                if current != os.path.realpath(item["path"]):
                    B._filtered(bpy.ops.wm.open_mainfile, filepath=item["path"], load_ui=False, use_scripts=False)
                    B.enable_addons()
                objects = None
            else:
                B.reset_scene()
                objects = B.import_file(item["path"], scale=params.get("scale", 1.0),
                                        axis_forward=params.get("axis_forward"), axis_up=params.get("axis_up"))
            exportable = B.exportable_objects()
            if not exportable:
                raise RuntimeError("the file contains no mesh geometry")
            if params.get("recenter"):
                B.recenter(exportable, params["recenter"])
            if params.get("ops"):
                B.apply_ops(exportable, params["ops"])
            out = sky.out_path(entry["file"])
            B.export_glb(out, None, apply_modifiers=params.get("apply_modifiers", True))
            entry.update(B.stats())
            entry["ok"] = True
            entry["bytes"] = os.path.getsize(out)
        except Exception as e:  # noqa: BLE001 - one bad file must not stop a batch
            traceback.print_exc()
            entry["ok"] = False
            entry["error"] = "%s: %s" % (type(e).__name__, e)
        results.append(entry)
    sky.result(files=results)
    if not any(r["ok"] for r in results):
        raise RuntimeError("no file could be converted: " + "; ".join(r.get("error", "") for r in results))


# ---------------------------------------------------------------------------
# export: collections/objects of a .blend -> one glb each
# ---------------------------------------------------------------------------

def _all_layer_collections(layer):
    yield layer
    for child in layer.children:
        yield from _all_layer_collections(child)


def _asset_objects(collection, include_hidden):
    out = []
    for o in collection.all_objects:
        if o.type not in B.MESH_LIKE:
            continue
        if not include_hidden and (o.hide_viewport or (B._in_view_layer(o) and o.hide_get())):
            continue
        out.append(o)
    return out


def _has_mesh(collection, include_hidden):
    return bool(_asset_objects(collection, include_hidden))


def export(params):
    blend = params.get("blend") or ""
    if blend and os.path.realpath(bpy.data.filepath or "") != os.path.realpath(blend):
        B._filtered(bpy.ops.wm.open_mainfile, filepath=blend, load_ui=False, use_scripts=False)
    B.enable_addons()
    scene = bpy.context.scene
    include_hidden = bool(params.get("include_hidden", False))
    if include_hidden:
        for lc in _all_layer_collections(bpy.context.view_layer.layer_collection):
            lc.exclude = False

    # Real units: a scene authored in centimeters (unit scale 0.01) is exported in meters.
    unit_scale = 1.0
    if params.get("real_units", True) and scene.unit_settings.system != "NONE":
        unit_scale = scene.unit_settings.scale_length
    if abs(unit_scale - 1.0) > 1e-9:
        B._scale_roots([o for o in scene.objects], unit_scale)
        sky.log("applied unit scale", unit_scale)

    # Which assets: explicit collections/objects, or one asset per top-level collection.
    assets = []  # (name, objects)
    for cname in params.get("collections") or []:
        coll = bpy.data.collections.get(cname)
        if coll is None:
            raise ValueError("no collection named %r (collections: %s)" % (cname, ", ".join(c.name for c in bpy.data.collections)))
        assets.append((cname, _asset_objects(coll, include_hidden)))
    for oname in params.get("objects") or []:
        obj = bpy.data.objects.get(oname)
        if obj is None:
            raise ValueError("no object named %r" % oname)
        group = [obj] + [c for c in obj.children_recursive if c.type in B.MESH_LIKE]
        assets.append((oname, [o for o in group if o.type in B.MESH_LIKE]))
    if not assets:
        for coll in scene.collection.children:
            if _has_mesh(coll, include_hidden):
                assets.append((coll.name, _asset_objects(coll, include_hidden)))
        loose = [o for o in scene.collection.objects if o.type in B.MESH_LIKE and (include_hidden or not o.hide_viewport)]
        if loose:
            stem = os.path.splitext(os.path.basename(blend or bpy.data.filepath or "scene"))[0]
            assets.append((stem, loose))

    origin = params.get("origin", "keep")
    results = []
    for name, objs in assets:
        entry = {"asset": name, "file": _safe_name(name) + ".glb"}
        if not objs:
            entry.update(ok=False, error="no exportable mesh objects")
            results.append(entry)
            continue
        offset = None
        bounds = B.world_bounds(objs)
        if origin in ("bottom_center", "center") and bounds:
            lo, hi = bounds
            cz = lo.z if origin == "bottom_center" else (lo.z + hi.z) / 2
            offset = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, cz))
        elif origin == "collection":
            coll = bpy.data.collections.get(name)
            if coll is not None:
                offset = Vector(coll.instance_offset)
        saved = {}
        try:
            if offset is not None:
                names = {o.name for o in objs}
                for o in objs:
                    if o.parent is None or o.parent.name not in names:
                        saved[o.name] = o.matrix_world.copy()
                        o.matrix_world = Matrix.Translation(-offset) @ o.matrix_world
            out = sky.out_path(entry["file"])
            B.export_glb(out, objs, apply_modifiers=params.get("apply_modifiers", True))
            entry.update(B.stats(objs))
            entry["ok"] = True
            entry["bytes"] = os.path.getsize(out)
        except Exception as e:  # noqa: BLE001
            traceback.print_exc()
            entry.update(ok=False, error="%s: %s" % (type(e).__name__, e))
        finally:
            for oname, m in saved.items():
                ob = bpy.data.objects.get(oname)
                if ob is not None:
                    ob.matrix_world = m
        results.append(entry)
    sky.result(files=results, unit_scale=unit_scale)
    if not any(r["ok"] for r in results):
        raise RuntimeError("nothing was exported: " + ("; ".join(r.get("error", "") for r in results) or "no assets found"))


# ---------------------------------------------------------------------------
# edit: glb -> Blender -> glb
# ---------------------------------------------------------------------------

def edit(params):
    B.reset_scene()
    objects = B.import_file(params["input"])
    meshes = B.mesh_objects(objects)
    if not meshes:
        raise RuntimeError("the asset has no mesh objects to edit")
    sky.log("loaded", len(meshes), "mesh object(s)")
    before = B.stats()
    if params.get("ops"):
        objects = B.apply_ops(B.exportable_objects(), params["ops"])
    if params.get("has_script"):
        _run_user_script(os.environ["SKY_SCRIPT"], objects=B.exportable_objects(), params=params)
    out = sky.out_path(params["output"])
    B.export_glb(out, None, apply_modifiers=True)
    after = B.stats()
    sky.result(files=[dict(file=params["output"], ok=True, bytes=os.path.getsize(out), **after)], before=before, after=after)


# ---------------------------------------------------------------------------
# generate: procedural modeling -> glb
# ---------------------------------------------------------------------------

def generate(params):
    from skywalker_dcc import procedural
    B.reset_scene()
    made = []
    if params.get("recipe"):
        obj = procedural.generate(params["recipe"], name=params.get("name") or params["recipe"], **(params.get("recipe_params") or {}))
        made.append(obj)
    if params.get("has_script"):
        _run_user_script(os.environ["SKY_SCRIPT"], objects=list(made), params=params, procedural=procedural)
    if not B.exportable_objects():
        raise RuntimeError("the script produced no mesh objects (create objects and link them to bpy.context.scene.collection)")
    if params.get("ops"):
        B.apply_ops(B.exportable_objects(), params["ops"])
    out = sky.out_path(params["output"])
    B.export_glb(out, None, apply_modifiers=True)
    st = B.stats()
    sky.result(files=[dict(file=params["output"], ok=True, bytes=os.path.getsize(out), **st)])
