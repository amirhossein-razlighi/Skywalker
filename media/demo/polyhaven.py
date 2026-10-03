"""Poly Haven assets (CC0) for the showcase scenes.

Resolves an asset through Poly Haven's public API, then has a crew member download it with
the engine's own `asset_download` tool, which records the license, author and source in the
asset's .meta and in the project's CREDITS.md. Models keep real-world units (meters).

Downloads land in <project>/downloads/<id>/ and are reused when present (they are not
committed; `showcase.py build` fetches them again on a fresh checkout).
"""
import json
import os
import urllib.request

API = "https://api.polyhaven.com"
UA = {"User-Agent": "Skywalker-Engine-Showcase/0.1 (+https://github.com/amirhossein-razlighi)"}
_cache = {}


def api(path):
    if path not in _cache:
        req = urllib.request.Request(f"{API}/{path}", headers=UA)
        _cache[path] = json.load(urllib.request.urlopen(req, timeout=60))
    return _cache[path]


def _credits(asset_id):
    info = api(f"info/{asset_id}")
    return {"license": "CC0-1.0", "author": ", ".join(info.get("authors", {}).keys()) or "Poly Haven",
            "source_page": f"https://polyhaven.com/a/{asset_id}"}


def _project(b):
    return b.studio.project


def model(b, asset_id, res="2k"):
    """Downloads (or reuses) a glTF model; returns the import result ({prefab?, mesh, material?, ...})."""
    gl = api(f"files/{asset_id}")["gltf"][res]["gltf"]
    folder = f"downloads/{asset_id}"
    name = gl["url"].rsplit("/", 1)[1]
    rel = f"{folder}/{name}"
    if os.path.exists(os.path.join(_project(b), rel)):
        return b.call("asset_import", path=rel, normalize=False)
    include = {k: v["url"] for k, v in gl["include"].items()}
    return b.call("asset_download", url=gl["url"], include=include, folder=folder, normalize=False, **_credits(asset_id))


def hdri(b, asset_id, res="4k"):
    """Downloads (or reuses) an .hdr sky panorama; returns its project path."""
    url = api(f"files/{asset_id}")["hdri"][res]["hdr"]["url"]
    rel = f"downloads/hdri/{url.rsplit('/', 1)[1]}"
    if not os.path.exists(os.path.join(_project(b), rel)):
        b.call("asset_download", url=url, folder="downloads/hdri", **_credits(asset_id))
    return rel


def texture(b, asset_id, res="2k", tiling=1.0, triplanar=True, **material):
    """Downloads (or reuses) a PBR texture set and creates a material; returns the material path."""
    files = api(f"files/{asset_id}")
    urls = {"diff": files["Diffuse"][res]["jpg"]["url"], "nor_gl": files["nor_gl"][res]["jpg"]["url"],
            "arm": files["arm"][res]["jpg"]["url"]}
    folder = f"downloads/{asset_id}"
    names = {k: u.rsplit("/", 1)[1] for k, u in urls.items()}
    if not os.path.exists(os.path.join(_project(b), folder, names["diff"])):
        b.call("asset_download", url=urls["diff"], folder=folder,
               include={names["nor_gl"]: urls["nor_gl"], names["arm"]: urls["arm"]}, **_credits(asset_id))
    path = f"materials/{asset_id}.mat.json"
    fields = dict(texture=f"{folder}/{names['diff']}", normalMap=f"{folder}/{names['nor_gl']}",
                  ormMap=f"{folder}/{names['arm']}", occlusionStrength=1.0, metallic=1.0, roughness=1.0,
                  tilingU=tiling, tilingV=tiling, triplanar=triplanar)
    fields.update(material)  # the ARM map's channels are multiplied by these factors
    b.material(path, **fields)
    return path


def place(b, res, name, pos, yaw=0.0, rot=None, scale=1.0, parent=None, tags=None):
    """Queues an instance of an imported model: a prefab instance for multi-material models,
    otherwise a mesh entity with the imported material."""
    rotation = list(rot) if rot else [0, yaw, 0]
    if res.get("prefab"):
        args = dict(prefab=res["prefab"], name=name, position=[round(v, 3) for v in pos], rotation=rotation, scale=scale)
        if parent:
            args["parent"] = parent
        b.op("prefab_instantiate", **args)
    else:
        extra = {"material": res["material"]} if res.get("material") else {}
        b.e(name, res["mesh"], pos=pos, rot=rotation, scale=scale, parent=parent, tags=tags, **extra)
    return name


def surface(b, x, z, top=120.0, exclude=None):
    """Height of whatever is below (x, z) — call after the terrain has been flushed."""
    args = dict(origin=[x, top, z], direction=[0, -1, 0])
    if exclude:
        args["exclude"] = exclude
    r = b.sky.call("raycast", **args)
    hit = r.get("hit") if isinstance(r, dict) else None
    if isinstance(r, dict) and "point" in r:
        return r["point"][1]
    if hit and "point" in hit:
        return hit["point"][1]
    return 0.0


def aim_sun(b, sky, azimuth, intensity=1.0):
    """Turns the HDRI so its sun sits at `azimuth` (degrees) and points the scene's sun at it."""
    def az_for(rot):
        b.call("environment_update", skyMode="hdri", hdri=sky, hdriRotation=rot, hdriIntensity=intensity, align_sun_to_hdri=True)
        return b.call("environment_update")["sunAzimuth"]
    base = az_for(0.0)
    best = None
    for sign in (1, -1):
        rot = sign * (azimuth - base)
        got = az_for(rot)
        err = abs((got - azimuth + 180) % 360 - 180)
        if best is None or err < best[0]:
            best = (err, rot)
    return az_for(best[1])
