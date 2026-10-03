"""Skywalker bridge server: a localhost TCP server inside Blender.

Protocol: newline-delimited JSON over TCP, loopback only.

    request : {"id": 1, "token": "<token from the session file>", "method": "exec", "params": {...}}
    response: {"id": 1, "ok": true, "result": {...}}
              {"id": 1, "ok": false, "error": {"type": "...", "message": "...", "trace": "..."}}

Security model: the server only listens on 127.0.0.1, every request must carry the random
token stored in the session file (~/.skywalker/dcc/blender.json, mode 0600), so only processes
running as the same user can drive Blender. The `exec` method runs arbitrary Python inside
Blender by design: that is what the bridge is for. Starting the server is an explicit user
action (the Start button, or the auto-start preference).

Threading: sockets live on helper threads; every request that touches `bpy` is queued and
executed on Blender's main thread (a bpy.app.timers callback in the UI, a manual pump loop in
headless mode).
"""

import ast
import contextlib
import hmac
import io
import json
import linecache
import os
import queue
import secrets
import socket
import threading
import time
import traceback

import bpy

MAX_LINE = 64 * 1024 * 1024
MAX_RESULT = 1024 * 1024
BRIDGE_VERSION = "1.0"


class BridgeError(Exception):
    """An error with a stable type that is reported to the engine as-is."""

    def __init__(self, type_, message):
        super().__init__(message)
        self.type = type_


class _Job:
    def __init__(self, method, params):
        self.method = method
        self.params = params
        self.done = threading.Event()
        self.response = None
        self.abandoned = False
        self.started = False
        self.lock = threading.Lock()


def _jsonable(value, depth=0):
    """Best-effort conversion of Blender values to JSON (vectors, matrices, ID blocks...)."""
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if depth > 6:
        return repr(value)
    if isinstance(value, dict):
        return {str(k): _jsonable(v, depth + 1) for k, v in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [_jsonable(v, depth + 1) for v in list(value)[:2000]]
    if hasattr(value, "to_list"):
        try:
            return _jsonable(value.to_list(), depth + 1)
        except Exception:  # noqa: BLE001
            pass
    if hasattr(value, "name") and hasattr(value, "bl_rna"):
        return {"type": type(value).__name__, "name": value.name}
    if hasattr(value, "__len__") and hasattr(value, "__getitem__"):
        try:
            return [_jsonable(v, depth + 1) for v in list(value)[:2000]]
        except Exception:  # noqa: BLE001
            pass
    return repr(value)


class Bridge:
    def __init__(self):
        self.sock = None
        self.thread = None
        self.token = ""
        self.port = 0
        self.mode = "ui"
        self.session_file = ""
        self.running = False
        self.queue = queue.Queue()
        self.last_pump = time.monotonic()
        self.scope = None
        self.stop_requested = False
        self.on_status = None  # optional callback for the UI

    # --- lifecycle --------------------------------------------------------------------------

    def start(self, port=0, session_file="", mode="ui"):
        if self.running:
            return self.port
        self.mode = mode
        self.token = secrets.token_hex(32)
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", int(port)))
        srv.listen(8)
        srv.settimeout(0.5)
        self.sock = srv
        self.port = srv.getsockname()[1]
        self.session_file = session_file or default_session_file()
        self.running = True
        self.stop_requested = False
        self._write_session_file()
        self.thread = threading.Thread(target=self._accept_loop, name="skywalker-bridge", daemon=True)
        self.thread.start()
        if mode == "ui":
            bpy.app.timers.register(self._timer, first_interval=0.05, persistent=True)
        return self.port

    def stop(self):
        if not self.running:
            return
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass
        self._remove_session_file()
        if self.thread is not None:
            self.thread.join(timeout=2.0)
        if bpy.app.timers.is_registered(self._timer):
            bpy.app.timers.unregister(self._timer)

    def _write_session_file(self):
        info = {
            "port": self.port,
            "token": self.token,
            "pid": os.getpid(),
            "version": bpy.app.version_string,
            "file": bpy.data.filepath,
            "mode": self.mode,
            "bridge": BRIDGE_VERSION,
            "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        }
        os.makedirs(os.path.dirname(self.session_file), exist_ok=True)
        tmp = self.session_file + ".tmp%d" % os.getpid()
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)  # token: owner only, from the first byte
        with os.fdopen(fd, "w") as f:
            json.dump(info, f)
        os.replace(tmp, self.session_file)

    def _remove_session_file(self):
        try:
            with open(self.session_file) as f:
                if json.load(f).get("token") == self.token:
                    os.remove(self.session_file)
        except (OSError, ValueError):
            pass

    # --- socket side (helper threads) ---------------------------------------------------------

    def _accept_loop(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        with conn:
            conn.settimeout(None)
            reader = conn.makefile("rb")
            while self.running:
                try:
                    line = reader.readline(MAX_LINE + 1)
                except OSError:
                    break
                if not line or len(line) > MAX_LINE:
                    break
                response = self._handle(line)
                try:
                    conn.sendall((json.dumps(response, default=repr) + "\n").encode("utf-8"))
                except OSError:
                    break

    def _handle(self, line):
        try:
            req = json.loads(line.decode("utf-8"))
            if not isinstance(req, dict):
                raise ValueError("request must be an object")
        except ValueError as e:
            return {"id": None, "ok": False, "error": {"type": "bad_request", "message": "not valid JSON: %s" % e}}
        rid = req.get("id")
        token = str(req.get("token", ""))
        if not hmac.compare_digest(token.encode(), self.token.encode()):
            return {"id": rid, "ok": False, "error": {"type": "unauthorized", "message": "wrong or missing token (re-read the session file)"}}
        method = str(req.get("method", ""))
        params = req.get("params") or {}
        if method == "ping":  # answered off the main thread: it reports how long Blender has been busy
            return {"id": rid, "ok": True, "result": {
                "pong": True, "pid": os.getpid(), "version": bpy.app.version_string, "mode": self.mode,
                "bridge": BRIDGE_VERSION, "main_thread_idle_s": round(time.monotonic() - self.last_pump, 2)}}
        job = _Job(method, params)
        self.queue.put(job)
        timeout = float(params.get("timeout", 120)) if isinstance(params, dict) else 120.0
        if not job.done.wait(timeout):
            with job.lock:
                job.abandoned = True
                started = job.started
            if not job.done.is_set():
                msg = ("the request was still running after %.0fs (Blender keeps going)" % timeout) if started else \
                      ("Blender's main thread did not pick the request up within %.0fs; it is busy or showing a dialog" % timeout)
                return {"id": rid, "ok": False, "error": {"type": "timeout", "message": msg}}
        resp = job.response
        resp["id"] = rid
        return resp

    # --- main thread side ---------------------------------------------------------------------

    def _timer(self):
        self.pump()
        return 0.05 if self.running else None

    def pump(self, budget=0.04):
        """Run queued requests on the calling (main) thread. Returns after `budget` seconds."""
        self.last_pump = time.monotonic()
        t0 = time.monotonic()
        while time.monotonic() - t0 < budget:
            try:
                job = self.queue.get_nowait()
            except queue.Empty:
                break
            with job.lock:
                if job.abandoned:
                    continue
                job.started = True
            try:
                result = self._dispatch(job.method, job.params)
                job.response = {"ok": True, "result": result}
            except BridgeError as e:
                job.response = {"ok": False, "error": {"type": e.type, "message": str(e)}}
            except Exception as e:  # noqa: BLE001 - the engine gets the traceback, Blender keeps running
                job.response = {"ok": False, "error": {"type": type(e).__name__, "message": str(e),
                                                       "trace": _user_trace(e)}}
            job.done.set()
        self.last_pump = time.monotonic()

    def serve_forever(self, parent_pid=0):
        """Headless mode: there is no event loop, so pump here until asked to stop."""
        while self.running and not self.stop_requested:
            self.pump(0.2)
            time.sleep(0.02)
            if parent_pid and not _alive(parent_pid):
                break
        self.stop()

    def _dispatch(self, method, params):
        fn = getattr(self, "m_" + method, None)
        if fn is None:
            raise BridgeError("unknown_method", "unknown method %r" % method)
        return fn(params)

    # --- methods ---------------------------------------------------------------------------------

    def _scope(self, reset=False):
        if self.scope is None or reset:
            import bmesh
            scope = {"bpy": bpy, "bmesh": bmesh, "__name__": "__skywalker__"}
            try:
                import skywalker_dcc as sky
                from skywalker_dcc import blender as B
                scope.update(sky=sky, B=B)
            except ImportError:
                pass
            self.scope = scope
        return self.scope

    def m_status(self, params):
        scene = bpy.context.scene
        sel = [o.name for o in scene.objects if o.select_get()]
        active = bpy.context.view_layer.objects.active
        return {
            "version": bpy.app.version_string, "file": bpy.data.filepath, "mode": self.mode, "scene": scene.name,
            "objects": len(scene.objects), "meshes": len([o for o in scene.objects if o.type == "MESH"]),
            "selected": sel, "active": active.name if active else None, "dirty": bpy.data.is_dirty,
            "unit_scale": scene.unit_settings.scale_length, "bridge": BRIDGE_VERSION,
        }

    def m_exec(self, params):
        code = params.get("code")
        if not isinstance(code, str) or not code.strip():
            raise BridgeError("bad_request", "exec needs a non-empty 'code' string")
        scope = self._scope(bool(params.get("reset")))
        # So tracebacks can show the offending line of the agent's code.
        linecache.cache["<skywalker>"] = (len(code), None, code.splitlines(True), "<skywalker>")
        out, err = io.StringIO(), io.StringIO()
        value = None
        t0 = time.monotonic()
        scope["result"] = None
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            tree = ast.parse(code, "<skywalker>", "exec")
            last = None
            if tree.body and isinstance(tree.body[-1], ast.Expr):
                last = ast.Expression(tree.body.pop().value)
            exec(compile(tree, "<skywalker>", "exec"), scope)
            if last is not None:
                value = eval(compile(last, "<skywalker>", "eval"), scope)
            elif scope.get("result") is not None:
                value = scope["result"]
        if not bpy.app.background and params.get("undo", True):
            try:
                bpy.ops.ed.undo_push(message="Skywalker: exec")  # Ctrl+Z undoes an agent's change
            except Exception:  # noqa: BLE001
                pass
        encoded = _jsonable(value)
        if len(json.dumps(encoded, default=repr)) > MAX_RESULT:
            encoded = repr(value)[:MAX_RESULT]
        return {"stdout": out.getvalue()[-200000:], "stderr": err.getvalue()[-50000:], "result": encoded,
                "seconds": round(time.monotonic() - t0, 3)}

    def m_selection(self, params):
        view = bpy.context.view_layer
        names = params.get("names")
        objs = [bpy.data.objects[n] for n in names if n in bpy.data.objects] if names else \
               [o for o in bpy.context.scene.objects if o.select_get(view_layer=view)]
        out = []
        for o in objs:
            info = {"name": o.name, "type": o.type, "location": [round(v, 4) for v in o.location],
                    "dimensions": [round(v, 4) for v in o.dimensions], "parent": o.parent.name if o.parent else None,
                    "collections": [c.name for c in o.users_collection]}
            if o.type == "MESH":
                info.update(vertices=len(o.data.vertices), polygons=len(o.data.polygons),
                            materials=[s.material.name for s in o.material_slots if s.material])
            out.append(info)
        active = view.objects.active
        return {"objects": out, "active": active.name if active else None}

    def m_export_selection(self, params):
        """Export the selection (or `names`) to a .glb the engine can import."""
        from skywalker_dcc import blender as B
        path = params.get("path")
        if not path:
            raise BridgeError("bad_request", "export_selection needs 'path'")
        names = params.get("names")
        view = bpy.context.view_layer
        if names:
            objs = [bpy.data.objects[n] for n in names if n in bpy.data.objects]
            missing = [n for n in names if n not in bpy.data.objects]
            if missing:
                raise BridgeError("not_found", "no object(s) named %s" % ", ".join(missing))
        else:
            objs = [o for o in bpy.context.scene.objects if o.select_get(view_layer=view)]
        # Children come along: exporting a parent should not drop its parts.
        pool = []
        for o in objs:
            for c in [o] + list(o.children_recursive):
                if c not in pool:
                    pool.append(c)
        objs = [o for o in pool if o.type in B.MESH_LIKE]
        if not objs:
            raise BridgeError("nothing_selected", "no mesh objects are selected in Blender (select some, or pass names)")
        origin = params.get("origin", "keep")
        saved = {}
        if origin in ("bottom_center", "center"):
            from mathutils import Matrix, Vector
            lo, hi = B.world_bounds(objs)
            cz = lo.z if origin == "bottom_center" else (lo.z + hi.z) / 2
            offset = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, cz))
            names_set = {o.name for o in objs}
            for o in objs:
                if o.parent is None or o.parent.name not in names_set:
                    saved[o.name] = o.matrix_world.copy()
                    o.matrix_world = Matrix.Translation(-offset) @ o.matrix_world
        try:
            B.export_glb(path, objs, apply_modifiers=params.get("apply_modifiers", True),
                         animations=params.get("animations", False))
            stats = B.stats(objs)
        finally:
            for name, m in saved.items():
                if name in bpy.data.objects:
                    bpy.data.objects[name].matrix_world = m
        return {"path": path, "objects": [o.name for o in objs], "stats": stats}

    def m_import_file(self, params):
        from skywalker_dcc import blender as B
        path = params.get("path")
        if not path or not os.path.exists(path):
            raise BridgeError("not_found", "no such file: %r" % path)
        new = B.import_file(path, scale=float(params.get("scale", 1.0)))
        if not bpy.app.background:
            B.select(new)
            for window in bpy.context.window_manager.windows:
                for area in window.screen.areas:
                    if area.type == "VIEW_3D":
                        try:
                            with bpy.context.temp_override(window=window, area=area,
                                                           region=next(r for r in area.regions if r.type == "WINDOW")):
                                bpy.ops.view3d.view_selected()
                        except Exception:  # noqa: BLE001
                            pass
                        break
        return {"objects": [o.name for o in new]}

    def m_save_copy(self, params):
        path = params.get("path")
        if not path:
            raise BridgeError("bad_request", "save_copy needs 'path'")
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=path, copy=True)
        return {"path": path}

    def m_shutdown(self, params):
        """Stop serving. A headless Blender exits; a window the user is working in stays open."""
        self.stop_requested = True
        if self.mode == "ui":
            bpy.app.timers.register(self.stop, first_interval=0.1)
        return {"stopping": True, "mode": self.mode}


def _user_trace(exc):
    """The traceback of an agent's code without the bridge's own frames."""
    frames = [f for f in traceback.extract_tb(exc.__traceback__) if f.filename != __file__]
    if not frames:
        return "".join(traceback.format_exception_only(type(exc), exc))
    lines = ["Traceback (most recent call last):\n"]
    lines += traceback.format_list(frames)
    lines += traceback.format_exception_only(type(exc), exc)
    return "".join(lines)


def _alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except OSError:
        return False


def default_session_file():
    return os.path.join(os.path.expanduser("~"), ".skywalker", "dcc", "blender.json")


BRIDGE = Bridge()
