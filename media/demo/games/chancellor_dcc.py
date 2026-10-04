# SPDX-License-Identifier: GPL-3.0-or-later
# (runs inside Blender and uses its Python API, see docs/LICENSING.md)
"""The Chancellor's Desk — office furniture and set dressing modeled in Blender (`dcc_run_script`).

Every model is written to SKY_OUT as its own .glb (meters, origin at the bottom center, front
facing +Z in the engine). Material names are stable (walnut, leather, brass, glass_green, velvet,
cloth, lacquer, ink_glass, marble...) so the engine can swap in photoscanned materials.
"""
import json
import math

import bmesh
import bpy
from mathutils import Vector

from skywalker_dcc import blender as B

JOB = json.loads(sky.ARGS[0]) if sky.ARGS else {}  # noqa: F821
MATS = {}


def mat(name, color, roughness=0.6, metallic=0.0, emission=None):
    if name not in MATS:
        MATS[name] = B.make_material(name, color=tuple(c / 255.0 for c in color), roughness=roughness, metallic=metallic,
                                     emission=emission)
    return MATS[name]


def new_obj(name, bm, material, smooth=False):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    ob.data.materials.append(material)
    if smooth:
        for p in me.polygons:
            p.use_smooth = True
    return ob


def box(name, size, loc, material, bevel=0.0, segments=2):
    """A box (Blender coordinates: x right, y depth, z up) with an optional bevel modifier."""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = Vector((v.co.x * size[0] + loc[0], v.co.y * size[1] + loc[1], v.co.z * size[2] + loc[2]))
    ob = new_obj(name, bm, material)
    if bevel > 0:
        m = ob.modifiers.new("bevel", "BEVEL")
        m.width = bevel
        m.segments = segments
        m.limit_method = "ANGLE"
        m.harden_normals = False
    return ob


def cylinder(name, r, h, loc, material, n=32, r_top=None, smooth=True):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=n, radius1=r, radius2=r if r_top is None else r_top, depth=h)
    for v in bm.verts:
        v.co += Vector((loc[0], loc[1], loc[2] + h / 2))
    ob = new_obj(name, bm, material, smooth)
    if smooth:
        m = ob.modifiers.new("ws", "WEIGHTED_NORMAL") if hasattr(bpy.types, "WeightedNormalModifier") else None
        ob.data.use_auto_smooth = True
        ob.data.auto_smooth_angle = math.radians(40)
        del m
    return ob


def sphere(name, r, loc, material, scale=(1, 1, 1)):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=24, v_segments=14, radius=r)
    for v in bm.verts:
        v.co = Vector((v.co.x * scale[0] + loc[0], v.co.y * scale[1] + loc[1], v.co.z * scale[2] + loc[2]))
    return new_obj(name, bm, material, True)


def lathe(name, profile, material, n=40, loc=(0, 0, 0)):
    """Surface of revolution around z from a (radius, z) profile."""
    bm = bmesh.new()
    rings = []
    for (r, z) in profile:
        ring = [bm.verts.new((loc[0] + r * math.cos(2 * math.pi * i / n), loc[1] + r * math.sin(2 * math.pi * i / n), loc[2] + z))
                for i in range(n)]
        rings.append(ring)
    for a, b in zip(rings[:-1], rings[1:]):
        for i in range(n):
            bm.faces.new([a[i], a[(i + 1) % n], b[(i + 1) % n], b[i]])
    if profile[0][0] > 1e-4:
        bm.faces.new(list(reversed(rings[0])))
    if profile[-1][0] > 1e-4:
        bm.faces.new(rings[-1])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    ob = new_obj(name, bm, material, True)
    ob.data.use_auto_smooth = True
    ob.data.auto_smooth_angle = math.radians(50)
    return ob


def sheet(name, nx, ny, fn, material, smooth=True):
    """A grid surface: fn(u, v) -> (x, y, z), u and v in 0..1; UVs follow (u, v)."""
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    verts = [[bm.verts.new(fn(i / nx, j / ny)) for j in range(ny + 1)] for i in range(nx + 1)]
    for i in range(nx):
        for j in range(ny):
            f = bm.faces.new([verts[i][j], verts[i + 1][j], verts[i + 1][j + 1], verts[i][j + 1]])
            for lp, (a, b) in zip(f.loops, [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]):
                lp[uv].uv = (a / nx, b / ny)
    return new_obj(name, bm, material, smooth)


def join(objs, name):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.convert(target="MESH")  # applies modifiers
    bpy.ops.object.join()
    ob = bpy.context.view_layer.objects.active
    ob.name = name
    return ob


def export(name):
    bpy.context.view_layer.update()
    B.export_glb(sky.out_path(f"{name}.glb"))  # noqa: F821
    for o in list(bpy.context.scene.objects):
        bpy.data.objects.remove(o, do_unlink=True)


# ============================================================================================
WALNUT = ("walnut", (92, 56, 34), 0.45)
LEATHER = ("leather", (34, 52, 38), 0.55)
BRASS = ("brass", (210, 168, 92), 0.28, 1.0)


def m(spec):
    return mat(spec[0], spec[1], spec[2], spec[3] if len(spec) > 3 else 0.0)


def desk():
    """A partners' pedestal desk: molded top with a green leather inset, two drawer pedestals, a
    raised-panel modesty front, a plinth and brass pulls. Front (visitor side) faces -y (engine +Z)."""
    W, D, H = 2.1, 1.0, 0.78
    wal, lea, bra = m(WALNUT), m(LEATHER), m(BRASS)
    o = []
    o.append(box("top", (W, D, 0.045), (0, 0, H - 0.0225), wal, bevel=0.012, segments=3))
    o.append(box("top_lip", (W - 0.04, D - 0.04, 0.03), (0, 0, H - 0.06), wal, bevel=0.008))
    o.append(box("inset", (W - 0.36, D - 0.32, 0.004), (0, 0.04, H + 0.002), lea, bevel=0.002, segments=1))
    for sx in (-1, 1):
        cx = sx * (W / 2 - 0.32)
        o.append(box("ped", (0.56, D - 0.1, H - 0.17), (cx, 0, 0.09 + (H - 0.17) / 2), wal, bevel=0.01))
        o.append(box("plinth", (0.62, D - 0.04, 0.09), (cx, 0, 0.045), wal, bevel=0.01))
        # drawers on the chancellor's side (+y in Blender = engine -Z)
        for k in range(3):
            z = 0.16 + k * 0.19
            o.append(box("drawer", (0.48, 0.02, 0.16), (cx, D / 2 - 0.05 + 0.01, z + 0.08), wal, bevel=0.006))
            o.append(box("pull", (0.12, 0.025, 0.018), (cx, D / 2 - 0.02, z + 0.1), bra, bevel=0.004))
        # raised panels on the visitor side and the outer flanks
        for face_y in (-(D / 2 - 0.05) - 0.012,):
            o.append(box("panel", (0.42, 0.02, 0.44), (cx, face_y, 0.38), wal, bevel=0.015, segments=3))
        o.append(box("side_panel", (0.02, D - 0.3, 0.44), (cx + sx * 0.29, 0, 0.38), wal, bevel=0.012, segments=3))
    # modesty panel between the pedestals, with two raised panels
    o.append(box("modesty", (W - 1.24, 0.03, 0.5), (0, -(D / 2 - 0.12), 0.4), wal, bevel=0.006))
    for sx in (-1, 1):
        o.append(box("mpanel", (0.36, 0.02, 0.34), (sx * 0.22, -(D / 2 - 0.12) - 0.02, 0.4), wal, bevel=0.014, segments=3))
    # center drawer under the top, brass escutcheon
    o.append(box("center_drawer", (W - 1.3, 0.02, 0.09), (0, D / 2 - 0.04, H - 0.13), wal, bevel=0.005))
    o.append(box("center_pull", (0.18, 0.02, 0.02), (0, D / 2 - 0.02, H - 0.13), bra, bevel=0.004))
    for ob in o:
        ob.data.materials[0] = ob.data.materials[0]
    join(o, "desk")
    export("desk")


def banker_lamp():
    bra, glass = m(BRASS), mat("glass_green", (36, 96, 52), 0.12, 0.0)
    inner = mat("shade_inner", (255, 236, 200), 0.6, 0.0, emission=(1.0, 0.85, 0.6))
    o = [lathe("base", [(0.0, 0.0), (0.11, 0.0), (0.115, 0.01), (0.1, 0.03), (0.04, 0.045), (0.022, 0.06), (0.0, 0.062)], bra)]
    o.append(cylinder("stem", 0.012, 0.3, (0, 0.0, 0.06), bra, n=16))
    o.append(box("arm", (0.02, 0.15, 0.015), (0, 0.06, 0.36), bra, bevel=0.004))
    for s in (-1, 1):
        o.append(box("yoke", (0.012, 0.012, 0.05), (s * 0.12, 0.08, 0.35), bra, bevel=0.003))

    # the shade: half of an elliptic tube, axis along x
    def shade(u, v, r=1.0):
        a = math.pi * (0.06 + 0.88 * v)
        return (-0.2 + 0.4 * u, 0.08 + 0.11 * math.cos(a) * r, 0.33 + 0.085 * math.sin(a) * r)
    outer = sheet("shade", 2, 18, shade, glass)
    o.append(outer)
    o.append(sheet("shade_in", 2, 18, lambda u, v: shade(1 - u, v, 0.94), inner))
    for s in (-1, 1):
        o.append(box("cap", (0.006, 0.2, 0.09), (s * 0.2, 0.08, 0.37), bra, bevel=0.002))
    join(o, "banker_lamp")
    export("banker_lamp")


def flag_indoor():
    """A ceremonial floor flag: weighted base, a turned pole with a brass spear finial and a cloth
    hanging from the pole in deep vertical folds (cloth UV: u from the pole, v from the bottom)."""
    bra = m(BRASS)
    pole = mat("pole_dark", (40, 26, 18), 0.35, 0.0)
    o = [lathe("fbase", [(0.0, 0.0), (0.24, 0.0), (0.24, 0.03), (0.18, 0.06), (0.06, 0.1), (0.04, 0.16), (0.0, 0.16)], bra)]
    o.append(cylinder("pole", 0.018, 2.75, (0, 0, 0.12), pole, n=16))
    o.append(lathe("spear", [(0.0, 2.86), (0.03, 2.88), (0.012, 2.9), (0.045, 2.95), (0.0, 3.08)], bra, n=16))
    join(o, "flag_pole_in")
    export("flag_pole_in")
    cloth = mat("cloth", (255, 255, 255), 0.85)

    def drape(u, v):
        # cloth width 1.2 m attached from z=1.55 to 2.75 along the pole; it hangs down and folds
        top, H, Wd = 2.78, 1.3, 1.15
        x = 0.03 + u * Wd * (0.86 + 0.14 * v)
        z = top - (1 - v) * H - u * u * 0.55 * (1 - v * 0.2)
        fold = math.sin(u * 13.0 + 0.6) * (0.035 + 0.075 * u) * (0.55 + 0.45 * (1 - v))
        y = fold + math.sin(u * 5.0) * 0.03 * u
        return (x, y, z)
    sheet("flag_cloth_in", 40, 24, drape, cloth)
    export("flag_cloth_in")


def curtain(name, width=1.15, height=3.7, pleats=9):
    vel = mat("velvet", (98, 22, 28), 0.85)

    def f(u, v):
        x = u * width * (0.75 + 0.25 * v)                 # gathered toward the bottom tieback
        y = math.sin(u * pleats * 2 * math.pi) * 0.045 + math.sin(u * pleats * math.pi + 1.3) * 0.02
        z = v * height
        return (x - width / 2, y, z)
    sheet(name, pleats * 8, 12, f, vel)
    export(name)


def desk_set():
    """Fountain pen, inkwell, blotter rocker, a pen tray; one model each."""
    lac = mat("lacquer", (14, 12, 12), 0.18)
    bra = m(BRASS)
    o = [cylinder("barrel", 0.0065, 0.1, (0, 0, 0), lac, n=20), cylinder("cap", 0.0072, 0.055, (0, 0, 0.1), lac, n=20),
         cylinder("band", 0.0074, 0.006, (0, 0, 0.098), bra, n=20), cylinder("tip", 0.0065, 0.02, (0, 0, -0.02), bra, n=20, r_top=0.0065)]
    o[-1].location = (0, 0, 0)
    pen = join(o, "pen")
    pen.rotation_euler = (math.radians(90), 0, 0)
    bpy.ops.object.transform_apply(rotation=True)
    export("pen")
    ink = mat("ink_glass", (40, 50, 60), 0.05)
    o = [box("well", (0.07, 0.07, 0.055), (0, 0, 0.0275), ink, bevel=0.006), cylinder("lid", 0.024, 0.014, (0, 0, 0.055), bra, n=24),
         box("tray", (0.26, 0.11, 0.012), (0.12, 0, 0.006), bra, bevel=0.004)]
    join(o, "inkwell")
    export("inkwell")


def pedestal():
    mar = mat("marble", (226, 222, 214), 0.25)
    o = [box("plinth", (0.46, 0.46, 0.08), (0, 0, 0.04), mar, bevel=0.01), cylinder("shaft", 0.15, 0.95, (0, 0, 0.08), mar, n=40),
         box("cap", (0.42, 0.42, 0.06), (0, 0, 1.06), mar, bevel=0.01)]
    join(o, "pedestal")
    export("pedestal")


def sconce():
    bra = m(BRASS)
    glow = mat("sconce_glass", (255, 232, 196), 0.3, 0.0, emission=(1.0, 0.8, 0.55))
    o = [box("plate", (0.12, 0.03, 0.22), (0, 0.015, 0), bra, bevel=0.008), box("arm", (0.02, 0.18, 0.02), (0, 0.1, -0.02), bra, bevel=0.005)]
    o.append(lathe("bobeche", [(0.0, -0.03), (0.05, -0.03), (0.05, -0.02), (0.0, -0.02)], bra, loc=(0, 0.19, 0)))
    o.append(lathe("shade", [(0.03, -0.02), (0.075, 0.12), (0.07, 0.125), (0.025, -0.015)], glow, loc=(0, 0.19, 0)))
    join(o, "sconce")
    export("sconce")


B.reset_scene()
made = []
for part, fn in (("desk", desk), ("lamp", banker_lamp), ("flag", flag_indoor), ("desk_set", desk_set), ("pedestal", pedestal),
                 ("sconce", sconce)):
    if JOB.get(part, True):
        fn()
        made.append(part)
if JOB.get("curtains", True):
    curtain("curtain")
    made.append("curtain")
sky.result(models=made)  # noqa: F821
