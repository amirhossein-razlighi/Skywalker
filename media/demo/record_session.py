#!/usr/bin/env python3
"""Timelapse recorder for a live Skywalker editor session.

Connects to the editor's MCP socket (like any agent), captures the viewport at a fixed
interval, and logs history entries (who changed what) with timestamps. Used to film real
agents building a scene.

  python3 record_session.py OUT_DIR [--interval 0.5] [--size 1280x720]
Stop with Ctrl-C (or create OUT_DIR/STOP).
"""
import json, os, socket, sys, time

out = sys.argv[1]
interval = float(sys.argv[sys.argv.index("--interval") + 1]) if "--interval" in sys.argv else 0.5
w, h = (sys.argv[sys.argv.index("--size") + 1] if "--size" in sys.argv else "1280x720").split("x")
os.makedirs(os.path.join(out, "frames"), exist_ok=True)

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(os.path.expanduser("~/.skywalker/editor.sock"))
buf = b""
next_id = 0

def rpc(method, params):
    global buf, next_id
    next_id += 1
    sock.sendall((json.dumps({"jsonrpc": "2.0", "id": next_id, "method": method, "params": params}) + "\n").encode())
    while True:
        while b"\n" not in buf:
            chunk = sock.recv(1 << 20)
            if not chunk:
                raise SystemExit("socket closed")
            buf += chunk
        line, buf = buf.split(b"\n", 1)
        msg = json.loads(line)
        if msg.get("id") == next_id:
            return msg.get("result", {})

rpc("initialize", {"protocolVersion": "2025-11-25", "clientInfo": {"name": "recorder"}})
log = open(os.path.join(out, "events.jsonl"), "a")
seen = 0
frame = int(sys.argv[sys.argv.index("--start") + 1]) if "--start" in sys.argv else 0
t0 = time.time()
while not os.path.exists(os.path.join(out, "STOP")):
    start = time.time()
    path = os.path.join(out, "frames", f"{frame:05d}.png")
    rpc("tools/call", {"name": "viewport_capture", "arguments": {
        "width": int(w), "height": int(h), "annotate": False, "overlays": False,
        "include_image": False, "save_path": path}})
    # Read-only: which entities exist at this frame (mapped to attributed history later).
    ov = rpc("tools/call", {"name": "scene_overview", "arguments": {"max_entities": 5000}})
    ids = [e["id"] for e in ov.get("structuredContent", {}).get("entities", [])]
    log.write(json.dumps({"t": round(start - t0, 2), "ts": start, "frame": frame, "ids": ids}) + "\n")
    log.flush()
    frame += 1
    time.sleep(max(0.0, interval - (time.time() - start)))
print(f"recorded {frame} frames", file=sys.stderr)
