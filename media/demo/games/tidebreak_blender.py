"""Blender scripts the Tidebreak Isle crew runs through the engine's DCC bridge (dcc_run_script).

They model what no CC0 library has in the right form: coconut palms (three variants), a
seagull with separately exported wings (so the engine can flap them), and the round
watchtower and wall pieces split out of Poly Haven's modular fort set. Everything is
deterministic (seeded) and written to the project's downloads/ folder (rebuilt on demand).
"""

PALMS = r'''
import math, random
import bmesh, bpy
from mathutils import Vector, Matrix
from skywalker_dcc import blender as B


def palm(seed, height, lean, n_fronds):
    rnd = random.Random(seed)
    B.reset_scene()
    trunk_mat = B.make_material("palm_trunk", (0.30, 0.26, 0.21), roughness=0.92, vertex_colors=True)
    leaf_mat = B.make_material("palm_leaf", (0.2, 0.34, 0.1), roughness=0.62, vertex_colors=True)
    nut_mat = B.make_material("palm_nut", (0.36, 0.33, 0.14), roughness=0.55)
    leaf_mat.use_backface_culling = False

    # --- trunk: a leaning, slightly S-curved column with growth rings ---------------------
    me = bpy.data.meshes.new("trunk")
    bm = bmesh.new()
    segs, sides = 64, 14
    ph = rnd.uniform(0, 6.28)
    def center(t):
        x = lean * height * (t ** 1.6) + 0.18 * math.sin(t * 3.0 + ph) * t
        y = 0.12 * math.sin(t * 2.3 + ph * 0.7) * t
        return Vector((x, y, t * height))
    rings, colors = [], []
    for i in range(segs + 1):
        t = i / segs
        c = center(t)
        d = (center(min(1, t + 0.01)) - center(max(0, t - 0.01))).normalized()
        ref = Vector((0, 1, 0))
        u = d.cross(ref).normalized()
        v = d.cross(u).normalized()
        s_m = t * height
        ring = 1.0 + 0.045 * abs(math.sin(math.pi * s_m / 0.22))
        r = (0.2 * (1 - 0.32 * t) + 0.16 * math.exp(-t * 18)) * ring
        row = []
        for k in range(sides):
            a = 2 * math.pi * k / sides
            row.append(bm.verts.new(c + (u * math.cos(a) + v * math.sin(a)) * r * (1 + 0.03 * math.sin(a * 3 + i))))
        rings.append(row)
        g = 0.75 + 0.25 * abs(math.sin(math.pi * s_m / 0.22)) - 0.12 * (1 - t)
        colors.append(g)
    face_cols = []
    for i in range(segs):
        for k in range(sides):
            f = bm.faces.new((rings[i][k], rings[i][(k + 1) % sides], rings[i + 1][(k + 1) % sides], rings[i + 1][k]))
            g = colors[i] * rnd.uniform(0.93, 1.05)
            face_cols.append((0.17 * g, 0.125 * g, 0.085 * g))  # linear albedo (vertex colors drive the base color)
    for f in bm.faces:  # outward normals (the ring order alone does not guarantee it)
        f.normal_update()
        c = f.calc_center_median()
        t = max(0.0, min(1.0, c.z / height))
        if f.normal.dot(c - center(t)) < 0:
            f.normal_flip()
    bm.to_mesh(me)
    trunk = bpy.data.objects.new("trunk", me)
    B.link_to_scene(trunk)
    trunk.data.materials.append(trunk_mat)
    B.set_vertex_colors(trunk, face_cols)
    for p in trunk.data.polygons:
        p.use_smooth = True
    top = center(1.0)

    # --- crown: fronds (rachis + V-folded leaflets that droop toward the tips) ------------
    me = bpy.data.meshes.new("crown")
    bm = bmesh.new()
    fcols = []
    def frond(yaw, pitch, length, droop, tone, dead=False):
        n = 34
        dirh = Vector((math.cos(yaw), math.sin(yaw), 0))
        pts = []
        p = top.copy()
        for i in range(n + 1):
            s = i / n
            ang = pitch - droop * s * s
            seg = (dirh * math.cos(ang) + Vector((0, 0, math.sin(ang)))) * (length / n)
            pts.append(p.copy())
            p = p + seg
        side = Vector((-dirh.y, dirh.x, 0))
        # rachis as a thin ribbon
        for i in range(n):
            w = 0.035 * (1 - i / n) + 0.008
            a, b = pts[i], pts[i + 1]
            bm.faces.new((bm.verts.new(a - side * w), bm.verts.new(a + side * w), bm.verts.new(b + side * w * 0.9),
                          bm.verts.new(b - side * w * 0.9)))
            fcols.append(tuple(c * tone for c in ((0.16, 0.15, 0.05) if not dead else (0.2, 0.13, 0.06))))
        for i in range(4, n):
            s = i / n
            base = pts[i]
            fwd = (pts[i + 1] - pts[i]).normalized()
            L = length * 0.3 * (math.sin(math.pi * min(1, s * 1.05)) ** 0.7) * rnd.uniform(0.85, 1.1)
            if L < 0.05:
                continue
            for sgn in (-1, 1):
                out = (side * sgn * 0.9 + fwd * 0.55).normalized()
                hang = 0.55 + 0.35 * s + (0.6 if dead else 0)
                w = 0.045 * rnd.uniform(0.8, 1.2)
                prev_l = prev_r = prev_c = None
                segsl = 3
                for j in range(segsl + 1):
                    q = j / segsl
                    c = base + out * L * q - Vector((0, 0, 1)) * L * hang * q * q
                    ww = w * math.sin(math.pi * min(0.98, 0.15 + q * 0.85))
                    fold = Vector((0, 0, ww * 0.6))
                    cl = bm.verts.new(c + fwd * ww + fold)
                    cr = bm.verts.new(c - fwd * ww + fold)
                    cc = bm.verts.new(c)
                    if prev_l is not None:
                        bm.faces.new((prev_c, prev_l, cl, cc))
                        bm.faces.new((prev_r, prev_c, cc, cr))
                        if dead:
                            col = (0.24, 0.16, 0.07)
                        else:
                            y = max(0, s - 0.6) * 0.5 + rnd.uniform(-0.05, 0.05)
                            col = (0.055 + y * 0.12, 0.12 + y * 0.05, 0.022)
                        fcols.append(tuple(v * tone for v in col))
                        fcols.append(tuple(v * tone * 0.92 for v in col))
                    prev_l, prev_r, prev_c = cl, cr, cc
    golden = math.pi * (3 - math.sqrt(5))
    for f in range(n_fronds):
        yaw = f * golden * 2 + rnd.uniform(-0.15, 0.15)
        young = f % 3 == 0
        pitch = math.radians(rnd.uniform(48, 62) if young else rnd.uniform(18, 38))
        frond(yaw, pitch, rnd.uniform(3.0, 4.2) * (0.8 if young else 1.0), math.radians(rnd.uniform(70, 110)),
              rnd.uniform(0.85, 1.08))
    for f in range(2):
        frond(rnd.uniform(0, 6.28), math.radians(-60), 2.6, math.radians(25), 1.0, dead=True)
    for f in bm.faces:  # leaves face the sky (lit from above, translucent from below)
        f.normal_update()
        if f.normal.z < 0:
            f.normal_flip()
    bm.to_mesh(me)
    crown = bpy.data.objects.new("crown", me)
    B.link_to_scene(crown)
    crown.data.materials.append(leaf_mat)
    B.set_vertex_colors(crown, fcols)

    # --- coconuts -------------------------------------------------------------------------
    nuts = []
    for k in range(rnd.randint(4, 7)):
        a = rnd.uniform(0, 6.28)
        bpy.ops.mesh.primitive_uv_sphere_add(segments=12, ring_count=8, radius=rnd.uniform(0.09, 0.12),
                                             location=top + Vector((math.cos(a) * 0.2, math.sin(a) * 0.2, -0.25 - rnd.uniform(0, 0.15))))
        o = bpy.context.active_object
        o.scale = (1, 1, 1.15)
        o.data.materials.append(nut_mat)
        bpy.ops.object.shade_smooth()
        nuts.append(o)
    objs = [trunk, crown] + nuts
    return objs


results = []
for name, seed, height, lean, n in (("palm_a", 3, 9.5, 0.10, 16), ("palm_b", 7, 7.5, 0.22, 14), ("palm_c", 11, 11.5, 0.06, 18),
                                    ("palm_d", 19, 5.5, 0.34, 20), ("palm_e", 23, 13.5, 0.04, 15)):
    objs = palm(seed, height, lean, n)
    B.export_glb(sky.out_path(name + ".glb"), objs)
    results.append({"name": name, **B.stats(objs)})
sky.result(palms=results)
'''

GULL = r'''
import math
import bmesh, bpy
from mathutils import Vector
from skywalker_dcc import blender as B

# A herring-gull: white body and head, pale grey back, black wing tips, yellow bill.
# Blender axes: -Y is forward (glTF +Z), X is the right wing, Z is up.
B.reset_scene()
white = B.make_material("gull_white", (0.86, 0.87, 0.88), roughness=0.75)
grey = B.make_material("gull_grey", (0.55, 0.58, 0.62), roughness=0.7)
black = B.make_material("gull_black", (0.05, 0.05, 0.06), roughness=0.6)
bill = B.make_material("gull_bill", (0.85, 0.65, 0.12), roughness=0.4)

bpy.ops.mesh.primitive_uv_sphere_add(segments=16, ring_count=10, radius=1)
body = bpy.context.active_object
body.scale = (0.075, 0.24, 0.07)
body.data.materials.append(white)
bpy.ops.mesh.primitive_uv_sphere_add(segments=12, ring_count=8, radius=0.055, location=(0, -0.22, 0.045))
head = bpy.context.active_object
head.data.materials.append(white)
bpy.ops.mesh.primitive_cone_add(vertices=8, radius1=0.014, radius2=0.002, depth=0.07, location=(0, -0.3, 0.035), rotation=(math.radians(90), 0, 0))
beak = bpy.context.active_object
beak.data.materials.append(bill)
bpy.ops.mesh.primitive_cone_add(vertices=4, radius1=0.06, radius2=0.0, depth=0.12, location=(0, 0.25, 0.01), rotation=(math.radians(-90), 0, 0))
tail = bpy.context.active_object
tail.scale = (1.4, 1, 0.25)
tail.data.materials.append(white)
for o in (body, head, beak, tail):
    o.select_set(True)
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.shade_smooth()
bpy.ops.object.select_all(action="DESELECT")
B.export_glb(sky.out_path("gull_body.glb"), [body, head, beak, tail])


def wing(sign):
    """A cambered, tapering wing from the shoulder (origin) outwards along sign*X."""
    me = bpy.data.meshes.new("wing")
    bm = bmesh.new()
    span, n = 0.68, 10
    rows = []
    for i in range(n + 1):
        s = i / n
        x = sign * span * s
        chord = 0.17 * (1 - s) ** 0.6 + 0.04
        lead = -0.06 - 0.06 * s          # leading edge sweeps back
        z = 0.02 * math.sin(math.pi * s) - 0.03 * s * s
        rows.append((bm.verts.new((x, lead, z)), bm.verts.new((x, lead + chord * 0.5, z + 0.012)),
                     bm.verts.new((x, lead + chord, z))))
    faces = []
    for i in range(n):
        a, b = rows[i], rows[i + 1]
        tip = (i / n) > 0.72
        faces.append((bm.faces.new((a[0], b[0], b[1], a[1])), tip))
        faces.append((bm.faces.new((a[1], b[1], b[2], a[2])), tip))
    bm.to_mesh(me)
    o = bpy.data.objects.new("wing", me)
    B.link_to_scene(o)
    o.data.materials.append(grey)
    o.data.materials.append(black)
    for poly, (f, tip) in zip(o.data.polygons, faces):
        poly.material_index = 1 if tip else 0
        poly.use_smooth = True
    sol = o.modifiers.new("thick", "SOLIDIFY")
    sol.thickness = 0.008
    return o


right, left = wing(1), wing(-1)
B.export_glb(sky.out_path("gull_wing_r.glb"), [right])
B.export_glb(sky.out_path("gull_wing_l.glb"), [left])
sky.result(ok=True)
'''

SPLIT = r'''
import bpy
from mathutils import Matrix, Vector
from skywalker_dcc import blender as B

# Poly Haven's modular fort ships every module in one mesh per material: split it into its
# loose parts, regroup the parts whose footprints touch (one module each), and export the
# modules asked for (sky.ARGS = module ids) with the origin at the bottom center.
B.reset_scene()
objs = B.import_file(sky.project_path(sky.ARGS[0]))
meshes = [o for o in objs if o.type == "MESH"]
bpy.ops.object.select_all(action="DESELECT")
for o in meshes:
    o.select_set(True)
bpy.context.view_layer.objects.active = meshes[0]
bpy.ops.object.join()
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.mesh.separate(type="LOOSE")
bpy.ops.object.mode_set(mode="OBJECT")
parts = [o for o in bpy.context.scene.objects if o.type == "MESH"]


def bbox(o):
    ws = [o.matrix_world @ Vector(c) for c in o.bound_box]
    return [min(v[k] for v in ws) for k in range(3)], [max(v[k] for v in ws) for k in range(3)]


boxes = [bbox(o) for o in parts]
parent = list(range(len(parts)))


def find(i):
    while parent[i] != i:
        parent[i] = parent[parent[i]]
        i = parent[i]
    return i


for i in range(len(parts)):
    a0, a1 = boxes[i]
    for j in range(i + 1, len(parts)):
        b0, b1 = boxes[j]
        if all(a0[k] - 0.15 <= b1[k] and b0[k] - 0.15 <= a1[k] for k in (0, 1)):
            parent[find(i)] = find(j)
clusters = {}
for i in range(len(parts)):
    clusters.setdefault(find(i), []).append(i)
order = sorted(clusters.values(), key=lambda idx: min(boxes[i][0][0] for i in idx) * 1000 + min(boxes[i][0][1] for i in idx))
want = {int(a) for a in sky.ARGS[2:]}
listing = []
done = []
for n, idx in enumerate(order):
    lo = [min(boxes[i][0][k] for i in idx) for k in range(3)]
    hi = [max(boxes[i][1][k] for i in idx) for k in range(3)]
    listing.append({"id": n, "center": [round((lo[k] + hi[k]) / 2, 2) for k in range(3)], "size": [round(hi[k] - lo[k], 2) for k in range(3)]})
    if n not in want:
        continue
    bpy.ops.object.select_all(action="DESELECT")
    for i in idx:
        parts[i].select_set(True)
    bpy.context.view_layer.objects.active = parts[idx[0]]
    if len(idx) > 1:
        bpy.ops.object.join()
    o = bpy.context.view_layer.objects.active
    o.data.transform(Matrix.Translation((-(lo[0] + hi[0]) / 2, -(lo[1] + hi[1]) / 2, -lo[2])))
    B.export_glb(sky.out_path("%s_%d.glb" % (sky.ARGS[1], n)), [o])
    done.append({"id": n, "size": [round(hi[k] - lo[k], 2) for k in range(3)]})
sky.result(modules=done, listing=listing)
'''
