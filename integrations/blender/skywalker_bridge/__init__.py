# SPDX-License-Identifier: GPL-3.0-or-later
# (code that runs inside Blender and uses its Python API follows Blender's license, see docs/LICENSING.md)
"""Skywalker Bridge: lets Skywalker's design agents work inside this Blender session.

* Starts a loopback-only server (token protected, see server.py) that the engine uses to run
  Python here, read the selection and exchange models.
* Adds a "Skywalker" tab to the 3D viewport sidebar (N): start/stop the bridge and send the
  current selection to the Skywalker editor with one click.

The add-on is installed with Skywalker's `dcc_install_addon` tool, or by copying this folder
into Blender's add-ons directory.
"""

bl_info = {
    "name": "Skywalker Bridge",
    "author": "Skywalker",
    "version": (1, 0, 0),
    "blender": (3, 2, 0),
    "location": "3D Viewport > Sidebar (N) > Skywalker",
    "description": "Live link to the Skywalker game engine: agents run scripts here, models flow both ways",
    "category": "Import-Export",
}

import os
import sys

import bpy

# The helper library ships inside this package when installed by Skywalker; when the add-on
# is used straight from the source tree it lives two folders up (integrations/dcc).
try:
    import skywalker_dcc  # noqa: F401
except ImportError:
    _here = os.path.dirname(os.path.abspath(__file__))
    for _candidate in (_here, os.path.join(_here, "..", "..", "dcc")):
        if os.path.isdir(os.path.join(_candidate, "skywalker_dcc")):
            sys.path.insert(0, os.path.abspath(_candidate))
            break

from . import engine_link, server  # noqa: E402

BRIDGE = server.BRIDGE


class SkywalkerPreferences(bpy.types.AddonPreferences):
    bl_idname = __name__

    auto_start: bpy.props.BoolProperty(
        name="Start the bridge when Blender starts",
        description="Allow Skywalker to run Python in this Blender as soon as it opens",
        default=False)
    socket_path: bpy.props.StringProperty(
        name="Editor socket",
        description="Unix socket of the running Skywalker editor (blank = ~/.skywalker/editor.sock)",
        subtype="FILE_PATH", default="")

    def draw(self, context):
        col = self.layout.column()
        col.prop(self, "auto_start")
        col.prop(self, "socket_path")
        col.label(text="The bridge listens on 127.0.0.1 only and requires a random token", icon="LOCKED")


def _prefs(context=None):
    addon = (context or bpy.context).preferences.addons.get(__name__)
    return addon.preferences if addon else None


class SKYWALKER_OT_start(bpy.types.Operator):
    bl_idname = "skywalker.start_bridge"
    bl_label = "Start Bridge"
    bl_description = "Let Skywalker agents run Python in this Blender and exchange models with it"

    def execute(self, context):
        try:
            port = BRIDGE.start(mode="ui")
        except OSError as e:
            self.report({"ERROR"}, "Could not start the bridge: %s" % e)
            return {"CANCELLED"}
        self.report({"INFO"}, "Skywalker bridge listening on 127.0.0.1:%d" % port)
        return {"FINISHED"}


class SKYWALKER_OT_stop(bpy.types.Operator):
    bl_idname = "skywalker.stop_bridge"
    bl_label = "Stop Bridge"
    bl_description = "Stop accepting commands from Skywalker"

    def execute(self, context):
        BRIDGE.stop()
        return {"FINISHED"}


class SKYWALKER_OT_send(bpy.types.Operator):
    bl_idname = "skywalker.send_selection"
    bl_label = "Send Selection to Skywalker"
    bl_description = "Export the selected objects as glTF and place them in the running Skywalker editor"

    def execute(self, context):
        import tempfile
        from skywalker_dcc import blender as B

        objs = [o for o in context.selected_objects if o.type in B.MESH_LIKE]
        if not objs:
            self.report({"WARNING"}, "Select at least one mesh object")
            return {"CANCELLED"}
        name = context.active_object.name if context.active_object else objs[0].name
        tmp = os.path.join(tempfile.gettempdir(), "skywalker-send-%d" % os.getpid())
        os.makedirs(tmp, exist_ok=True)
        path = os.path.join(tmp, "%s.glb" % name)
        try:
            B.export_glb(path, objs)
            prefs = _prefs(context)
            sock = prefs.socket_path if prefs and prefs.socket_path else None
            result = engine_link.call_tool("dcc_receive", {"file": path, "name": name}, socket_path=sock)
        except (engine_link.EngineError, RuntimeError, OSError) as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        finally:
            try:
                os.remove(path)
            except OSError:
                pass
        self.report({"INFO"}, "Sent %s to Skywalker (%s)" % (name, result.get("mesh", "ok")))
        return {"FINISHED"}


class SKYWALKER_PT_panel(bpy.types.Panel):
    bl_label = "Skywalker"
    bl_idname = "SKYWALKER_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Skywalker"

    def draw(self, context):
        layout = self.layout
        if BRIDGE.running:
            layout.label(text="Bridge running on port %d" % BRIDGE.port, icon="LINKED")
            layout.operator("skywalker.stop_bridge", icon="PAUSE")
        else:
            layout.label(text="Bridge stopped", icon="UNLINKED")
            layout.operator("skywalker.start_bridge", icon="PLAY")
        layout.separator()
        layout.operator("skywalker.send_selection", icon="EXPORT")
        box = layout.box()
        box.scale_y = 0.8
        box.label(text="Agents connect with the token in")
        box.label(text="~/.skywalker/dcc/blender.json")


_classes = (SkywalkerPreferences, SKYWALKER_OT_start, SKYWALKER_OT_stop, SKYWALKER_OT_send, SKYWALKER_PT_panel)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    prefs = _prefs()
    if prefs and prefs.auto_start and not BRIDGE.running:
        try:
            BRIDGE.start(mode="headless" if bpy.app.background else "ui")
        except OSError:
            pass


def unregister():
    BRIDGE.stop()
    for c in reversed(_classes):
        try:
            bpy.utils.unregister_class(c)
        except RuntimeError:
            pass


def run_from_env():
    """Entry point used by the engine's dcc_session_start (environment: SKY_SESSION_FILE,
    SKY_SESSION_MODE, SKY_OPEN, SKY_PARENT_PID). In headless mode this call serves requests
    until the engine asks it to stop, then quits Blender."""
    headless = os.environ.get("SKY_SESSION_MODE") == "headless" or bpy.app.background
    try:
        register()
    except Exception:  # noqa: BLE001 - the panel is a convenience; the bridge must still run
        pass
    from skywalker_dcc import blender as B
    if headless:
        B.reset_scene()  # agents start from an empty scene, not Blender's default cube
    opened = os.environ.get("SKY_OPEN", "")
    if opened and os.path.exists(opened):
        B.import_file(opened)
    BRIDGE.start(session_file=os.environ.get("SKY_SESSION_FILE", ""), mode="headless" if headless else "ui")
    if headless:
        import time
        parent = int(os.environ.get("SKY_PARENT_PID") or 0)
        BRIDGE.serve_forever(parent_pid=parent)
        time.sleep(0.3)  # let the shutdown response leave the socket
        sys.exit(0)
