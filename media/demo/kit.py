"""Building blocks for the showcase games.

A `Studio` gives each crew member their own MCP connection to the engine. When attached
to the live editor, every edit shows up attributed (`mcp:Cirro`, `mcp:Aurora`, ...) in the
history and activity feed, and the recorder films the scene as it is built. Headless, all
crew members share one engine process (used for fast look-dev iterations).
"""
import math
import os
import time

from sky import Sky

CREW_ROLES = {
    "Nimbus": "Creative Director",
    "Cirro": "Level Designer",
    "Aurora": "Lighting Artist",
    "Stratus": "Gameplay Programmer",
    "Pixel": "Asset Artist",
}


class Studio:
    def __init__(self, project, attach=False, pace=0.0, style=None, log=None):
        self.project = os.path.abspath(project)
        self.style = style or {}
        self.log = open(log, "a") if log else None  # edit log (wall clock) for the timelapse
        self.attach = attach
        self.pace = pace  # seconds to pause after each step (so a timelapse can see progress)
        self._agents = {}
        self._shared = None if attach else Sky(project=self.project)

    def agent(self, name):
        if not self.attach:
            return Builder(self._shared, name, self)
        if name not in self._agents:
            self._agents[name] = Sky(attach=True, name=name)
        return Builder(self._agents[name], name, self)

    def record(self, who, label, kind):
        if self.log:
            import json
            self.log.write(json.dumps({"ts": time.time(), "actor": who, "label": label, "kind": kind}) + "\n")
            self.log.flush()

    def close(self):
        if self.log:
            self.log.close()
        for s in self._agents.values():
            s.close()
        if self._shared:
            self._shared.close()


class Builder:
    """Collects entity-creation ops and sends them as one attributed, undoable batch."""

    def __init__(self, sky, who, studio):
        self.sky, self.who, self.studio = sky, who, studio
        self.ops = []
        self.style = dict(studio.style)  # default surface fields for every mesh (e.g. toon + outline)

    # --- entities ------------------------------------------------------------------------
    def e(self, name, mesh=None, color=None, pos=None, rot=None, scale=None, parent=None, tags=None,
          vars=None, metallic=None, roughness=None, emissive=None, unlit=None, billboard=None, texture=None,
          material=None, light=None, camera=None, **surface):
        if isinstance(color, tuple) and pos is None:
            raise ValueError(f"{name}: a tuple was passed as color — did you mean pos=?")
        a = {"name": name}
        if parent is not None:
            a["parent"] = parent
        if mesh:
            a["mesh"] = mesh
        if color is not None:
            a["color"] = hexa(color) if isinstance(color, (list, tuple)) else color
        if pos is not None:
            a["position"] = [round(v, 4) for v in pos]
        if rot is not None:
            a["rotation"] = [round(v, 3) for v in rot]
        if scale is not None:
            a["scale"] = [round(v, 4) for v in (scale if isinstance(scale, (list, tuple)) else (scale,) * 3)]
        if tags:
            a["tags"] = tags
        if vars:
            a["vars"] = vars
        comps = {}
        m = dict(self.style) if mesh else {}
        m.update({k: v for k, v in (("metallic", metallic), ("roughness", roughness), ("emissive", emissive),
                                    ("unlit", unlit), ("billboard", billboard), ("texture", texture),
                                    ("material", material)) if v is not None})
        m.update(surface)
        if m:
            comps["mesh"] = m
        if light:
            comps["light"] = light
        if camera:
            comps["camera"] = camera
        if comps:
            a["components"] = comps
        self.ops.append(("entity_create", a))
        return name

    def light(self, name, pos, color, intensity, range_=8.0, kind="point", parent=None, rot=None, spot=35.0, tags=None):
        return self.e(name, pos=pos, parent=parent, rot=rot, tags=tags,
                      light={"kind": kind, "color": color, "intensity": intensity, "range": range_, "spotAngle": spot})

    def op(self, tool, **args):
        self.ops.append((tool, args))

    def behave(self, entity, name, intent, source):
        self.ops.append(("behavior_set", {"entity": entity, "name": name, "intent": intent, "source": source}))

    def flush(self, label):
        if not self.ops:
            return None
        ops, self.ops = self.ops, []
        r = self.sky.call("batch", label=f"{self.who}: {label}",
                          operations=[{"tool": t, "args": a} for t, a in ops])
        self.studio.record(self.who, label, "batch")
        if self.studio.pace:
            time.sleep(self.studio.pace)
        return r

    def call(self, tool, **args):
        r = self.sky.call(tool, **args)
        if tool not in ("entity_get", "scene_query", "asset_list", "raycast", "viewport_multi"):
            label = {"scene_new": "New scene", "scatter": f"Scatter {args.get('group', '')}".strip(),
                     "texture_generate": f"Texture: {args.get('kind', '')}", "material_create": "Material",
                     "prefab_create": f"Prefab {os.path.basename(args.get('path', ''))}", "environment_update": "Lighting & sky",
                     "scene_save": "Save scene",
                     "asset_download": f"Download {os.path.basename(args.get('folder', '') or args.get('url', ''))} ({args.get('license', '')})",
                     "asset_import": f"Import {os.path.basename(args.get('path', ''))}"}.get(tool, tool)
            self.studio.record(self.who, label, tool)
        if self.studio.pace and tool not in ("entity_get", "scene_query", "asset_list", "raycast"):
            time.sleep(self.studio.pace * 0.5)
        return r

    # --- reuse ---------------------------------------------------------------------------
    def prefab(self, root, path, description, tags=(), keep=False):
        """Saves an already-built entity tree as a prefab (and removes the template)."""
        self.call("prefab_create", entity=root, path=path, description=description, tags=list(tags))
        if not keep:
            self.call("entity_delete", entity=root)

    def texture(self, kind, name, **params):
        """Procedural PBR texture set + triplanar material; returns the material path."""
        return self.call("texture_generate", kind=kind, name=f"textures/{name}", **params)["material"]

    def material(self, path, **fields):
        try:
            self.call("material_create", path=path, **fields)
        except RuntimeError as e:
            if "exists" not in str(e):
                raise
            self.call("material_update", path=path, **fields)


# --- small geometry helpers --------------------------------------------------------------
def ring(n, r, y=0.0, phase=0.0, cx=0.0, cz=0.0):
    for i in range(n):
        a = phase + 2 * math.pi * i / n
        yield i, (cx + math.cos(a) * r, y, cz + math.sin(a) * r), math.degrees(a)


def rnd(seed):
    """Deterministic pseudo-random generator (so builds are reproducible)."""
    import random
    return random.Random(seed)


def hexmix(a, b, t):
    ca = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    cb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{int(round(x + (y - x) * t)):02x}" for x, y in zip(ca, cb))


def hexa(c):
    """[r, g, b, a] in 0..1 -> "#rrggbbaa"."""
    vals = list(c) + [1.0] * (4 - len(c))
    return "#" + "".join(f"{max(0, min(255, int(round(v * 255)))):02x}" for v in vals)
