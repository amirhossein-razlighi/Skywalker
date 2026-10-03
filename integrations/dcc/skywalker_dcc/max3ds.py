"""3ds Max toolkit (runs inside 3dsmaxbatch, Windows only). NOT TESTED: written against
Autodesk's documented pymxs API; Skywalker's test-suite only exercises Blender.

    from skywalker_dcc import max3ds as X
    X.export_fbx(sky.out_path("crate.fbx"))
"""

from pymxs import runtime as rt

import skywalker_dcc as sky  # noqa: F401


def export_fbx(path, selected_only=False):
    rt.exportFile(path, rt.name("noPrompt"), selectedOnly=selected_only, using=rt.FBXEXP)
    return path


def export_obj(path, selected_only=False):
    rt.exportFile(path, rt.name("noPrompt"), selectedOnly=selected_only, using=rt.ObjExp)
    return path


def import_file(path):
    rt.importFile(path, rt.name("noPrompt"))
