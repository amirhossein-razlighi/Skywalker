"""Houdini toolkit (runs inside hython). NOT TESTED: written against SideFX's documented hou
API; Skywalker's test-suite only exercises Blender.

    from skywalker_dcc import houdini as H
    H.export_geometry("/obj/geo1/OUT", sky.out_path("rock.obj"))

Prefer OBJ/PLY/STL/BGEO for single meshes, FBX for hierarchies/materials, then bring the
result to the engine with dcc_convert (Blender turns it into glTF with materials).
"""

import hou

import skywalker_dcc as sky  # noqa: F401


def export_geometry(node_path, path):
    """Save the cooked geometry of a SOP node; the extension picks the format (.obj .ply .stl .bgeo .abc)."""
    node = hou.node(node_path)
    if node is None:
        raise ValueError("no node at %s" % node_path)
    node.geometry().saveToFile(path)
    return path


def export_fbx(path, start_node="/obj"):
    """Export with a Filmbox FBX ROP. `start_node` is the network whose objects are exported."""
    out = hou.node("/out")
    rop = out.createNode("filmboxfbx")
    try:
        rop.parm("sopoutput").set(path)
        rop.parm("startnode").set(start_node)
        rop.render()
    finally:
        rop.destroy()
    return path
