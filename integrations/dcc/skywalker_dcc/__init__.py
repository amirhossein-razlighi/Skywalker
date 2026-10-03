"""Skywalker DCC helper library.

Imported by every script Skywalker runs inside a design app (Blender, Maya, Houdini, 3ds Max).
It exposes the job environment and a few app-independent helpers; the app-specific toolkits
live in submodules:

    import skywalker_dcc as sky
    sky.PROJECT                  # absolute path of the Skywalker project
    sky.OUT                      # directory whose new files Skywalker picks up (and can import)
    sky.out_path("tower.glb")    # a path inside OUT (creates folders, refuses to escape OUT)
    sky.log("hello")             # shows up in the tool result's log
    sky.result(vertices=1234)    # structured values returned to the agent

    from skywalker_dcc import blender, procedural      # inside Blender
    from skywalker_dcc import maya, houdini, max3ds     # inside the other apps

Scripts run through dcc_run_script already have `sky` in scope.
"""

import json
import os

__version__ = "1.0"

APP = os.environ.get("SKY_APP", "")
PROJECT = os.environ.get("SKY_PROJECT", "")
OUT = os.environ.get("SKY_OUT", "")
INPUT = os.environ.get("SKY_INPUT", "")
JOB = os.environ.get("SKY_JOB", "")
TASK = os.environ.get("SKY_TASK", "script")


def _load_args():
    try:
        value = json.loads(os.environ.get("SKY_ARGS") or "[]")
        return value if isinstance(value, list) else []
    except ValueError:
        return []


ARGS = _load_args()

_result = {}
_warnings = []


def log(*args):
    """Print a line that is easy to spot in the captured output."""
    print("[sky]", *args, flush=True)


def warn(message):
    """Record a warning that is returned to the agent alongside the result."""
    _warnings.append(str(message))
    print("[sky][warn]", message, flush=True)


def out_path(*parts):
    """Absolute path inside SKY_OUT, creating parent folders. Refuses paths that escape OUT."""
    if not OUT:
        raise RuntimeError("SKY_OUT is not set: this script was not started by Skywalker")
    root = os.path.realpath(OUT)
    path = os.path.realpath(os.path.join(root, *parts))
    if path != root and not path.startswith(root + os.sep):
        raise ValueError("path escapes the output folder: %r" % (parts,))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def project_path(*parts):
    """Absolute path inside the Skywalker project (read access to existing assets)."""
    if not PROJECT:
        raise RuntimeError("SKY_PROJECT is not set: this script was not started by Skywalker")
    root = os.path.realpath(PROJECT)
    path = os.path.realpath(os.path.join(root, *parts))
    if path != root and not path.startswith(root + os.sep):
        raise ValueError("path escapes the project: %r" % (parts,))
    return path


def result(**fields):
    """Merge fields into the structured result returned to the agent (written immediately, so
    partial results survive a crash later in the script)."""
    _result.update(fields)
    _write_result()
    return _result


def _write_result():
    path = os.environ.get("SKY_RESULT", "")
    if not path:
        return
    payload = dict(_result)
    if _warnings:
        payload["warnings"] = list(_warnings)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(payload, f, default=str)
    os.replace(tmp, path)


def app_module():
    """The app-specific toolkit for the running app (skywalker_dcc.blender, .maya, ...)."""
    import importlib
    name = {"blender": "blender", "maya": "maya", "houdini": "houdini", "3dsmax": "max3ds"}.get(APP)
    if not name:
        raise RuntimeError("unknown app %r" % APP)
    return importlib.import_module("skywalker_dcc." + name)
