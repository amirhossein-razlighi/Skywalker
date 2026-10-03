"""Entry point executed inside the design app (never imported by user scripts).

The engine starts the app with a two-line stub that puts this library on sys.path and calls
main(). main() prepares the app (Maya standalone, scene loading), runs the requested task and
turns failures into a non-zero exit code, so the engine always knows whether the run succeeded.
"""

import json
import os
import runpy
import sys
import traceback

import skywalker_dcc as sky


def _read_params():
    path = os.environ.get("SKY_PARAMS", "")
    if not path or not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f)


def _prepare_app():
    app = sky.APP
    if app == "maya":
        import maya.standalone  # noqa: F401  (only importable inside mayapy)
        maya.standalone.initialize(name="python")
        if sky.INPUT and sky.INPUT.lower().endswith((".ma", ".mb")):
            from maya import cmds
            cmds.file(sky.INPUT, open=True, force=True, ignoreVersion=True)
    elif app == "houdini":
        if sky.INPUT and sky.INPUT.lower().endswith((".hip", ".hipnc", ".hiplc")):
            import hou
            hou.hipFile.load(sky.INPUT, suppress_save_prompt=True, ignore_load_warnings=True)
    # Blender opens a .blend itself (it is on the command line); 3ds Max opens -sceneFile.


def _shutdown_app():
    if sky.APP == "maya":
        try:
            import maya.standalone
            maya.standalone.uninitialize()
        except Exception:  # noqa: BLE001
            pass


def _run_task():
    task = sky.TASK
    params = _read_params()
    if task == "script":
        script = os.environ["SKY_SCRIPT"]
        runpy.run_path(script, init_globals={"sky": sky}, run_name="__main__")
        return
    from skywalker_dcc import tasks
    getattr(tasks, task)(params)


def main():
    code = 0
    try:
        _prepare_app()
        _run_task()
    except SystemExit as e:  # a script calling sys.exit() decides the exit code
        code = e.code if isinstance(e.code, int) else (0 if e.code is None else 1)
    except BaseException:  # noqa: BLE001
        traceback.print_exc()
        code = 1
    finally:
        _shutdown_app()
        sys.stdout.flush()
        sys.stderr.flush()
    if code:
        sys.exit(code)
