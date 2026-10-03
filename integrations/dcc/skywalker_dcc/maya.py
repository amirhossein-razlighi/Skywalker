"""Maya toolkit (runs inside mayapy). NOT TESTED: written against Autodesk's documented
maya.cmds API; Skywalker's test-suite only exercises Blender. Report problems with the exact
mayapy version.

    from skywalker_dcc import maya as M
    M.export_fbx(sky.out_path("crate.fbx"))      # then dcc_convert / dcc_run_script import:true
"""

from maya import cmds

import skywalker_dcc as sky  # noqa: F401


def _selection_kwargs(selection):
    if selection:
        cmds.select(selection, replace=True)
        return {"exportSelected": True}
    return {"exportAll": True}


def export_fbx(path, selection=None):
    """Export the scene (or `selection`, a list of node names) as binary FBX."""
    cmds.loadPlugin("fbxmaya", quiet=True)
    cmds.file(path, force=True, options="v=0;", type="FBX export", **_selection_kwargs(selection))
    return path


def export_obj(path, selection=None):
    cmds.loadPlugin("objExport", quiet=True)
    cmds.file(path, force=True, type="OBJexport",
              options="groups=1;ptgroups=1;materials=1;smoothing=1;normals=1", **_selection_kwargs(selection))
    return path


def import_file(path):
    """Import FBX/OBJ/Maya files into the current scene."""
    cmds.file(path, i=True, ignoreVersion=True, mergeNamespacesOnClash=False, options="v=0;")


def mesh_stats():
    meshes = cmds.ls(type="mesh", noIntermediate=True) or []
    tris = sum(cmds.polyEvaluate(m, triangle=True) for m in meshes) if meshes else 0
    return {"meshes": len(meshes), "triangles": tris}
