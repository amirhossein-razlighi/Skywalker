# Procedural sports coupe for the vehicle demo (original design, CC0).
#
#   /Applications/Blender.app/Contents/MacOS/Blender -b --python make_coupe.py -- <output dir>
#
# Builds the car in Blender units (meters; +Y forward, +Z up, the origin on the ground between
# the axles) and exports one single-material .glb per part, so the engine scene can give every
# part its own material and parent the wheels under the chassis:
#   coupe_body (paint), coupe_glass, coupe_trim (black plastic), coupe_light_front,
#   coupe_light_rear, coupe_chrome, wheel_tire, wheel_rim (outer face toward +X).
# glTF export turns +Y forward into -Z forward, the engine's convention.

import math
import os
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

OUT = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else os.path.dirname(os.path.abspath(__file__))

WHEEL_R = 0.345
WHEEL_W = 0.255
FRONT_AXLE = 1.38
REAR_AXLE = -1.24
TRACK = 0.80  # wheel center x


def lerp_keys(keys, values, y):
    """Piecewise-linear profile: keys descending (front to rear)."""
    if y >= keys[0]:
        return values[0]
    for i in range(len(keys) - 1):
        a, b = keys[i], keys[i + 1]
        if a >= y >= b:
            t = (a - y) / (a - b)
            return values[i] * (1 - t) + values[i + 1] * t
    return values[-1]


def smooth_keys(keys, values, y):
    """Smoothstep between keys (no kinks in the loft)."""
    if y >= keys[0]:
        return values[0]
    for i in range(len(keys) - 1):
        a, b = keys[i], keys[i + 1]
        if a >= y >= b:
            t = (a - y) / (a - b)
            t = t * t * (3 - 2 * t)
            return values[i] * (1 - t) + values[i + 1] * t
    return values[-1]


def superellipse_ring(a, zc, b, n_side, m_top, count, taper_top=0.0, taper_bottom=0.0):
    pts = []
    for k in range(count):
        th = 2 * math.pi * k / count
        c, s = math.cos(th), math.sin(th)
        x = a * math.copysign(abs(c) ** (2.0 / n_side), c)
        zn = math.copysign(abs(s) ** (2.0 / m_top), s)
        if zn > 0:
            x *= 1.0 - taper_top * zn
        else:
            x *= 1.0 - taper_bottom * (-zn)
        pts.append((x, zc + b * zn))
    return pts


def loft(name, stations, ring_fn, count):
    """Skins rings (one per station y) into a closed mesh with fan caps."""
    verts, faces = [], []
    for y in stations:
        for (x, z) in ring_fn(y):
            verts.append((x, y, z))
    rings = len(stations)
    for r in range(rings - 1):
        for k in range(count):
            a = r * count + k
            b = r * count + (k + 1) % count
            c = (r + 1) * count + (k + 1) % count
            d = (r + 1) * count + k
            faces.append((a, d, c, b))
    # caps: center vertex fans
    for r, flip in ((0, False), (rings - 1, True)):
        cx = sum(verts[r * count + k][0] for k in range(count)) / count
        cz = sum(verts[r * count + k][2] for k in range(count)) / count
        verts.append((cx, stations[r], cz))
        ci = len(verts) - 1
        for k in range(count):
            a = r * count + k
            b = r * count + (k + 1) % count
            faces.append((ci, b, a) if not flip else (ci, a, b))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.validate()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def apply_modifiers(obj):
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    for m in list(obj.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)
    obj.select_set(False)


def subdivide(obj, levels=2):
    m = obj.modifiers.new("sub", "SUBSURF")
    m.levels = levels
    m.render_levels = levels
    apply_modifiers(obj)


def smooth(obj, angle=40):
    for p in obj.data.polygons:
        p.use_smooth = True
    obj.data.use_auto_smooth = True
    obj.data.auto_smooth_angle = math.radians(angle)


def recalc_normals(obj):
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(obj.data)
    bm.free()


def boolean(obj, cutter, op="DIFFERENCE"):
    m = obj.modifiers.new("bool", "BOOLEAN")
    m.operation = op
    m.solver = "EXACT"
    m.object = cutter
    apply_modifiers(obj)


def cylinder_x(name, radius, length, center, segments=48, cap=True):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=cap, cap_tris=False, segments=segments, radius1=radius, radius2=radius, depth=length)
    bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0), matrix=Matrix.Rotation(math.radians(90), 3, "Y"))
    bmesh.ops.translate(bm, verts=bm.verts, vec=Vector(center))
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def box(name, size, center, rot=(0, 0, 0), bevel=0.0):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bevel > 0:
        bmesh.ops.bevel(bm, geom=list(bm.edges), offset=bevel, segments=3, affect="EDGES", profile=0.5)
    r = Matrix.Rotation(rot[0], 3, "X") @ Matrix.Rotation(rot[1], 3, "Y") @ Matrix.Rotation(rot[2], 3, "Z")
    bmesh.ops.rotate(bm, verts=bm.verts, cent=(0, 0, 0), matrix=r)
    bmesh.ops.translate(bm, verts=bm.verts, vec=Vector(center))
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def join(objs, name):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.join()
    objs[0].name = name
    objs[0].data.name = name
    return objs[0]


def export(obj, filename):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    path = os.path.join(OUT, filename)
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True, export_apply=True,
                              export_yup=True, export_materials="NONE", export_normals=True, export_texcoords=True)
    print("wrote", path)


# ---------------------------------------------------------------------------
# Scene
# ---------------------------------------------------------------------------
bpy.ops.wm.read_factory_settings(use_empty=True)

# Lower body: a lofted superellipse shell, long hood, short ducktail. Materials are assigned on the
# loft's control quads (by station and ring angle), so after subdivision every panel boundary
# (lights, intake, diffuser, roof, pillars) follows the body's own flow lines.
Y_KEYS = [2.27, 2.20, 2.05, 1.80, 1.38, 1.00, 0.70, 0.00, -0.80, -1.24, -1.70, -1.98, -2.10, -2.16]
HALF_W = [0.50, 0.74, 0.86, 0.915, 0.945, 0.915, 0.900, 0.905, 0.925, 0.955, 0.945, 0.90, 0.83, 0.70]
Z_BOT = [0.30, 0.24, 0.20, 0.17, 0.16, 0.15, 0.15, 0.15, 0.15, 0.16, 0.17, 0.22, 0.28, 0.36]
Z_TOP = [0.50, 0.60, 0.67, 0.74, 0.80, 0.83, 0.86, 0.88, 0.89, 0.905, 0.92, 0.95, 0.965, 0.88]
stations = [Y_KEYS[0] - i * (Y_KEYS[0] - Y_KEYS[-1]) / 60 for i in range(61)]
RING = 48

PAINT, LIGHT_F, LIGHT_R, TRIM, GLASS = 0, 1, 2, 3, 4


def body_ring(y):
    a = smooth_keys(Y_KEYS, HALF_W, y)
    zb = smooth_keys(Y_KEYS, Z_BOT, y)
    zt = smooth_keys(Y_KEYS, Z_TOP, y)
    return superellipse_ring(a, (zb + zt) / 2, (zt - zb) / 2, 3.6, 2.6, RING, taper_top=0.15, taper_bottom=0.07)


def angle_in(theta, lo, hi):
    d = math.degrees(theta) % 360.0
    return lo <= d <= hi


def body_material(y, theta):
    # Headlights: slim wedges on the upper corners of the nose.
    if 1.86 <= y <= 2.12 and (angle_in(theta, 22, 52) or angle_in(theta, 128, 158)):
        return LIGHT_F
    # Tail lights: a full-width bar across the ducktail.
    if -2.135 <= y <= -2.05 and angle_in(theta, 36, 144):
        return LIGHT_R
    # Intake under the nose, diffuser under the tail.
    if y >= 2.06 and angle_in(theta, 205, 335):
        return TRIM
    if y <= -1.96 and angle_in(theta, 215, 325):
        return TRIM
    return PAINT


def loft_materials(obj, stations_list, count, material_fn):
    for poly in obj.data.polygons:
        if poly.index >= (len(stations_list) - 1) * count:
            poly.material_index = PAINT  # end caps
            continue
        r, k = divmod(poly.index, count)
        y = (stations_list[r] + stations_list[r + 1]) / 2
        theta = 2 * math.pi * (k + 0.5) / count
        poly.material_index = material_fn(y, theta)


def by_material(src, name, index, lift=0.0, thickness=0.0):
    """Faces of `src` with a material index become their own object (lifted off the surface)."""
    mesh = src.data.copy()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index != index], context="FACES")
    if lift:
        bm.normal_update()
        for v in bm.verts:
            v.co += v.normal * lift
    if thickness:
        bmesh.ops.solidify(bm, geom=bm.faces[:], thickness=thickness)
    bm.to_mesh(mesh)
    bm.free()
    return obj


def drop_material(src, index):
    bm = bmesh.new()
    bm.from_mesh(src.data)
    bmesh.ops.delete(bm, geom=[f for f in bm.faces if f.material_index == index], context="FACES")
    bm.to_mesh(src.data)
    bm.free()


body = loft("coupe_body", stations, body_ring, RING)
loft_materials(body, stations, RING, body_material)
subdivide(body, 1)
# Wheel arches.
for (y, side) in ((FRONT_AXLE, 1), (FRONT_AXLE, -1), (REAR_AXLE, 1), (REAR_AXLE, -1)):
    cut = cylinder_x("arch", WHEEL_R + 0.045, 0.6, (side * 0.95, y, WHEEL_R + 0.005), segments=56)
    boolean(body, cut)
    bpy.data.objects.remove(cut)
recalc_normals(body)
front_lights = by_material(body, "coupe_light_front", LIGHT_F, lift=0.002)
rear_lights = by_material(body, "coupe_light_rear", LIGHT_R, lift=0.002)
body_trim = by_material(body, "body_trim", TRIM, lift=0.002)
for index in (LIGHT_F, LIGHT_R, TRIM):
    drop_material(body, index)  # those panels now live in their own objects
smooth(body, 35)

# Greenhouse: windshield, side windows and a fastback rear window in glass; roof and pillars painted.
C_KEYS = [0.82, 0.55, 0.25, -0.05, -0.45, -0.90, -1.35, -1.75]
C_TOP = [0.84, 1.02, 1.17, 1.24, 1.225, 1.13, 1.01, 0.93]
C_HALF = [0.74, 0.70, 0.66, 0.65, 0.65, 0.64, 0.62, 0.56]
c_stations = [C_KEYS[0] - i * (C_KEYS[0] - C_KEYS[-1]) / 34 for i in range(35)]
CRING = 40


def cabin_ring(y):
    top = smooth_keys(C_KEYS, C_TOP, y)
    a = smooth_keys(C_KEYS, C_HALF, y)
    base = 0.80
    return superellipse_ring(a, base, max(top - base, 0.02), 3.2, 2.4, CRING, taper_top=0.30, taper_bottom=0.0)


def cabin_material(y, theta):
    if -0.80 <= y <= 0.28 and angle_in(theta, 54, 126):
        return PAINT  # roof
    if 0.28 < y <= 0.80 and (angle_in(theta, 50, 63) or angle_in(theta, 117, 130)):
        return PAINT  # A-pillars
    if y < -0.80 and (angle_in(theta, 20, 63) or angle_in(theta, 117, 160)):
        return PAINT  # rear quarters (C-pillars)
    return GLASS


cabin = loft("coupe_glass", c_stations, cabin_ring, CRING)
loft_materials(cabin, c_stations, CRING, cabin_material)
for poly in cabin.data.polygons:
    if poly.index >= (len(c_stations) - 1) * CRING:
        poly.material_index = GLASS
subdivide(cabin, 1)
recalc_normals(cabin)
cabin_paint = by_material(cabin, "cabin_paint", PAINT, lift=0.004, thickness=-0.012)
drop_material(cabin, PAINT)
smooth(cabin, 40)
smooth(cabin_paint, 40)

# Mirrors on short stalks at the front of the side windows.
parts = [body, cabin_paint]
for side in (1, -1):
    parts.append(box("mirror", (0.17, 0.10, 0.085), (side * 0.87, 0.58, 0.95), rot=(0, 0, side * math.radians(8)), bevel=0.025))
    parts.append(box("stalk", (0.12, 0.04, 0.03), (side * 0.77, 0.58, 0.92), bevel=0.008))
body = join(parts, "coupe_body")
smooth(body, 35)

# Black trim: intake and diffuser panels, wheel-well liners.
trim = [body_trim]
for (y, side) in ((FRONT_AXLE, 1), (FRONT_AXLE, -1), (REAR_AXLE, 1), (REAR_AXLE, -1)):
    # Wheel well: an inward-facing liner around the tire (no outer cap), so the arch shows black.
    well = cylinder_x("well", WHEEL_R + 0.05, 0.40, (side * 0.70, y, WHEEL_R + 0.005), segments=48, cap=False)
    bm = bmesh.new()
    bm.from_mesh(well.data)
    inner_x = side * 0.50
    ring = [v for v in bm.verts if abs(v.co.x - inner_x) < 1e-3]
    bmesh.ops.contextual_create(bm, geom=ring)
    for f in bm.faces:
        f.normal_flip()
    bm.to_mesh(well.data)
    bm.free()
    trim.append(well)
trim_obj = join(trim, "coupe_trim")
smooth(trim_obj, 30)

# Chrome: twin exhaust tips under the diffuser.
ex = []
for side in (1, -1):
    o = cylinder_x("exhaust", 0.05, 0.14, (0, 0, 0), segments=28)
    o.rotation_euler = (0, 0, math.radians(90))
    o.location = (side * 0.40, -2.13, 0.27)
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    o.select_set(False)
    ex.append(o)
chrome = join(ex, "coupe_chrome")
smooth(chrome, 40)

glass = cabin

# ---------------------------------------------------------------------------
# Wheel (modeled at the origin, axle along X, outer face toward +X)
# ---------------------------------------------------------------------------
def lathe_x(name, profile, segments=64):
    """Revolves a (radius, x) profile around the X axis."""
    verts, faces = [], []
    n = len(profile)
    for s in range(segments):
        a = 2 * math.pi * s / segments
        for (r, x) in profile:
            verts.append((x, r * math.cos(a), r * math.sin(a)))
    for s in range(segments):
        s2 = (s + 1) % segments
        for i in range(n - 1):
            faces.append((s * n + i, s * n + i + 1, s2 * n + i + 1, s2 * n + i))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.validate()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


hw = WHEEL_W / 2
rim_r = 0.245
tire_profile = [(rim_r - 0.005, -hw + 0.02), (rim_r + 0.02, -hw), (WHEEL_R - 0.03, -hw - 0.005), (WHEEL_R - 0.008, -hw + 0.02),
                (WHEEL_R, -hw + 0.06), (WHEEL_R, hw - 0.06), (WHEEL_R - 0.008, hw - 0.02), (WHEEL_R - 0.03, hw + 0.005),
                (rim_r + 0.02, hw), (rim_r - 0.005, hw - 0.02)]
tire = lathe_x("wheel_tire", tire_profile, 72)
recalc_normals(tire)
smooth(tire, 50)

rim_parts = []
# Barrel and lip.
barrel = lathe_x("barrel", [(rim_r - 0.01, hw - 0.01), (rim_r + 0.008, hw - 0.005), (rim_r + 0.008, hw - 0.025), (rim_r - 0.012, hw - 0.03),
                            (rim_r - 0.012, -hw + 0.01), (rim_r, -hw + 0.005)], 64)
rim_parts.append(barrel)
# Hub and center cap.
rim_parts.append(cylinder_x("hub", 0.075, 0.07, (hw - 0.06, 0, 0), segments=32))
rim_parts.append(cylinder_x("cap", 0.04, 0.02, (hw - 0.02, 0, 0), segments=24))
# Ten spokes in five pairs, slightly dished.
for i in range(5):
    for d in (-0.13, 0.13):
        a = 2 * math.pi * i / 5 + d
        spoke = box("spoke", (0.022, 0.045, 0.17), (0, 0, 0), bevel=0.008)
        spoke.rotation_euler = (a, 0, 0)
        spoke.location = (hw - 0.045, -math.sin(a) * 0.155, math.cos(a) * 0.155)
        bpy.context.view_layer.objects.active = spoke
        spoke.select_set(True)
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        spoke.select_set(False)
        rim_parts.append(spoke)
# Brake disc behind the spokes (spins with the wheel).
rim_parts.append(cylinder_x("disc", 0.175, 0.03, (-0.01, 0, 0), segments=48))
rim = join(rim_parts, "wheel_rim")
recalc_normals(rim)
smooth(rim, 30)

for obj, fname in ((body, "coupe_body.glb"), (glass, "coupe_glass.glb"), (trim_obj, "coupe_trim.glb"),
                   (front_lights, "coupe_light_front.glb"), (rear_lights, "coupe_light_rear.glb"), (chrome, "coupe_chrome.glb"),
                   (tire, "wheel_tire.glb"), (rim, "wheel_rim.glb")):
    export(obj, fname)
